#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod_host.h"

#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"
#include "engine/ref/ref_host.h"

#include "sandbox/reflect/sb_gen_dll/sb_gen_dll.h"

MOD_USE_CORE;
MOD_USE_SB_GEN_DLL;

/*============================================================================================*/
int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "=== sb_gen_exe ===\n" );

    mod_system_init();

    ref_wire_mod_callbacks();

    // dev_hot_init( NULL, NULL );

    if ( !mod_static( sys ) )
        goto shutdown;

    if ( !mod_static( ref ) )
        goto shutdown;

    if ( !mod_static( core ) )
        goto shutdown;

    if ( !mod_load( sb_gen_dll ) )
    {
        fprintf( stderr, "load sb_gen_dll: %s\n", mod_last_error() );
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

    MOD_HOST_FETCH_API( sb_gen_dll );
    sb_gen_dll()->test_function_one();
    sb_gen_dll()->test_function_two();

shutdown:

    fprintf( stderr, "%s\n", mod_last_error() );
    mod_system_exit();

    return 0;
}

/*============================================================================================*/