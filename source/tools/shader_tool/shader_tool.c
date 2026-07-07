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

        shader_tool reflect <file.spv | file.oshd>
            Human-readable dump of what the RHI needs to agree with the shader: entry point,
            stage, vertex inputs (location/format), push constant block layout (member
            offsets/sizes), and descriptor bindings (set/binding/type/count).  A raw .spv is
            reflected live via the vendored SPIRV-Reflect library
            (shader_tool_spirv_reflect.c); a cooked .oshd is dumped from its serialized
            tables -- both paths print the same sections, which is the round-trip proof that
            cooking loses nothing.

        shader_tool cook <src.hlsl> -o <out.oshd> -T <profile> [-E entry] [-I dir] [-Zi]
            compile + reflect + serialize in one step: dxc compiles the HLSL (same conventions
            as `compile`), SPIRV-Reflect extracts the layout contract, and everything lands in
            a single .oshd container (rhi_shader_format.h) -- reflection tables, layout hash,
            and the SPIR-V payload.  One stage per file.

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

    Later phases (SHADER_SYSTEM plan) add verbs here: `header` (generated C layout structs).

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
#include "runtime_service/rhi/rhi_shader_format.h"
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

/* oshd_stage_name -- printable OSHD_STAGE_* value (the cooked-container twin of stage_name). */
static const char*
oshd_stage_name( u32 stage )
{
    switch ( stage )
    {
        case OSHD_STAGE_VERTEX:       return "vertex";
        case OSHD_STAGE_TESS_CONTROL: return "tess_control";
        case OSHD_STAGE_TESS_EVAL:    return "tess_eval";
        case OSHD_STAGE_GEOMETRY:     return "geometry";
        case OSHD_STAGE_FRAGMENT:     return "fragment";
        case OSHD_STAGE_COMPUTE:      return "compute";
        default:                      return "unknown";
    }
}

/* collect_user_inputs -- enumerate a module's input variables, drop built-ins (SV_VertexID
   and friends are internal wiring, not vertex buffer contract), and insertion-sort the
   survivors by location so consumers see a vertex layout declaration regardless of the order
   dxc emitted them.  Returns the user input count; *out is malloc'd (caller frees) or NULL
   when the count is 0. */
static u32
collect_user_inputs( const SpvReflectShaderModule* module, SpvReflectInterfaceVariable*** out )
{
    u32 count = 0;
    spvReflectEnumerateInputVariables( module, &count, NULL );

    SpvReflectInterfaceVariable** inputs = NULL;
    if ( count )
    {
        inputs = ( SpvReflectInterfaceVariable** )malloc( count * sizeof( *inputs ) );
        spvReflectEnumerateInputVariables( module, &count, inputs );
    }

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

    *out = inputs;
    return user_inputs;
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

/* oshd_str -- bounds-checked string table fetch; a bad offset degrades to "" rather than
   walking off the mapping (the table's final NUL is validated before any fetch). */
static const char*
oshd_str( const char* strtab, u32 strtab_size, u32 off )
{
    return ( off < strtab_size ) ? strtab + off : "";
}

/* run_reflect_oshd -- print the same dump as the raw-SPIR-V path, but sourced from the
   container's serialized tables.  That the two dumps match line for line is the proof that
   cooking preserved the whole layout contract. */
static int
run_reflect_oshd( const char* path, const sys_file_data_t* fd )
{
    const oshd_header_t* h = ( const oshd_header_t* )fd->data;

    if ( h->version != OSHD_VERSION )
    {
        fprintf( stderr, "shader_tool: error: %s is .oshd version %u, tool expects %u\n",
                 path, h->version, OSHD_VERSION );
        return 1;
    }

    /* Validate the section math in u64 before trusting any count or offset. */
    u64 need = ( u64 )sizeof( oshd_header_t )
             + ( u64 )h->input_count * sizeof( oshd_input_t )
             + ( u64 )h->pc_member_count * sizeof( oshd_pc_member_t )
             + ( u64 )h->binding_count * sizeof( oshd_binding_t )
             + ( u64 )h->strtab_size + ( u64 )h->spirv_size;
    if ( need != ( u64 )fd->size ||
         h->strtab_size < 4 || h->strtab_size % 4 != 0 ||
         h->spirv_size == 0 || h->spirv_size % 4 != 0 )
    {
        fprintf( stderr, "shader_tool: error: %s is corrupt (%u bytes, sections need %llu)\n",
                 path, fd->size, ( unsigned long long )need );
        return 1;
    }

    const oshd_input_t*     inputs  = ( const oshd_input_t* )( h + 1 );
    const oshd_pc_member_t* members = ( const oshd_pc_member_t* )( inputs + h->input_count );
    const oshd_binding_t*   binds   = ( const oshd_binding_t* )( members + h->pc_member_count );
    const char*             strtab  = ( const char* )( binds + h->binding_count );

    if ( strtab[ h->strtab_size - 1 ] != 0 )
    {
        fprintf( stderr, "shader_tool: error: %s string table is not NUL-terminated\n", path );
        return 1;
    }

    printf( "shader_tool: reflect %s (.oshd v%u, %u bytes, spirv %u bytes, layout hash %016llx)\n",
            path, h->version, fd->size, h->spirv_size, ( unsigned long long )h->layout_hash );
    printf( "  stage : %s\n", oshd_stage_name( h->stage ) );

    const char* entry = oshd_str( strtab, h->strtab_size, h->entry );
    printf( "  entry : %s\n", entry[ 0 ] ? entry : "(none)" );

    printf( "  inputs: %u\n", h->input_count );
    for ( u32 i = 0; i < h->input_count; ++i )
    {
        const oshd_input_t* v    = &inputs[ i ];
        const char*         fmt  = format_name( ( SpvReflectFormat )v->vk_format );
        const char*         name = oshd_str( strtab, h->strtab_size, v->name );
        if ( fmt )
            printf( "      [location %2u] %-8s %s\n", v->location, fmt,
                    name[ 0 ] ? name : "(unnamed)" );
        else
            printf( "      [location %2u] vkfmt=%-4d %s\n", v->location, ( int )v->vk_format,
                    name[ 0 ] ? name : "(unnamed)" );
    }

    u32 block_count = ( h->pc_size > 0 ) ? 1 : 0;
    printf( "  push constant blocks: %u\n", block_count );
    if ( block_count )
    {
        const char* bname = oshd_str( strtab, h->strtab_size, h->pc_name );
        printf( "      block \"%s\" -- size %u, padded %u\n",
                bname[ 0 ] ? bname : "(unnamed)", h->pc_size, h->pc_padded_size );
        for ( u32 i = 0; i < h->pc_member_count; ++i )
        {
            const oshd_pc_member_t* m    = &members[ i ];
            const char*             name = oshd_str( strtab, h->strtab_size, m->name );
            printf( "  %*s[offset %4u, size %4u] %s\n", 4 + 4 * m->depth, "", m->offset,
                    m->size, name[ 0 ] ? name : "(unnamed)" );
        }
    }

    printf( "  descriptor bindings: %u\n", h->binding_count );
    for ( u32 i = 0; i < h->binding_count; ++i )
    {
        const oshd_binding_t* b = &binds[ i ];
        printf( "      [set %u, binding %u] %s x%u %s\n", b->set, b->binding,
                descriptor_type_name( ( SpvReflectDescriptorType )b->descriptor_type ),
                b->count, oshd_str( strtab, h->strtab_size, b->name ) );
    }

    return 0;
}

/* run_reflect -- load a compiled blob and print everything the RHI must agree with.  A
   cooked .oshd dumps from its tables; anything else is treated as raw SPIR-V and reflected
   live. */
static int
run_reflect( const char* path )
{
    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
    {
        fprintf( stderr, "shader_tool: error: could not read %s\n", path );
        return 1;
    }

    if ( fd.size >= sizeof( oshd_header_t ) &&
         ( ( const oshd_header_t* )fd.data )->magic == OSHD_MAGIC )
    {
        int rc = run_reflect_oshd( path, &fd );
        sys_file_free( &fd );
        return rc;
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

    /* Vertex inputs, filtered and sorted (see collect_user_inputs). */
    SpvReflectInterfaceVariable** inputs      = NULL;
    u32                           user_inputs = collect_user_inputs( &module, &inputs );

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
    u32 count = 0;
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
/*  cook verb                                                                                 */
/*============================================================================================*/

/* The layout hash fingerprints every field the CPU side must agree with.  FNV-1a 64,
   accumulated in file order; strings are folded in with their terminator so adjacent names
   cannot alias.  Only the cooker computes this -- readers just compare stored hashes. */

static u64
fnv_u32( u64 h, u32 v )
{
    for ( int i = 0; i < 4; ++i )
    {
        h ^= ( v >> ( i * 8 ) ) & 0xFF;
        h *= 0x100000001b3ull;
    }
    return h;
}

static u64
fnv_str( u64 h, const char* s )
{
    do
    {
        h ^= ( u64 )( u8 )*s;
        h *= 0x100000001b3ull;
    } while ( *s++ );
    return h;
}

/* strtab_t -- string table builder.  Offset 0 is a reserved empty string so 0 always reads
   back as "unnamed"; identical names are deduplicated by linear scan (tables are tiny). */
#define OSHD_STRTAB_CAP ( 64 * 1024 )

typedef struct strtab_s
{
    char* buf;
    u32   size;
    bool  overflow;

} strtab_t;

static u32
strtab_add( strtab_t* st, const char* s )
{
    if ( !s || !s[ 0 ] )
        return 0;

    size_t len = strlen( s ) + 1;
    for ( u32 off = 1; off + len <= st->size; )
    {
        if ( strcmp( st->buf + off, s ) == 0 )
            return off;
        off += ( u32 )strlen( st->buf + off ) + 1;
    }

    if ( st->size + len > OSHD_STRTAB_CAP )
    {
        st->overflow = true;
        return 0;
    }
    u32 off = st->size;
    memcpy( st->buf + off, s, len );
    st->size += ( u32 )len;
    return off;
}

/* spv_format_size -- byte size of a vertex attribute format (stride derivation).  Only the
   32/64-bit scalar and vector families dxc emits for vertex inputs; 0 = unsupported, which
   the cooker treats as a hard error rather than serializing a size it cannot vouch for. */
static u32
spv_format_size( SpvReflectFormat fmt )
{
    switch ( fmt )
    {
        case SPV_REFLECT_FORMAT_R32_SFLOAT:
        case SPV_REFLECT_FORMAT_R32_UINT:
        case SPV_REFLECT_FORMAT_R32_SINT:            return 4;
        case SPV_REFLECT_FORMAT_R32G32_SFLOAT:
        case SPV_REFLECT_FORMAT_R32G32_UINT:
        case SPV_REFLECT_FORMAT_R32G32_SINT:         return 8;
        case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:
        case SPV_REFLECT_FORMAT_R32G32B32_UINT:
        case SPV_REFLECT_FORMAT_R32G32B32_SINT:      return 12;
        case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT:
        case SPV_REFLECT_FORMAT_R32G32B32A32_UINT:
        case SPV_REFLECT_FORMAT_R32G32B32A32_SINT:   return 16;
        case SPV_REFLECT_FORMAT_R64_SFLOAT:          return 8;
        case SPV_REFLECT_FORMAT_R64G64_SFLOAT:       return 16;
        case SPV_REFLECT_FORMAT_R64G64B64_SFLOAT:    return 24;
        case SPV_REFLECT_FORMAT_R64G64B64A64_SFLOAT: return 32;
        default:                                     return 0;
    }
}

/* oshd_stage_from_reflect -- SpvReflect stage bit -> OSHD_STAGE_* (0 = unsupported). */
static u32
oshd_stage_from_reflect( SpvReflectShaderStageFlagBits stage )
{
    switch ( stage )
    {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:                  return OSHD_STAGE_VERTEX;
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return OSHD_STAGE_TESS_CONTROL;
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return OSHD_STAGE_TESS_EVAL;
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:                return OSHD_STAGE_GEOMETRY;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:                return OSHD_STAGE_FRAGMENT;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:                 return OSHD_STAGE_COMPUTE;
        default:                                                   return OSHD_STAGE_NONE;
    }
}

/* pc_member_total -- pre-order record count for a block's member tree. */
static u32
pc_member_total( const SpvReflectBlockVariable* b )
{
    u32 n = 0;
    for ( u32 i = 0; i < b->member_count; ++i )
        n += 1 + pc_member_total( &b->members[ i ] );
    return n;
}

/* pc_member_fill -- flatten a block's member tree pre-order into the serialized table.
   Offsets are absolute within the block, matching what `reflect` prints. */
static void
pc_member_fill( const SpvReflectBlockVariable* b, u32 depth, oshd_pc_member_t* out, u32* idx,
                strtab_t* st )
{
    for ( u32 i = 0; i < b->member_count; ++i )
    {
        const SpvReflectBlockVariable* m   = &b->members[ i ];
        oshd_pc_member_t*              rec = &out[ ( *idx )++ ];
        rec->name_hash = oshd_name_hash( m->name ? m->name : "" );
        rec->name      = strtab_add( st, m->name );
        rec->offset    = m->absolute_offset;
        rec->size      = m->size;
        rec->depth     = depth;
        pc_member_fill( m, depth + 1, out, idx, st );
    }
}

/* run_cook -- compile + reflect + serialize into a single .oshd container.  dxc writes to a
   temp .spv next to the output (deleted on every exit path); the container is assembled in
   one heap block and written atomically via sys_file_write_entire. */
static int
run_cook( const compile_args_t* a )
{
    char tmp[ 1024 ];
    snprintf( tmp, sizeof( tmp ), "%s.tmp.spv", a->out );

    compile_args_t ca = *a;
    ca.out            = tmp;
    if ( run_compile( &ca ) != 0 )
        return 1;

    int                           rc      = 1;
    bool                          have    = false;
    SpvReflectShaderModule        module;
    SpvReflectInterfaceVariable** inputs  = NULL;
    oshd_input_t*                 in_tab  = NULL;
    oshd_pc_member_t*             pc_tab  = NULL;
    oshd_binding_t*               bd_tab  = NULL;
    SpvReflectDescriptorBinding** binds   = NULL;
    u8*                           file    = NULL;
    strtab_t                      st      = { 0 };

    sys_file_data_t fd = sys_file_read_entire( tmp );
    if ( !fd.ok || fd.size == 0 || fd.size % 4 != 0 )
    {
        fprintf( stderr, "shader_tool: error: could not read back %s\n", tmp );
        goto done;
    }

    if ( spvReflectCreateShaderModule( fd.size, fd.data, &module ) != SPV_REFLECT_RESULT_SUCCESS )
    {
        fprintf( stderr, "shader_tool: error: SPIR-V reflection failed for %s\n", a->src );
        goto done;
    }
    have = true;

    oshd_header_t h = { 0 };
    h.magic         = OSHD_MAGIC;
    h.version       = OSHD_VERSION;
    h.stage         = oshd_stage_from_reflect( module.shader_stage );
    h.spirv_size    = fd.size;
    if ( h.stage == OSHD_STAGE_NONE )
    {
        fprintf( stderr, "shader_tool: error: unsupported shader stage in %s\n", a->src );
        goto done;
    }

    st.buf      = ( char* )calloc( 1, OSHD_STRTAB_CAP );
    st.size     = 1;    /* offset 0 reserved as "" */
    h.entry     = strtab_add( &st, module.entry_point_name );

    /* Vertex inputs. */
    h.input_count = collect_user_inputs( &module, &inputs );
    if ( h.input_count )
        in_tab = ( oshd_input_t* )calloc( h.input_count, sizeof( *in_tab ) );
    for ( u32 i = 0; i < h.input_count; ++i )
    {
        const SpvReflectInterfaceVariable* v = inputs[ i ];
        u32                                sz = spv_format_size( v->format );
        if ( sz == 0 )
        {
            fprintf( stderr, "shader_tool: error: unsupported vertex input format %d (%s) in %s\n",
                     ( int )v->format, v->name ? v->name : "(unnamed)", a->src );
            goto done;
        }
        in_tab[ i ].location  = v->location;
        in_tab[ i ].vk_format = ( u32 )v->format;
        in_tab[ i ].size      = sz;
        in_tab[ i ].name      = strtab_add( &st, v->name );
    }

    /* Push constant block (dxc permits at most one per stage; enforce it here so the header's
       single-block fields are never ambiguous). */
    u32 pc_count = 0;
    spvReflectEnumeratePushConstantBlocks( &module, &pc_count, NULL );
    if ( pc_count > 1 )
    {
        fprintf( stderr, "shader_tool: error: %u push constant blocks in %s (max 1)\n",
                 pc_count, a->src );
        goto done;
    }
    if ( pc_count == 1 )
    {
        SpvReflectBlockVariable* block = NULL;
        spvReflectEnumeratePushConstantBlocks( &module, &pc_count, &block );

        const char* bname = block->type_description && block->type_description->type_name
                              ? block->type_description->type_name
                              : block->name;
        h.pc_name        = strtab_add( &st, bname );
        h.pc_size        = block->size;
        h.pc_padded_size = block->padded_size;
        h.pc_member_count = pc_member_total( block );
        if ( h.pc_member_count )
        {
            pc_tab  = ( oshd_pc_member_t* )calloc( h.pc_member_count, sizeof( *pc_tab ) );
            u32 idx = 0;
            pc_member_fill( block, 0, pc_tab, &idx, &st );
        }
    }

    /* Descriptor bindings, as reflected. */
    spvReflectEnumerateDescriptorBindings( &module, &h.binding_count, NULL );
    if ( h.binding_count )
    {
        binds  = ( SpvReflectDescriptorBinding** )malloc( h.binding_count * sizeof( *binds ) );
        bd_tab = ( oshd_binding_t* )calloc( h.binding_count, sizeof( *bd_tab ) );
        spvReflectEnumerateDescriptorBindings( &module, &h.binding_count, binds );
        for ( u32 i = 0; i < h.binding_count; ++i )
        {
            bd_tab[ i ].set             = binds[ i ]->set;
            bd_tab[ i ].binding         = binds[ i ]->binding;
            bd_tab[ i ].descriptor_type = ( u32 )binds[ i ]->descriptor_type;
            bd_tab[ i ].count           = binds[ i ]->count;
            bd_tab[ i ].name            = strtab_add( &st, binds[ i ]->name );
        }
    }

    if ( st.overflow )
    {
        fprintf( stderr, "shader_tool: error: string table exceeds %d bytes for %s\n",
                 OSHD_STRTAB_CAP, a->src );
        goto done;
    }

    /* Pad the string table so the SPIR-V payload stays 4-aligned in the mapped file. */
    while ( st.size % 4 != 0 )
        st.buf[ st.size++ ] = 0;
    h.strtab_size = st.size;

    /* Layout hash: every CPU-visible contract field, in file order, names included. */
    {
        u64 lh = 0xcbf29ce484222325ull;
        lh     = fnv_u32( lh, h.stage );
        lh     = fnv_str( lh, oshd_str( st.buf, st.size, h.entry ) );
        for ( u32 i = 0; i < h.input_count; ++i )
        {
            lh = fnv_u32( lh, in_tab[ i ].location );
            lh = fnv_u32( lh, in_tab[ i ].vk_format );
            lh = fnv_u32( lh, in_tab[ i ].size );
            lh = fnv_str( lh, oshd_str( st.buf, st.size, in_tab[ i ].name ) );
        }
        lh = fnv_u32( lh, h.pc_size );
        lh = fnv_u32( lh, h.pc_padded_size );
        lh = fnv_str( lh, oshd_str( st.buf, st.size, h.pc_name ) );
        for ( u32 i = 0; i < h.pc_member_count; ++i )
        {
            lh = fnv_u32( lh, pc_tab[ i ].offset );
            lh = fnv_u32( lh, pc_tab[ i ].size );
            lh = fnv_u32( lh, pc_tab[ i ].depth );
            lh = fnv_str( lh, oshd_str( st.buf, st.size, pc_tab[ i ].name ) );
        }
        for ( u32 i = 0; i < h.binding_count; ++i )
        {
            lh = fnv_u32( lh, bd_tab[ i ].set );
            lh = fnv_u32( lh, bd_tab[ i ].binding );
            lh = fnv_u32( lh, bd_tab[ i ].descriptor_type );
            lh = fnv_u32( lh, bd_tab[ i ].count );
            lh = fnv_str( lh, oshd_str( st.buf, st.size, bd_tab[ i ].name ) );
        }
        h.layout_hash = lh;
    }

    /* Assemble and write.  Sizes are u32 in the header; the u64 total guards the sum. */
    u64 total = ( u64 )sizeof( h )
              + ( u64 )h.input_count * sizeof( oshd_input_t )
              + ( u64 )h.pc_member_count * sizeof( oshd_pc_member_t )
              + ( u64 )h.binding_count * sizeof( oshd_binding_t )
              + ( u64 )h.strtab_size + ( u64 )h.spirv_size;
    if ( total > 0xFFFFFFFFull )
    {
        fprintf( stderr, "shader_tool: error: cooked size overflows u32 for %s\n", a->src );
        goto done;
    }

    file   = ( u8* )malloc( ( size_t )total );
    u8* at = file;
    memcpy( at, &h, sizeof( h ) );                                        at += sizeof( h );
    if ( h.input_count )
    {
        memcpy( at, in_tab, h.input_count * sizeof( oshd_input_t ) );
        at += h.input_count * sizeof( oshd_input_t );
    }
    if ( h.pc_member_count )
    {
        memcpy( at, pc_tab, h.pc_member_count * sizeof( oshd_pc_member_t ) );
        at += h.pc_member_count * sizeof( oshd_pc_member_t );
    }
    if ( h.binding_count )
    {
        memcpy( at, bd_tab, h.binding_count * sizeof( oshd_binding_t ) );
        at += h.binding_count * sizeof( oshd_binding_t );
    }
    memcpy( at, st.buf, h.strtab_size );                                  at += h.strtab_size;
    memcpy( at, fd.data, h.spirv_size );                                  at += h.spirv_size;

    if ( !sys_file_write_entire( a->out, file, ( u32 )total ) )
    {
        fprintf( stderr, "shader_tool: error: could not write %s\n", a->out );
        goto done;
    }

    printf( "shader_tool: cook %s -> %s (%s, spirv %u bytes, %u inputs, pc %u bytes, "
            "%u bindings, layout hash %016llx)\n",
            a->src, a->out, oshd_stage_name( h.stage ), h.spirv_size, h.input_count, h.pc_size,
            h.binding_count, ( unsigned long long )h.layout_hash );
    rc = 0;

done:
    free( file );
    free( st.buf );
    free( bd_tab );
    free( binds );
    free( pc_tab );
    free( in_tab );
    free( inputs );
    if ( have )
        spvReflectDestroyShaderModule( &module );
    sys_file_free( &fd );
    sys_file_delete( tmp );
    return rc;
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
             "  shader_tool cook <src.hlsl> -o <out.oshd> -T <profile> [-E entry] [-I dir] [-Zi]\n"
             "      compile + reflect + serialize into a cooked .oshd container\n"
             "  shader_tool reflect <file.spv | file.oshd>\n"
             "      dump stage, entry, inputs, push constants, descriptor bindings\n" );
    return 1;
}

int
main( int argc, char** argv )
{
    sys_tick_init();

    int rc = 1;

    bool is_compile = argc >= 3 && strcmp( argv[ 1 ], "compile" ) == 0;
    bool is_cook    = argc >= 3 && strcmp( argv[ 1 ], "cook" ) == 0;

    if ( is_compile || is_cook )
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
            rc = is_cook ? run_cook( &a ) : run_compile( &a );
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
