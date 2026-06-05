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

/*==============================================================================================
    Deferred descriptor slot retirement.

    UPDATE_AFTER_BIND lets the GPU read descriptors while the CPU writes other slots in
    the same array.  Returning a freed slot to the free stack immediately is therefore
    unsafe: a new registration would overwrite that descriptor entry while the GPU may
    still be reading the old one in a previous frame.

    Instead, vk_unregister_* pushes (idx, safe_at) into a FIFO retire ring, where safe_at
    is the global_frame threshold at which the slot is provably idle on all contexts.
    vk_descriptor_flush_retired() (called after the fence wait in vk_frame_begin) drains
    entries once vk.global_frame reaches their safe_at.

    Multi-context correctness: vk.global_frame increments once per frame_begin, which is
    once per context per display frame.  With N active contexts, global_frame advances by N
    per display frame, so the effective in-flight guard must be VK_MAX_FRAMES_IN_FLIGHT * N.
    vk_retire_safe_at() snapshots N at retirement time and bakes it into safe_at.  If a
    context is later destroyed the FIFO head may have a larger safe_at than newer entries;
    the drain loop will delay those newer entries until the older one ages out -- conservative
    but correct, never a leak.

    Ring capacity equals the pool size.  Because slot 0 is reserved, at most
    (capacity - 1) slots can ever be allocated at once, so the ring never overflows.
==============================================================================================*/

typedef struct vk_deferred_retire_s
{
    u32 idx;
    u32 safe_at;    /* vk.global_frame value at which this slot is safe to reuse */
} vk_deferred_retire_t;

static vk_deferred_retire_t  g_tex_retire[ VK_MAX_BINDLESS_TEXTURES ];
static u32                   g_tex_retire_head;   /* next entry to drain */
static u32                   g_tex_retire_tail;   /* next entry to write */

static vk_deferred_retire_t  g_samp_retire[ VK_MAX_BINDLESS_SAMPLERS ];
static u32                   g_samp_retire_head;
static u32                   g_samp_retire_tail;

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

/* Compute the global_frame threshold that must be reached before a slot retired NOW is safe.
   Scales by the active context count because global_frame advances once per frame_begin per
   context; all contexts must have cycled through VK_MAX_FRAMES_IN_FLIGHT fence waits. */
static u32
vk_retire_safe_at( void )
{
    u32 n_ctx = 0;
    for ( u32 mask = vk.ctx_alloc; mask; mask &= mask - 1 )
        n_ctx++;
    if ( n_ctx == 0 )
        n_ctx = 1;
    return vk.global_frame + VK_MAX_FRAMES_IN_FLIGHT * n_ctx;
}

static void
vk_descriptor_flush_retired( void )
{
    while ( g_tex_retire_head != g_tex_retire_tail )
    {
        vk_deferred_retire_t* e = &g_tex_retire[ g_tex_retire_head ];
        if ( vk.global_frame < e->safe_at )
            break;
        vk_bindless_free( &g_tex_free, e->idx );
        g_tex_retire_head = ( g_tex_retire_head + 1 ) % VK_MAX_BINDLESS_TEXTURES;
    }

    while ( g_samp_retire_head != g_samp_retire_tail )
    {
        vk_deferred_retire_t* e = &g_samp_retire[ g_samp_retire_head ];
        if ( vk.global_frame < e->safe_at )
            break;
        vk_bindless_free( &g_samp_free, e->idx );
        g_samp_retire_head = ( g_samp_retire_head + 1 ) % VK_MAX_BINDLESS_SAMPLERS;
    }
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
    img_info.imageView             = vk.textures[ handle.id ].view;
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
    if ( bindless_index == 0 )
        return;
    /* Defer until all active contexts have cycled through VK_MAX_FRAMES_IN_FLIGHT fences. */
    u32 next = ( g_tex_retire_tail + 1 ) % VK_MAX_BINDLESS_TEXTURES;
    ORB_ASSERT( next != g_tex_retire_head );   /* deferred queue overflow -- impossible if slot 0 is reserved */
    g_tex_retire[ g_tex_retire_tail ] = ( vk_deferred_retire_t ){ bindless_index, vk_retire_safe_at() };
    g_tex_retire_tail                 = next;
    /* Stale descriptor remains; PARTIALLY_BOUND allows unused slots to stay as-is. */
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
    samp_info.sampler               = vk.samplers[ handle.id ].sampler;
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
    if ( bindless_index == 0 )
        return;
    u32 next = ( g_samp_retire_tail + 1 ) % VK_MAX_BINDLESS_SAMPLERS;
    ORB_ASSERT( next != g_samp_retire_head );
    g_samp_retire[ g_samp_retire_tail ] = ( vk_deferred_retire_t ){ bindless_index, vk_retire_safe_at() };
    g_samp_retire_tail                  = next;
}

/*============================================================================================*/
