/*==============================================================================================

    vk_shader_load.c -- SPIR-V shader loading helpers.

    Two loading paths:
        vk_shader_load_file    -- reads a compiled .spv file from disk
        vk_shader_load_memory  -- creates from an in-memory or embedded SPIR-V array

    Both delegate to vk_shader_create() in vk_shader.c and follow the same error
    reporting conventions.  The caller is responsible for vk_shader_destroy() on the
    returned handle.

    Usage from draw_material.c (or anywhere) will look like:
    rhi_shader_t vert = rhi()->shader_load_file( 
        "data/shaders/solid.vert.spv", RHI_SHADER_STAGE_VERTEX, "main", "solid.vert" );

    rhi_shader_t frag = rhi()->shader_load_file( 
        "data/shaders/solid.frag.spv", RHI_SHADER_STAGE_FRAGMENT, "main", "solid.frag" );

==============================================================================================*/

/* Load a compiled SPIR-V blob (.spv) from disk.  debug_name falls back to path if NULL. */
static rhi_shader_t
vk_shader_load_file( const char* path, rhi_shader_stage_t stage,
                     const char* entry, const char* debug_name )
{
    if ( !path || !path[ 0 ] )
    {
        LOG_ERROR( "shader_load_file: null or empty path" );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    FILE* f = fopen( path, "rb" );
    if ( !f )
    {
        LOG_ERROR( "shader_load_file: could not open '%s'", path );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    fseek( f, 0, SEEK_END );
    long sz = ftell( f );
    fseek( f, 0, SEEK_SET );

    if ( sz <= 0 || sz % 4 != 0 )
    {
        LOG_ERROR( "shader_load_file: '%s' invalid size (%ld bytes, must be a non-zero multiple of 4)",
                   path, sz );
        fclose( f );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    void* data = malloc( (size_t)sz );
    if ( !data )
    {
        LOG_ERROR( "shader_load_file: out of memory reading '%s' (%ld bytes)", path, sz );
        fclose( f );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    size_t n = fread( data, 1, (size_t)sz, f );
    fclose( f );

    if ( n != (size_t)sz )
    {
        LOG_ERROR( "shader_load_file: read error on '%s' (%zu of %ld bytes)", path, n, sz );
        free( data );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }

    rhi_shader_desc_t desc = { 0 };
    desc.spirv      = data;
    desc.spirv_size = (u32)sz;
    desc.stage      = stage;
    desc.entry      = entry;
    desc.debug_name = debug_name ? debug_name : path;

    rhi_shader_t h = vk_shader_create( &desc );
    free( data );

    if ( rhi_handle_valid( h ) )
        LOG_INFO( "shader_load_file: '%s' (%ld bytes)", path, sz );
    else
        LOG_ERROR( "shader_load_file: vk_shader_create failed for '%s'", path );

    return h;
}

/* Create a shader from an embedded SPIR-V byte array (e.g. a static C array).
   Equivalent to rhi()->shader_create() but with individual parameters instead of a desc struct. */
static rhi_shader_t
vk_shader_load_memory( const void* spirv, u32 size, rhi_shader_stage_t stage,
                        const char* entry, const char* debug_name )
{
    rhi_shader_desc_t desc = { 0 };
    desc.spirv      = spirv;
    desc.spirv_size = size;
    desc.stage      = stage;
    desc.entry      = entry;
    desc.debug_name = debug_name;
    return vk_shader_create( &desc );
}

/*============================================================================================*/
