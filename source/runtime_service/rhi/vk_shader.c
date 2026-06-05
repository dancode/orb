/*==============================================================================================

    vulkan/vk_shader.c -- SPIR-V shader module lifecycle.

    Slot pool: vk.shaders[ VK_MAX_SHADERS ] (vk_shader_slot_t).

    Shaders are cheap VkShaderModule objects (Vulkan holds the SPIR-V blob).
    Pipeline creation (vk_pipeline.c) consumes shader handles; the shader may be
    destroyed after the pipeline is built -- Vulkan retains the compiled code.

    Hot-reload compatibility:
        The render DLL above can re-create shaders and call pipeline_create/destroy
        to swap pipeline objects without restarting the host.  The RHI pipeline handle
        is a stable identifier from the host's perspective; only the GPU object changes.

==============================================================================================*/

static i32
vk_shader_alloc_slot( void )
{
    for ( u32 i = 0; i < VK_MAX_SHADERS; ++i )
    {
        if ( vk.shaders[ i ].generation == 0 )
            return ( i32 )i;
    }
    return -1;
}

static bool
vk_shader_validate( rhi_shader_t handle )
{
    u32 idx = VK_HANDLE_IDX( handle.id );
    u8  gen = VK_HANDLE_GEN( handle.id );
    return idx < VK_MAX_SHADERS && gen != 0 && vk.shaders[ idx ].generation == gen;
}

static rhi_shader_t
vk_shader_create( const rhi_shader_desc_t* desc )
{
    if ( !desc || !desc->spirv || desc->spirv_size == 0 )
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };

    if ( desc->spirv_size % 4 != 0 )
    {
        LOG_ERROR( "shader SPIR-V size (%u) is not a multiple of 4", desc->spirv_size );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    i32 idx = vk_shader_alloc_slot();
    if ( idx < 0 )
    {
        LOG_ERROR( "shader pool exhausted (VK_MAX_SHADERS = %d)", VK_MAX_SHADERS );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    vk_shader_slot_t* slot = &vk.shaders[ idx ];
    u8 gen = ( u8 )( slot->generation == 0 ? 1 : slot->generation );

    VkShaderModuleCreateInfo ci = { 0 };
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = desc->spirv_size;
    ci.pCode    = (const u32*)desc->spirv;

    VkResult r = vkCreateShaderModule( vk.device, &ci, vk.alloc_cb, &slot->module );
    if ( r != VK_SUCCESS )
    {
        LOG_ERROR( "shader_create: vkCreateShaderModule: %s", string_VkResult( r ) );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    /* Copy entry point name; fall back to "main" if not specified. */
    const char* entry = ( desc->entry && desc->entry[0] ) ? desc->entry : "main";
    strncpy( slot->entry, entry, sizeof( slot->entry ) - 1 );
    slot->entry[ sizeof( slot->entry ) - 1 ] = '\0';

    if ( desc->debug_name )
        vk_debug_name_object( VK_OBJECT_TYPE_SHADER_MODULE, (u64)slot->module, desc->debug_name );

    slot->stage      = desc->stage;
    slot->generation = gen;

    return ( rhi_shader_t ){ VK_MAKE_HANDLE( gen, ( u32 )idx ) };
}

static void
vk_shader_destroy( rhi_shader_t handle )
{
    if ( !vk_shader_validate( handle ) )
        return;

    u32               idx  = VK_HANDLE_IDX( handle.id );
    vk_shader_slot_t* slot = &vk.shaders[ idx ];

    vkDestroyShaderModule( vk.device, slot->module, vk.alloc_cb );

    slot->generation = ( u8 )( slot->generation + 1 );
    slot->module     = VK_NULL_HANDLE;
    slot->entry[ 0 ] = '\0';
}

/*============================================================================================*/
