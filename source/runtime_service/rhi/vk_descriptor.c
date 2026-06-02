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

    VkPipelineLayout  (g_vk.pipeline_layout):
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
    printf( "[rhi:vk] descriptor_init (placeholder)\n" );

    vk_bindless_free_init( &g_tex_free,  VK_MAX_BINDLESS_TEXTURES );
    vk_bindless_free_init( &g_samp_free, VK_MAX_BINDLESS_SAMPLERS );

    /* TODO (Vulkan implementation):

       Enable VK_EXT_descriptor_indexing flags:
           VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci = {
               .bindingCount  = 2,
               .pBindingFlags = { PARTIALLY_BOUND | UPDATE_AFTER_BIND, ... }
           };

       Bindings:
           VkDescriptorSetLayoutBinding bindings[2] = {
               {  // Binding 0: sampled textures
                   .binding            = 0,
                   .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                   .descriptorCount    = VK_MAX_BINDLESS_TEXTURES,
                   .stageFlags         = VK_SHADER_STAGE_ALL,
               },
               {  // Binding 1: samplers
                   .binding            = 1,
                   .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
                   .descriptorCount    = VK_MAX_BINDLESS_SAMPLERS,
                   .stageFlags         = VK_SHADER_STAGE_ALL,
               },
           };
           VkDescriptorSetLayoutCreateInfo layout_ci = {
               .pNext        = &flags_ci,
               .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
               .bindingCount = 2,
               .pBindings    = bindings,
           };
           vkCreateDescriptorSetLayout -> g_vk.bindless_layout

       Pool:
           VkDescriptorPoolSize pool_sizes[] = {
               { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_MAX_BINDLESS_TEXTURES },
               { VK_DESCRIPTOR_TYPE_SAMPLER,       VK_MAX_BINDLESS_SAMPLERS },
           };
           VkDescriptorPoolCreateInfo pool_ci = {
               .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
               .maxSets       = 1,
               .poolSizeCount = 2,
               .pPoolSizes    = pool_sizes,
           };
           vkCreateDescriptorPool -> g_vk.bindless_pool

       Set:
           VkDescriptorSetAllocateInfo alloc_info = {
               .descriptorPool     = g_vk.bindless_pool,
               .descriptorSetCount = 1,
               .pSetLayouts        = &g_vk.bindless_layout,
           };
           vkAllocateDescriptorSets -> g_vk.bindless_set

       Pipeline layout (push constants + bindless set):
           VkPushConstantRange pc_range = {
               .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
               .offset     = 0,
               .size       = RHI_MAX_PUSH_CONST_SIZE,
           };
           VkPipelineLayoutCreateInfo pl_ci = {
               .setLayoutCount         = 1,
               .pSetLayouts            = &g_vk.bindless_layout,
               .pushConstantRangeCount = 1,
               .pPushConstantRanges    = &pc_range,
           };
           vkCreatePipelineLayout -> g_vk.pipeline_layout
    */

    return true;
}

static void
vk_descriptor_shutdown( void )
{
    /* TODO:
       vkDestroyPipelineLayout  ( g_vk.device, g_vk.pipeline_layout, g_vk.alloc_cb )
       vkDestroyDescriptorPool  ( g_vk.device, g_vk.bindless_pool,   g_vk.alloc_cb )
       vkDestroyDescriptorSetLayout( g_vk.device, g_vk.bindless_layout, g_vk.alloc_cb )
    */
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

    /* TODO:
       VkDescriptorImageInfo img_info = {
           .sampler     = VK_NULL_HANDLE,
           .imageView   = g_vk.textures[ VK_HANDLE_IDX(handle.id) ].view,
           .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
       };
       VkWriteDescriptorSet write = {
           .dstSet          = g_vk.bindless_set,
           .dstBinding      = 0,
           .dstArrayElement = slot_idx,
           .descriptorCount = 1,
           .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
           .pImageInfo      = &img_info,
       };
       vkUpdateDescriptorSets( g_vk.device, 1, &write, 0, NULL )
    */

    UNUSED( handle );
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

    /* TODO:
       VkDescriptorImageInfo samp_info = {
           .sampler     = g_vk.samplers[ VK_HANDLE_IDX(handle.id) ].sampler,
           .imageView   = VK_NULL_HANDLE,
           .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       };
       Write descriptor at binding 1, element slot_idx.
    */

    UNUSED( handle );
    return slot_idx;
}

static void
vk_unregister_sampler( u32 bindless_index )
{
    vk_bindless_free( &g_samp_free, bindless_index );
}

/*============================================================================================*/
