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

    Later phases (SHADER_SYSTEM plan) add verbs here: `reflect` (spirv-reflect dump), `cook`
    (.oshd container = reflection tables + SPIR-V), and `header` (generated C layout structs).

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

static int
usage( void )
{
    fprintf( stderr,
             "usage:\n"
             "  shader_tool compile <src.hlsl> -o <out.spv> -T <profile> [-E entry] [-I dir] [-Zi]\n"
             "      <profile>   dxc target profile: vs_6_0, ps_6_0, cs_6_0, ...\n"
             "      -E entry    entry point name (default \"main\")\n"
             "      -I dir      extra include directory (repeatable)\n"
             "      -Zi         embed debug info in the SPIR-V\n" );
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
    else
    {
        usage();
    }

    sys_tick_exit();
    return rc;
}

/*============================================================================================*/
