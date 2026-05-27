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
#include "engine/sys/sys.h"

/*============================================================================================*/

static void
cook_asset( const char* src_path, const char* dst_path )
{
    /*
    uint64_t           start = clock_now_ms();

    file_read_result_t src   = file_read_entire( src_path );
    if ( !src.ok )
    {
        fprintf( stderr, "error: could not read %s\n", src_path );
        return;
    }

    // ... transform src.data ... 

    file_write_entire( dst_path, src.data, src.size );
    file_read_free( &src );

    uint64_t elapsed = clock_now_ms() - start;
    printf( "cooked %s -> %s (%llu ms)\n", src_path, dst_path, elapsed );

    */
}

/*============================================================================================*/

int
main( int argc, char** argv )
{
    printf( "Asset tool executed\n" );

    sys_tick_init();
    i64 ticks = sys_tick_milliseconds();
    printf( "ticks are: %llu\n", ticks );
    sys_tick_exit();

    if ( argc < 3 )
    {
        fprintf( stderr, "usage: cooker <src_dir> <dst_dir>\n" );
        return 1;
    }

    /*
    dir_walk_t walk = { 0 };
    while ( file_dir_walk_next( argv[ 1 ], &walk ) )
    {
        char dst[ 512 ];
        snprintf( dst, sizeof( dst ), "%s/%s", argv[ 2 ], walk.filename );
        cook_asset( walk.path, dst );
    }
    */
    return 0;
}

/*============================================================================================*/