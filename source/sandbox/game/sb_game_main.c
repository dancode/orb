/*==============================================================================================

    sb_game_main.c : sandbox game executable

    What it does: It bootstraps the engine, starts the runtime, and initializes the 
    GAME modules (world, entity, actor, tags).

    It does not load project.dll 

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/mod/mod_api.h"

/* declare static linked modules */
#include "engine/core/core_api.h"
#include "engine/sys/sys_api.h"

/* for dynamic module access frp, host -- skip for static modules */
#include "runtime_module/example/example_api.h"
#include "runtime_module/render/render_api.h"
MOD_DEFINE_API_PTR( example_api_t, example );
MOD_DEFINE_API_PTR( render_api_t, render );

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
    if ( !mod_load( example ) )
        return 1;

    if ( !mod_load( render ) )
        return 1;

    if ( mod_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", mod_last_error() );
        return 1;
    }
    mod_list_all();

    /* 2. API binding */

    HOST_FETCH_API( example_api_t, example );
    HOST_FETCH_API( render_api_t, render );

    /* 3. Main loop */

    example_api()->example_function_1();
    core_api()->log( "Hello from the host executable!" );
    render_api()->begin_frame();

    /* 4. Shutdown */
    mod_system_exit();

    return 0;
}

/*============================================================================================*/