/*==============================================================================================

    shader_tool.c (shader cooker -- offline HLSL -> SPIR-V compiler front end)

    The font_tool analog for shaders: a thin CLI that drives dxc.exe (the DirectX Shader
    Compiler shipped with the Vulkan SDK) to compile HLSL source into SPIR-V, spawned as a
    child process via sys_process_run -- the same primitive asset_tool uses to drive font_tool.

    Usage:

        shader_tool compile <src.hlsl> -o <out.spv> -T <profile> [-E entry] [-I dir] [-Zi]
            <profile>   dxc target profile: vs_6_0, ps_6_0, cs_6_0, ...
            -E entry    entry point function name (default "main")
            -I dir      extra include directory (may repeat)
            -Zi         emit debug info into the SPIR-V

        shader_tool reflect <file.spv>
            Human-readable dump of what the RHI needs to agree with the shader: entry point,
            stage, vertex inputs (location/format), push constant block layout (member
            offsets/sizes), and descriptor bindings (set/binding/type/count).  Uses the
            vendored SPIRV-Reflect library (shader_tool_spirv_reflect.c).

    Engine-wide conventions are baked into the dxc invocation so no shader or build script
    ever re-decides them:

        -spirv -fspv-target-env=vulkan1.3    matches the RHI (dynamic rendering, sync2)
        -WX                                  warnings are errors
        -Zpc                                 column-major matrix packing (explicit, not implied)
        -fvk-invert-y                        vertex-ish stages only (vs/ds/gs): HLSL is authored
                                             in the D3D convention (+Y up in clip space); dxc
                                             negates gl_Position.y so output lands in Vulkan's
                                             Y-down NDC without per-shader hacks

    dxc is located through %VULKAN_SDK%\Bin\dxc.exe; there is no fallback to PATH, so the
    error message can name the one thing to fix (install / re-source the Vulkan SDK).

    Later phases (SHADER_SYSTEM plan) add verbs here: `cook` (.oshd container = reflection
    tables + SPIR-V) and `header` (generated C layout structs).

    Link list for this executable:

        base            (headers only -- unity built in)
        sys             (process spawn, file io, clock -- statically linked)

    No core, no module system, no app.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/base.h"
#include "engine/sys/sys_host.h"
#include "vendor/spirv_reflect.h"

#define SHADER_TOOL_CMD_MAX      4096
#define SHADER_TOOL_MAX_INCLUDES 8

/*============================================================================================*/
/*  dxc location                                                                              */
/*============================================================================================*/

/* dxc_locate -- resolve %VULKAN_SDK%\Bin\dxc.exe and verify it exists. */
static bool
dxc_locate( char* out, size_t cap )
{
    const char* sdk = getenv( "VULKAN_SDK" );
    if ( !sdk || !sdk[ 0 ] )
    {
        fprintf( stderr, "shader_tool: error: VULKAN_SDK is not set (install the Vulkan SDK)\n" );
        return false;
    }

    snprintf( out, cap, "%s\\Bin\\dxc.exe", sdk );
    if ( !sys_file_exists( out ) )
    {
        fprintf( stderr, "shader_tool: error: dxc not found at %s\n", out );
        return false;
    }
    return true;
}

/*============================================================================================*/
/*  compile verb                                                                              */
/*============================================================================================*/

typedef struct compile_args_s
{
    const char* src;                                    /* input .hlsl                    */
    const char* out;                                    /* output .spv                    */
    const char* profile;                                /* dxc -T (vs_6_0, ps_6_0, ...)   */
    const char* entry;                                  /* dxc -E (default "main")        */
    const char* includes[ SHADER_TOOL_MAX_INCLUDES ];   /* dxc -I dirs                    */
    int         include_count;
    bool        debug_info;                             /* -Zi                            */

} compile_args_t;

/* profile_is_vertex_ish -- stages whose SV_Position output needs the Vulkan Y flip. */
static bool
profile_is_vertex_ish( const char* profile )
{
    return strncmp( profile, "vs_", 3 ) == 0 ||
           strncmp( profile, "ds_", 3 ) == 0 ||
           strncmp( profile, "gs_", 3 ) == 0;
}

/* cmd_append -- bounded strcat onto the dxc command line; flags overflow instead of truncating
   silently so a clipped command never reaches the child process. */
static bool
cmd_append( char* cmd, size_t cap, const char* piece )
{
    size_t len = strlen( cmd );
    size_t add = strlen( piece );
    if ( len + add + 1 > cap )
        return false;
    memcpy( cmd + len, piece, add + 1 );
    return true;
}

static int
run_compile( const compile_args_t* a )
{
    char dxc[ 512 ];
    if ( !dxc_locate( dxc, sizeof( dxc ) ) )
        return 1;

    /* Assemble the full dxc command line.  Every path is quoted in case of spaces. */
    char cmd[ SHADER_TOOL_CMD_MAX ] = { 0 };
    char piece[ 1024 ];
    bool ok = true;

    snprintf( piece, sizeof( piece ),
              "\"%s\" -spirv -fspv-target-env=vulkan1.3 -WX -Zpc -T %s -E %s",
              dxc, a->profile, a->entry );
    ok = ok && cmd_append( cmd, sizeof( cmd ), piece );

    if ( profile_is_vertex_ish( a->profile ) )
        ok = ok && cmd_append( cmd, sizeof( cmd ), " -fvk-invert-y" );

    if ( a->debug_info )
        ok = ok && cmd_append( cmd, sizeof( cmd ), " -Zi" );

    for ( int i = 0; i < a->include_count; ++i )
    {
        snprintf( piece, sizeof( piece ), " -I \"%s\"", a->includes[ i ] );
        ok = ok && cmd_append( cmd, sizeof( cmd ), piece );
    }

    snprintf( piece, sizeof( piece ), " \"%s\" -Fo \"%s\"", a->src, a->out );
    ok = ok && cmd_append( cmd, sizeof( cmd ), piece );

    if ( !ok )
    {
        fprintf( stderr, "shader_tool: error: dxc command line exceeds %d chars\n",
                 SHADER_TOOL_CMD_MAX );
        return 1;
    }

    /* Spawn dxc with inherited stdio, so its diagnostics print straight to our console. */
    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) )
    {
        fprintf( stderr, "shader_tool: error: could not launch dxc (%s)\n", dxc );
        return 1;
    }
    if ( res.exit_code != 0 )
    {
        fprintf( stderr, "shader_tool: error: dxc exited %d for %s\n", res.exit_code, a->src );
        return 1;
    }

    /* dxc can exit 0 without writing output in some pathological cases; verify the artifact. */
    u32 size = sys_file_size( a->out );
    if ( !sys_file_exists( a->out ) || size == 0 || size % 4 != 0 )
    {
        fprintf( stderr, "shader_tool: error: dxc succeeded but %s is missing or invalid (%u bytes)\n",
                 a->out, size );
        return 1;
    }

    printf( "shader_tool: compile %s -> %s (%s %s, %u bytes, %.0f ms)\n", a->src, a->out,
            a->profile, a->entry, size, res.elapsed_seconds * 1000.0 );
    return 0;
}

/*============================================================================================*/
/*  reflect verb                                                                              */
/*============================================================================================*/

/* stage_name -- printable shader stage. */
static const char*
stage_name( SpvReflectShaderStageFlagBits stage )
{
    switch ( stage )
    {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:                  return "vertex";
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return "tess_control";
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tess_eval";
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:                return "geometry";
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:                return "fragment";
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:                 return "compute";
        default:                                                   return "unknown";
    }
}

/* format_name -- printable interface variable format (the common 32-bit cases; anything
   exotic prints its raw VkFormat value via the fallback). */
static const char*
format_name( SpvReflectFormat fmt )
{
    switch ( fmt )
    {
        case SPV_REFLECT_FORMAT_R32_SFLOAT:          return "float";
        case SPV_REFLECT_FORMAT_R32G32_SFLOAT:       return "float2";
        case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:    return "float3";
        case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT: return "float4";
        case SPV_REFLECT_FORMAT_R32_UINT:            return "uint";
        case SPV_REFLECT_FORMAT_R32G32_UINT:         return "uint2";
        case SPV_REFLECT_FORMAT_R32G32B32_UINT:      return "uint3";
        case SPV_REFLECT_FORMAT_R32G32B32A32_UINT:   return "uint4";
        case SPV_REFLECT_FORMAT_R32_SINT:            return "int";
        case SPV_REFLECT_FORMAT_R32G32_SINT:         return "int2";
        case SPV_REFLECT_FORMAT_R32G32B32_SINT:      return "int3";
        case SPV_REFLECT_FORMAT_R32G32B32A32_SINT:   return "int4";
        default:                                     return NULL;
    }
}

/* descriptor_type_name -- printable VkDescriptorType. */
static const char*
descriptor_type_name( SpvReflectDescriptorType t )
{
    switch ( t )
    {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:                return "sampler";
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined_image_sampler";
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:          return "sampled_image";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:          return "storage_image";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:   return "uniform_texel_buffer";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:   return "storage_texel_buffer";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         return "uniform_buffer";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:         return "storage_buffer";
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "uniform_buffer_dynamic";
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "storage_buffer_dynamic";
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:       return "input_attachment";
        default:                                                 return "unknown";
    }
}

/* dump_block_members -- recursive member printout for a push constant (or buffer) block.
   Offsets are absolute within the block, which is exactly what the CPU-side struct must
   reproduce. */
static void
dump_block_members( const SpvReflectBlockVariable* block, int indent )
{
    for ( u32 i = 0; i < block->member_count; ++i )
    {
        const SpvReflectBlockVariable* m = &block->members[ i ];
        printf( "  %*s[offset %4u, size %4u] %s\n", indent, "", m->absolute_offset, m->size,
                m->name ? m->name : "(unnamed)" );
        if ( m->member_count > 0 )
            dump_block_members( m, indent + 4 );
    }
}

/* run_reflect -- load a compiled SPIR-V blob and print everything the RHI must agree with. */
static int
run_reflect( const char* path )
{
    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
    {
        fprintf( stderr, "shader_tool: error: could not read %s\n", path );
        return 1;
    }
    if ( fd.size == 0 || fd.size % 4 != 0 )
    {
        fprintf( stderr, "shader_tool: error: %s is not SPIR-V (%u bytes, must be a multiple of 4)\n",
                 path, fd.size );
        sys_file_free( &fd );
        return 1;
    }

    SpvReflectShaderModule module;
    SpvReflectResult       res = spvReflectCreateShaderModule( fd.size, fd.data, &module );
    if ( res != SPV_REFLECT_RESULT_SUCCESS )
    {
        fprintf( stderr, "shader_tool: error: SPIR-V reflection failed for %s (result %d)\n",
                 path, ( int )res );
        sys_file_free( &fd );
        return 1;
    }

    printf( "shader_tool: reflect %s (%u bytes)\n", path, fd.size );
    printf( "  stage : %s\n", stage_name( module.shader_stage ) );
    printf( "  entry : %s\n", module.entry_point_name ? module.entry_point_name : "(none)" );

    /* Vertex inputs (built-ins like SV_VertexID are internal wiring, not vertex buffer
       contract, so they are skipped). */
    u32 count = 0;
    spvReflectEnumerateInputVariables( &module, &count, NULL );
    SpvReflectInterfaceVariable** inputs = NULL;
    if ( count )
    {
        inputs = ( SpvReflectInterfaceVariable** )malloc( count * sizeof( *inputs ) );
        spvReflectEnumerateInputVariables( &module, &count, inputs );
    }

    /* Drop built-ins, then insertion-sort the survivors by location so the dump reads like a
       vertex layout declaration regardless of the order dxc emitted them. */
    u32 user_inputs = 0;
    for ( u32 i = 0; i < count; ++i )
        if ( inputs[ i ]->built_in < 0 )
            inputs[ user_inputs++ ] = inputs[ i ];
    for ( u32 i = 1; i < user_inputs; ++i )
    {
        SpvReflectInterfaceVariable* v = inputs[ i ];
        u32                          j = i;
        for ( ; j > 0 && inputs[ j - 1 ]->location > v->location; --j )
            inputs[ j ] = inputs[ j - 1 ];
        inputs[ j ] = v;
    }

    printf( "  inputs: %u\n", user_inputs );
    for ( u32 i = 0; i < user_inputs; ++i )
    {
        const SpvReflectInterfaceVariable* v   = inputs[ i ];
        const char*                        fmt = format_name( v->format );
        if ( fmt )
            printf( "      [location %2u] %-8s %s\n", v->location, fmt,
                    v->name ? v->name : "(unnamed)" );
        else
            printf( "      [location %2u] vkfmt=%-4d %s\n", v->location, ( int )v->format,
                    v->name ? v->name : "(unnamed)" );
    }
    free( inputs );

    /* Push constant blocks: the CPU struct contract.  size = span of declared members;
       padded_size = std430-rounded size (what dxc actually reserves). */
    count = 0;
    spvReflectEnumeratePushConstantBlocks( &module, &count, NULL );
    SpvReflectBlockVariable** blocks = NULL;
    if ( count )
    {
        blocks = ( SpvReflectBlockVariable** )malloc( count * sizeof( *blocks ) );
        spvReflectEnumeratePushConstantBlocks( &module, &count, blocks );
    }

    printf( "  push constant blocks: %u\n", count );
    for ( u32 i = 0; i < count; ++i )
    {
        const SpvReflectBlockVariable* b = blocks[ i ];
        printf( "      block \"%s\" -- size %u, padded %u\n",
                b->type_description && b->type_description->type_name ? b->type_description->type_name
                : ( b->name ? b->name : "(unnamed)" ),
                b->size, b->padded_size );
        dump_block_members( b, 4 );
    }
    free( blocks );

    /* Descriptor bindings: must stay within the RHI's global bindless contract (set 0). */
    count = 0;
    spvReflectEnumerateDescriptorBindings( &module, &count, NULL );
    SpvReflectDescriptorBinding** binds = NULL;
    if ( count )
    {
        binds = ( SpvReflectDescriptorBinding** )malloc( count * sizeof( *binds ) );
        spvReflectEnumerateDescriptorBindings( &module, &count, binds );
    }

    printf( "  descriptor bindings: %u\n", count );
    for ( u32 i = 0; i < count; ++i )
    {
        const SpvReflectDescriptorBinding* b = binds[ i ];
        printf( "      [set %u, binding %u] %s x%u %s\n", b->set, b->binding,
                descriptor_type_name( b->descriptor_type ), b->count, b->name ? b->name : "" );
    }
    free( binds );

    spvReflectDestroyShaderModule( &module );
    sys_file_free( &fd );
    return 0;
}

/*============================================================================================*/

static int
usage( void )
{
    fprintf( stderr,
             "usage:\n"
             "  shader_tool compile <src.hlsl> -o <out.spv> -T <profile> [-E entry] [-I dir] [-Zi]\n"
             "      <profile>   dxc target profile: vs_6_0, ps_6_0, cs_6_0, ...\n"
             "      -E entry    entry point name (default \"main\")\n"
             "      -I dir      extra include directory (repeatable)\n"
             "      -Zi         embed debug info in the SPIR-V\n"
             "  shader_tool reflect <file.spv>\n"
             "      dump stage, entry, inputs, push constants, descriptor bindings\n" );
    return 1;
}

int
main( int argc, char** argv )
{
    sys_tick_init();

    int rc = 1;

    if ( argc >= 3 && strcmp( argv[ 1 ], "compile" ) == 0 )
    {
        compile_args_t a = { 0 };
        a.src   = argv[ 2 ];
        a.entry = "main";

        bool bad = false;
        for ( int i = 3; i < argc; ++i )
        {
            if ( strcmp( argv[ i ], "-o" ) == 0 && i + 1 < argc )
                a.out = argv[ ++i ];
            else if ( strcmp( argv[ i ], "-T" ) == 0 && i + 1 < argc )
                a.profile = argv[ ++i ];
            else if ( strcmp( argv[ i ], "-E" ) == 0 && i + 1 < argc )
                a.entry = argv[ ++i ];
            else if ( strcmp( argv[ i ], "-I" ) == 0 && i + 1 < argc )
            {
                if ( a.include_count < SHADER_TOOL_MAX_INCLUDES )
                    a.includes[ a.include_count++ ] = argv[ ++i ];
                else
                {
                    fprintf( stderr, "shader_tool: error: too many -I dirs (max %d)\n",
                             SHADER_TOOL_MAX_INCLUDES );
                    bad = true;
                    ++i;
                }
            }
            else if ( strcmp( argv[ i ], "-Zi" ) == 0 )
                a.debug_info = true;
            else
            {
                fprintf( stderr, "shader_tool: error: unknown argument %s\n", argv[ i ] );
                bad = true;
            }
        }

        if ( bad || !a.out || !a.profile )
            usage();
        else
            rc = run_compile( &a );
    }
    else if ( argc >= 3 && strcmp( argv[ 1 ], "reflect" ) == 0 )
    {
        rc = run_reflect( argv[ 2 ] );
    }
    else
    {
        usage();
    }

    sys_tick_exit();
    return rc;
}

/*============================================================================================*/
