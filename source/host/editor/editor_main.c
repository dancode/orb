/*==============================================================================================

    editor_main.c : Editor host executable for the module system.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/mod/mod_api.h"

/* declare static linked modules */
#include "engine/core/core_api.h"
#include "engine/sys/sys_api.h"

/* for dynamic module access frp, host -- skip for static modules */
// #include "runtime_modules/example/example_api.h"
// MOD_DEFINE_API_PTR( example_api_t, example );

/*============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* 1. Boot the module system, register static modules, load dynamic ones, and initialize all. */

    mod_system_init();
    mod_static_load( "sys", sys_get_mod_api() );
    mod_static_load( "core", core_get_mod_api() );

    if ( 0 )    // if ( !mod_load( example ))
    {
        return 1;
    }
    if ( mod_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", mod_last_error() );
        return 1;
    }

    mod_list_all();

    /* 2. API binding */

    // HOST_FETCH_API( example_api_t, example );

    /* 2. Main loop */

    // example_api()->example_function_1();
    core_api()->log( "Hello from the host executable!" );

    /* 3. Shutdown */
    mod_system_exit();

    return 0;
}

/*============================================================================================*/