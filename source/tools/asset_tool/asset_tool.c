/*==============================================================================================

    asset_tool.c (asset cooker -- offline cook orchestrator)

    The build_tool analog for data: a job runner that dispatches a source file to a converter
    chosen by its extension. Some converters are built in (they read/write bytes directly); some
    are spawned sub-tools (font_tool bakes .ttf -> .orb_font, via sys_process_run -- the same
    child-process primitive build_tool uses to drive cl.exe).

    Usage:

        asset_tool cook <src> <dst> [args...]

            .ttf/.otf   -> spawn font_tool  <src> <size> <dst>   (args[0] = size_px, default 16)
            image/other -> built-in copy                          (cooked .tex lands in Cook-C)

    Cook-A scope (ASSET_SYSTEM_PLAN.md): single-job CLI + extension dispatch + sub-tool spawn.
    Tree scan / incremental (-src/-dst), cooked .tex, and packaging are later cook phases.

    Tools that don't need hot-reload, a service registry, or a game loop skip the module system
    entirely and call sys directly.

    Link list for this executable:

        base            (headers only -- unity built in)
        sys             (file_io, clock, process spawn -- statically linked)

    Nothing else. No core, no module system, no app.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/base.h"
#include "engine/sys/sys_host.h"

/*============================================================================================*/

#define ASSET_TOOL_DEFAULT_FONT_SIZE 16

/* path_ext -- returns a pointer to the extension (past the last '.'), or "" if none. The '.'
   must come after the last path separator so a dotted directory does not fool it. */
static const char*
path_ext( const char* path )
{
    const char* dot = NULL;
    for ( const char* p = path; *p; ++p )
    {
        if ( *p == '.' )
            dot = p;
        else if ( *p == '/' || *p == '\\' )
            dot = NULL;
    }
    return dot ? dot + 1 : "";
}

/* ext_is -- case-insensitive extension compare (ext has no leading dot). */
static bool
ext_is( const char* ext, const char* want )
{
    while ( *ext && *want )
    {
        char a = *ext, b = *want;
        if ( a >= 'A' && a <= 'Z' )
            a = ( char )( a - 'A' + 'a' );
        if ( b >= 'A' && b <= 'Z' )
            b = ( char )( b - 'A' + 'a' );
        if ( a != b )
            return false;
        ++ext;
        ++want;
    }
    return *ext == 0 && *want == 0;
}

static bool
ext_is_font( const char* ext )
{
    return ext_is( ext, "ttf" ) || ext_is( ext, "otf" );
}

/*============================================================================================*/

/* cook_copy -- built-in passthrough converter: read src, write dst verbatim. This is the
   Cook-A stand-in for the image path; the real pre-decoded .tex format is Cook-C, at which
   point this becomes the .png -> .tex converter. */
static bool
cook_copy( const char* src_path, const char* dst_path )
{
    sys_file_data_t src = sys_file_read_entire( src_path );
    if ( !src.ok )
    {
        fprintf( stderr, "asset_tool: error: could not read %s\n", src_path );
        return false;
    }

    u32  size  = src.size; /* capture before free zeroes the result */
    bool wrote = sys_file_write_entire( dst_path, src.data, src.size );
    sys_file_free( &src );

    if ( !wrote )
    {
        fprintf( stderr, "asset_tool: error: could not write %s\n", dst_path );
        return false;
    }

    printf( "asset_tool: copied %s -> %s (%u bytes)\n", src_path, dst_path, size );
    return true;
}

/* cook_font -- spawn font_tool to bake a TTF/OTF into an .orb_font atlas. font_tool lives next
   to asset_tool in bin/, so we locate it via the executable directory rather than PATH.
   font_tool's CLI is: font_tool <input.ttf> <size_px> <output.orb_font>. */
static bool
cook_font( const char* src_path, const char* dst_path, int size_px )
{
    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    /* Quote every path in case bin/ or the asset paths contain spaces. */
    char cmd[ 1536 ];
    snprintf( cmd, sizeof( cmd ), "\"%s\\font_tool.exe\" \"%s\" %d \"%s\"", exe_dir, src_path,
              size_px, dst_path );

    printf( "asset_tool: spawn %s\n", cmd );

    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) )
    {
        fprintf( stderr, "asset_tool: error: could not launch font_tool (is it built?)\n" );
        return false;
    }
    if ( res.exit_code != 0 )
    {
        fprintf( stderr, "asset_tool: error: font_tool exited %d\n", res.exit_code );
        return false;
    }

    printf( "asset_tool: baked %s -> %s (%dpx, %.0f ms)\n", src_path, dst_path, size_px,
            res.elapsed_seconds * 1000.0 );
    return true;
}

/*============================================================================================*/

/* cook_one -- dispatch a single source file to a converter chosen by its extension.
   `args`/`arg_count` are the trailing converter-specific positional arguments (e.g. font size). */
static bool
cook_one( const char* src_path, const char* dst_path, char** args, int arg_count )
{
    i64         start = sys_tick_milliseconds();
    const char* ext   = path_ext( src_path );
    bool        ok;

    if ( ext_is_font( ext ) )
    {
        int size_px = ( arg_count > 0 ) ? atoi( args[ 0 ] ) : ASSET_TOOL_DEFAULT_FONT_SIZE;
        if ( size_px <= 0 )
            size_px = ASSET_TOOL_DEFAULT_FONT_SIZE;
        ok = cook_font( src_path, dst_path, size_px );
    }
    else
    {
        /* Image and everything else: built-in copy for Cook-A. */
        ok = cook_copy( src_path, dst_path );
    }

    if ( ok )
    {
        i64 elapsed = sys_tick_milliseconds() - start;
        printf( "asset_tool: cook done (%lld ms)\n", ( long long )elapsed );
    }
    return ok;
}

/*============================================================================================*/

static int
usage( void )
{
    fprintf( stderr,
             "usage: asset_tool cook <src> <dst> [args...]\n"
             "         .ttf/.otf  -> font_tool  (args[0] = size_px, default %d)\n"
             "         image/other -> copy       (cooked .tex arrives in a later cook phase)\n",
             ASSET_TOOL_DEFAULT_FONT_SIZE );
    return 1;
}

int
main( int argc, char** argv )
{
    sys_tick_init();

    int rc = 1;
    if ( argc >= 2 && strcmp( argv[ 1 ], "cook" ) == 0 )
    {
        if ( argc < 4 )
        {
            usage();
        }
        else
        {
            bool ok = cook_one( argv[ 2 ], argv[ 3 ], argv + 4, argc - 4 );
            rc      = ok ? 0 : 1;
        }
    }
    else
    {
        usage();
    }

    sys_tick_exit();
    return rc;
}

/*============================================================================================*/
