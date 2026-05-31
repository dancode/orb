#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod_host.h"

#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"
#include "engine/ref/ref_host.h"

#include "runtime_modules/example_gen/example_gen.h"

MOD_USE_CORE;
MOD_USE_EXAMPLE_GEN;

/*============================================================================================*/
int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "=== sb_runtime_gen ===\n" );

    mod_system_init();

    ref_wire_mod_callbacks();

    // dev_hot_init( NULL, NULL );

    if ( !mod_static( sys ) )
        goto shutdown;

    if ( !mod_static( ref ) )
        goto shutdown;

    if ( !mod_static( core ) )
        goto shutdown;

    if ( !mod_load( example_gen ) )
    {
        fprintf( stderr, "load example_gen: %s\n", mod_last_error() );
        goto shutdown;
    }

    if ( mod_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", mod_last_error() );
        goto shutdown;
    }

    sid_t sid = core()->sid_intern_cstr( "test_sid_host" );

    UNUSED( sid );
    mod_list_all();

    MOD_HOST_FETCH_API( example_gen );
    example_gen()->test_function_one();
    example_gen()->test_function_two();

shutdown:

    fprintf( stderr, "%s\n", mod_last_error() );
    mod_system_exit();

    return 0;
}

/*============================================================================================*/