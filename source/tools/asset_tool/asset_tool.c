/*==============================================================================================

    asset_tool.c (asset cooker — example offline tool)

    Tools that don't need hot-reload, a service registry, or a game loop
    skip the module system entirely and call sys directly.

    Link list for this executable:

        base            (headers only — unity built in)
        sys    (file_io, clock — statically linked)

    Nothing else. No core, no module system, no app.

==============================================================================================*/

#include <stdio.h>
#include "base/base.h"
#include "engine/sys/sys_host.h"

/*============================================================================================*/

/* Phase 0: the cooker does no format transform yet -- it is a straight read -> write copy that
   proves the shared sys whole-file primitive. The real converter dispatch (extension -> built-in
   or spawned sub-tool such as font_tool) is the COOK track in ASSET_SYSTEM_PLAN.md. */

static bool
cook_asset( const char* src_path, const char* dst_path )
{
    i64             start = sys_tick_milliseconds();

    sys_file_data_t src   = sys_file_read_entire( src_path );
    if ( !src.ok )
    {
        fprintf( stderr, "error: could not read %s\n", src_path );
        return false;
    }

    /* ... transform src.data here once cooked formats exist ... */

    u32  size  = src.size; /* capture before free zeroes the result */
    bool wrote = sys_file_write_entire( dst_path, src.data, src.size );
    sys_file_free( &src );

    if ( !wrote )
    {
        fprintf( stderr, "error: could not write %s\n", dst_path );
        return false;
    }

    i64 elapsed = sys_tick_milliseconds() - start;
    printf( "cooked %s -> %s (%u bytes, %lld ms)\n", src_path, dst_path, size, ( long long )elapsed );
    return true;
}

/*============================================================================================*/

int
main( int argc, char** argv )
{
    sys_tick_init();

    if ( argc < 3 )
    {
        fprintf( stderr, "usage: asset_tool <src_file> <dst_file>\n" );
        sys_tick_exit();
        return 1;
    }

    bool ok = cook_asset( argv[ 1 ], argv[ 2 ] );

    sys_tick_exit();
    return ok ? 0 : 1;
}

/*============================================================================================*/