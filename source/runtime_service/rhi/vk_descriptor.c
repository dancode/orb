/*==============================================================================================

    vulkan/vk_descriptor.c -- Global bindless descriptor set + shared pipeline layout.

    Architecture
    ------------
    One VkDescriptorSetLayout (set 0) is shared by all pipelines:
        Binding 0: VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE   x VK_MAX_BINDLESS_TEXTURES
        Binding 1: VK_DESCRIPTOR_TYPE_SAMPLER         x VK_MAX_BINDLESS_SAMPLERS
    
    Each binding uses VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT and
    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT (core in VK 1.2 via descriptor indexing).

    Resource indices (u32) are passed to shaders via push constants:
        struct PushConstants { u32 texture_idx; u32 sampler_idx; ... };

    This eliminates per-draw descriptor binding overhead.  The render layer and game code
    call register_texture / register_sampler to get stable per-resource indices, then
    pack those indices into the push constant struct.

    VkPipelineLayout  (vk.pipeline_layout):
        set 0    = bindless layout above
        push constants = one range covering all stages, size RHI_MAX_PUSH_CONST_SIZE

==============================================================================================*/

/* Free index stacks for bindless slots.
   head == 0 means the stack is empty (index 0 is reserved as "invalid"). */
typedef struct vk_bindless_free_s
{
    u32  stack[ VK_MAX_BINDLESS_TEXTURES ];  /* conservative; samplers use a smaller stack */
    u32  top;

} vk_bindless_free_t;

static vk_bindless_free_t  g_tex_free;
static vk_bindless_free_t  g_samp_free;

static void
vk_bindless_free_init( vk_bindless_free_t* pool, u32 count )
{
    /* Pre-fill the free stack in reverse so index 1 is returned first.
       Index 0 is reserved (invalid). */
    pool->top = 0;
    for ( u32 i = count; i >= 1; --i )
        pool->stack[ pool->top++ ] = i;
}

static u32
vk_bindless_alloc( vk_bindless_free_t* pool )
{
    if ( pool->top == 0 )
        return 0;   /* 0 == invalid */
    return pool->stack[ --pool->top ];
}

static void
vk_bindless_free( vk_bindless_free_t* pool, u32 idx )
{
    if ( idx == 0 )
        return;
    ORB_ASSERT( pool->top < ARRAY_COUNT( pool->stack ) );
    pool->stack[ pool->top++ ] = idx;
}

/*==============================================================================================
    Init / shutdown  (called from vk_device.c)
==============================================================================================*/

static bool
vk_descriptor_init( void )
{
    vk_bindless_free_init( &g_tex_free,  VK_MAX_BINDLESS_TEXTURES );
    vk_bindless_free_init( &g_samp_free, VK_MAX_BINDLESS_SAMPLERS );

    /* --- Descriptor set layout --- */

    /* Both bindings need PARTIALLY_BOUND (slots may be empty) and UPDATE_AFTER_BIND
       (CPU can write new slots while GPU reads other slots in the same array). */
    VkDescriptorBindingFlags binding_flags[ 2 ] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
    };

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci = { 0 };
    flags_ci.sType                                       = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount                                = 2;
    flags_ci.pBindingFlags                               = binding_flags;

    VkDescriptorSetLayoutBinding bindings[ 2 ] = { 0 };

    /* Binding 0: sampled images (texture array) */
    bindings[ 0 ].binding         = 0;
    bindings[ 0 ].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[ 0 ].descriptorCount = VK_MAX_BINDLESS_TEXTURES;
    bindings[ 0 ].stageFlags      = VK_SHADER_STAGE_ALL;

    /* Binding 1: samplers */
    bindings[ 1 ].binding         = 1;
    bindings[ 1 ].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[ 1 ].descriptorCount = VK_MAX_BINDLESS_SAMPLERS;
    bindings[ 1 ].stageFlags      = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo layout_ci = { 0 };
    layout_ci.sType                           = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext                           = &flags_ci;
    layout_ci.flags                           = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount                    = 2;
    layout_ci.pBindings                       = bindings;

    VkResult r = vkCreateDescriptorSetLayout( vk.device, &layout_ci, vk.alloc_cb, &vk.bindless_layout );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "descriptor_init: vkCreateDescriptorSetLayout: %s", string_VkResult( r ) );
        return false;
    }

    /* --- Descriptor pool --- */

    VkDescriptorPoolSize pool_sizes[ 2 ] = { 0 };
    pool_sizes[ 0 ].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[ 0 ].descriptorCount = VK_MAX_BINDLESS_TEXTURES;
    pool_sizes[ 1 ].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[ 1 ].descriptorCount = VK_MAX_BINDLESS_SAMPLERS;

    VkDescriptorPoolCreateInfo pool_ci = { 0 };
    pool_ci.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags                      = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets                    = 1;
    pool_ci.poolSizeCount              = 2;
    pool_ci.pPoolSizes                 = pool_sizes;

    r = vkCreateDescriptorPool( vk.device, &pool_ci, vk.alloc_cb, &vk.bindless_pool );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "descriptor_init: vkCreateDescriptorPool: %s", string_VkResult( r ) );
        goto fail_after_layout;
    }

    /* --- Allocate the one bindless set --- */

    VkDescriptorSetAllocateInfo alloc_info = { 0 };
    alloc_info.sType                       = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool              = vk.bindless_pool;
    alloc_info.descriptorSetCount          = 1;
    alloc_info.pSetLayouts                 = &vk.bindless_layout;

    r = vkAllocateDescriptorSets( vk.device, &alloc_info, &vk.bindless_set );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "descriptor_init: vkAllocateDescriptorSets: %s", string_VkResult( r ) );
        goto fail_after_pool;
    }

    /* --- Pipeline layout: set 0 (bindless) + push constants --- */

    VkPushConstantRange pc_range = { 0 };
    pc_range.stageFlags          = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    pc_range.offset              = 0;
    pc_range.size                = RHI_MAX_PUSH_CONST_SIZE;

    VkPipelineLayoutCreateInfo pl_ci = { 0 };
    pl_ci.sType                      = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount             = 1;
    pl_ci.pSetLayouts                = &vk.bindless_layout;
    pl_ci.pushConstantRangeCount     = 1;
    pl_ci.pPushConstantRanges        = &pc_range;

    r = vkCreatePipelineLayout( vk.device, &pl_ci, vk.alloc_cb, &vk.pipeline_layout );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "descriptor_init: vkCreatePipelineLayout: %s", string_VkResult( r ) );
        goto fail_after_pool;
    }

    LOG_INFO( "descriptor_init: OK (textures=%u, samplers=%u, push=%u bytes)",
              VK_MAX_BINDLESS_TEXTURES, VK_MAX_BINDLESS_SAMPLERS, RHI_MAX_PUSH_CONST_SIZE );
    return true;

fail_after_pool:
    vkDestroyDescriptorPool( vk.device, vk.bindless_pool, vk.alloc_cb );
    vk.bindless_pool = VK_NULL_HANDLE;
fail_after_layout:
    vkDestroyDescriptorSetLayout( vk.device, vk.bindless_layout, vk.alloc_cb );
    vk.bindless_layout = VK_NULL_HANDLE;
    return false;
}

static void
vk_descriptor_shutdown( void )
{
    if ( vk.pipeline_layout != VK_NULL_HANDLE )
    {
        vkDestroyPipelineLayout( vk.device, vk.pipeline_layout, vk.alloc_cb );
        vk.pipeline_layout = VK_NULL_HANDLE;
    }
    /* Destroying the pool implicitly frees vk.bindless_set. */
    if ( vk.bindless_pool != VK_NULL_HANDLE )
    {
        vkDestroyDescriptorPool( vk.device, vk.bindless_pool, vk.alloc_cb );
        vk.bindless_pool = VK_NULL_HANDLE;
        vk.bindless_set  = VK_NULL_HANDLE;
    }
    if ( vk.bindless_layout != VK_NULL_HANDLE )
    {
        vkDestroyDescriptorSetLayout( vk.device, vk.bindless_layout, vk.alloc_cb );
        vk.bindless_layout = VK_NULL_HANDLE;
    }
}

/*==============================================================================================
    Bindless registration  (called via rhi()->register_texture etc.)
==============================================================================================*/

static u32
vk_register_texture( rhi_texture_t handle )
{
    if ( !vk_texture_validate( handle ) )
        return 0;

    u32 slot_idx = vk_bindless_alloc( &g_tex_free );
    if ( slot_idx == 0 )
    {
        LOG_ERROR( "bindless texture slots exhausted" );
        return 0;
    }

    VkDescriptorImageInfo img_info = { 0 };
    img_info.sampler               = VK_NULL_HANDLE;
    img_info.imageView             = vk.textures[ VK_HANDLE_IDX( handle.id ) ].view;
    img_info.imageLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = { 0 };
    write.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet               = vk.bindless_set;
    write.dstBinding           = 0;
    write.dstArrayElement      = slot_idx;
    write.descriptorCount      = 1;
    write.descriptorType       = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo           = &img_info;

    vkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
    return slot_idx;
}

static void
vk_unregister_texture( u32 bindless_index )
{
    vk_bindless_free( &g_tex_free, bindless_index );
    /* Intentionally leave the stale descriptor; it is PARTIALLY_BOUND so unused entries
       are legal as long as shaders do not access them. */
}

static u32
vk_register_sampler( rhi_sampler_t handle )
{
    if ( !vk_sampler_validate( handle ) )
        return 0;

    u32 slot_idx = vk_bindless_alloc( &g_samp_free );
    if ( slot_idx == 0 )
    {
        LOG_ERROR( "bindless sampler slots exhausted" );
        return 0;
    }

    VkDescriptorImageInfo samp_info = { 0 };
    samp_info.sampler               = vk.samplers[ VK_HANDLE_IDX( handle.id ) ].sampler;
    samp_info.imageView             = VK_NULL_HANDLE;
    samp_info.imageLayout           = VK_IMAGE_LAYOUT_UNDEFINED;

    VkWriteDescriptorSet write = { 0 };
    write.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet               = vk.bindless_set;
    write.dstBinding           = 1;
    write.dstArrayElement      = slot_idx;
    write.descriptorCount      = 1;
    write.descriptorType       = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo           = &samp_info;

    vkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
    return slot_idx;
}

static void
vk_unregister_sampler( u32 bindless_index )
{
    vk_bindless_free( &g_samp_free, bindless_index );
}

/*============================================================================================*/
