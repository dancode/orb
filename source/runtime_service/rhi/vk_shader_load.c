/*==============================================================================================

    vk_shader_load.c -- SPIR-V shader loading helpers.

    Three loading paths:
        vk_shader_load_file    -- reads a compiled .spv file from disk
        vk_shader_load_memory  -- creates from an in-memory or embedded SPIR-V array
        vk_shader_load_oshd    -- reads a cooked .oshd container (rhi_shader_format.h): the
                                  SPIR-V payload plus the reflection tables shader_tool baked
                                  at cook time.  Stage and entry point come from the
                                  container, and the reflection lands in the shader slot so
                                  pipeline_create can derive vertex input and validate the
                                  push constant contract.  Bindings are checked against the
                                  RHI's bindless contract here -- a shader that declares
                                  anything but set 0 / binding 0 (sampled images) / binding 1
                                  (samplers) fails the load outright.

    All delegate to vk_shader_create() in vk_shader.c and follow the same error
    reporting conventions.  The caller is responsible for vk_shader_destroy() on the
    returned handle.

    Usage from draw_material.c (or anywhere) will look like:
    rhi_shader_t vert = rhi()->shader_load_file( 
        "data/shaders/solid.vert.spv", RHI_SHADER_STAGE_VERTEX, "main", "solid.vert" );

    rhi_shader_t frag = rhi()->shader_load_file( 
        "data/shaders/solid.frag.spv", RHI_SHADER_STAGE_FRAGMENT, "main", "solid.frag" );

==============================================================================================*/

#include "runtime_service/rhi/rhi_shader_format.h"

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

/* oshd_stage_to_rhi -- container stage -> RHI stage; 0 = a stage the RHI does not run. */
static rhi_shader_stage_t
oshd_stage_to_rhi( u32 stage )
{
    switch ( stage )
    {
        case OSHD_STAGE_VERTEX:   return RHI_SHADER_STAGE_VERTEX;
        case OSHD_STAGE_FRAGMENT: return RHI_SHADER_STAGE_FRAGMENT;
        case OSHD_STAGE_COMPUTE:  return RHI_SHADER_STAGE_COMPUTE;
        default:                  return ( rhi_shader_stage_t )0;
    }
}

/* Load a cooked .oshd container: SPIR-V payload + the reflection tables shader_tool baked at
   cook time.  Stage and entry come from the container.  debug_name falls back to path. */
static rhi_shader_t
vk_shader_load_oshd( const char* path, const char* debug_name )
{
    rhi_shader_t bad = { RHI_NULL_HANDLE };

    if ( !path || !path[ 0 ] )
    {
        LOG_ERROR( "shader_load_oshd: null or empty path" );
        return bad;
    }

    FILE* f = fopen( path, "rb" );
    if ( !f )
    {
        LOG_ERROR( "shader_load_oshd: could not open '%s'", path );
        return bad;
    }

    fseek( f, 0, SEEK_END );
    long sz = ftell( f );
    fseek( f, 0, SEEK_SET );

    if ( sz < ( long )sizeof( oshd_header_t ) )
    {
        LOG_ERROR( "shader_load_oshd: '%s' too small for an .oshd header (%ld bytes)", path, sz );
        fclose( f );
        return bad;
    }

    u8* data = ( u8* )malloc( ( size_t )sz );
    if ( !data )
    {
        LOG_ERROR( "shader_load_oshd: out of memory reading '%s' (%ld bytes)", path, sz );
        fclose( f );
        return bad;
    }

    size_t n = fread( data, 1, ( size_t )sz, f );
    fclose( f );
    if ( n != ( size_t )sz )
    {
        LOG_ERROR( "shader_load_oshd: read error on '%s' (%zu of %ld bytes)", path, n, sz );
        free( data );
        return bad;
    }

    /* Validate the container in u64 before trusting any count or offset. */
    const oshd_header_t* h = ( const oshd_header_t* )data;
    u64 need               = ( u64 )sizeof( oshd_header_t )
                           + ( u64 )h->input_count * sizeof( oshd_input_t )
                           + ( u64 )h->pc_member_count * sizeof( oshd_pc_member_t )
                           + ( u64 )h->binding_count * sizeof( oshd_binding_t )
                           + ( u64 )h->strtab_size + ( u64 )h->spirv_size;

    if ( h->magic != OSHD_MAGIC || h->version != OSHD_VERSION || need != ( u64 )sz ||
         h->strtab_size < 4 || h->strtab_size % 4 != 0 ||
         h->spirv_size == 0 || h->spirv_size % 4 != 0 )
    {
        LOG_ERROR( "shader_load_oshd: '%s' is not a valid .oshd v%d container", path,
                   OSHD_VERSION );
        free( data );
        return bad;
    }

    const oshd_input_t*   inputs = ( const oshd_input_t* )( h + 1 );
    const oshd_binding_t* binds  = ( const oshd_binding_t* )
        ( ( const oshd_pc_member_t* )( inputs + h->input_count ) + h->pc_member_count );
    const char*           strtab = ( const char* )( binds + h->binding_count );
    const void*           spirv  = strtab + h->strtab_size;

    rhi_shader_stage_t stage = oshd_stage_to_rhi( h->stage );
    if ( stage == 0 || strtab[ h->strtab_size - 1 ] != 0 )
    {
        LOG_ERROR( "shader_load_oshd: '%s' has an unsupported stage (%u) or corrupt strings",
                   path, h->stage );
        free( data );
        return bad;
    }

    /* Contract guards: everything the pipeline will trust must fit the RHI's limits, and
       every binding must be the bindless global set -- set 0, binding 0 = sampled images,
       binding 1 = samplers.  UBOs/SSBOs/anything else has no home in this RHI; refusing the
       load here turns a would-be GPU mystery into a named error. */
    if ( h->input_count > RHI_MAX_VERTEX_ATTRIBS )
    {
        LOG_ERROR( "shader_load_oshd: '%s' declares %u vertex inputs (max %d)", path,
                   h->input_count, RHI_MAX_VERTEX_ATTRIBS );
        free( data );
        return bad;
    }
    if ( h->pc_size > RHI_MAX_PUSH_CONST_SIZE )
    {
        LOG_ERROR( "shader_load_oshd: '%s' push constants are %u bytes (max %d)", path,
                   h->pc_size, RHI_MAX_PUSH_CONST_SIZE );
        free( data );
        return bad;
    }
    for ( u32 i = 0; i < h->binding_count; ++i )
    {
        const oshd_binding_t* b  = &binds[ i ];
        bool                  ok =
            b->set == 0 &&
            ( ( b->binding == 0 && b->descriptor_type == ( u32 )VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ) ||
              ( b->binding == 1 && b->descriptor_type == ( u32 )VK_DESCRIPTOR_TYPE_SAMPLER ) );
        if ( !ok )
        {
            LOG_ERROR( "shader_load_oshd: '%s' binding [set %u, binding %u, type %u] '%s' is "
                       "outside the bindless contract (set 0: b0 sampled images, b1 samplers)",
                       path, b->set, b->binding, b->descriptor_type,
                       b->name < h->strtab_size ? strtab + b->name : "" );
            free( data );
            return bad;
        }
    }

    rhi_shader_desc_t desc = { 0 };
    desc.spirv             = spirv;
    desc.spirv_size        = h->spirv_size;
    desc.stage             = stage;
    desc.entry             = h->entry < h->strtab_size ? strtab + h->entry : "main";
    desc.debug_name        = debug_name ? debug_name : path;

    rhi_shader_t handle = vk_shader_create( &desc );
    if ( !rhi_handle_valid( handle ) )
    {
        LOG_ERROR( "shader_load_oshd: vk_shader_create failed for '%s'", path );
        free( data );
        return bad;
    }

    /* Land the reflection in the slot for pipeline_create to derive/validate against. */
    vk_shader_reflect_t* r = &vk.shaders[ handle.id ].reflect;
    r->has_data            = true;
    r->input_count         = h->input_count;
    for ( u32 i = 0; i < h->input_count; ++i )
    {
        r->input_location[ i ] = inputs[ i ].location;
        r->input_format[ i ]   = inputs[ i ].vk_format;
        r->input_size[ i ]     = inputs[ i ].size;
    }
    r->pc_size     = h->pc_size;
    r->layout_hash = h->layout_hash;

    LOG_INFO( "shader_load_oshd: '%s' (%s, spirv %u bytes, %u inputs, pc %u bytes, hash %016llx)",
              path, desc.entry, h->spirv_size, h->input_count, h->pc_size,
              ( unsigned long long )h->layout_hash );

    free( data );
    return handle;
}

/*============================================================================================*/
