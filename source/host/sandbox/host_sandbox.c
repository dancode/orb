/*==============================================================================================

    sandbox_main.c : sandbox dll host executable (hot reload testing modules)

==============================================================================================*/

#include <stdio.h>    // printf, fprintf
#include <string.h>

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

// #include "runtime/host/runtime_host.h"
#include "host/common/host_common.h"

/*============================================================================================*/

int
main( int argc, char** argv )
{
    // 1. Set a default payload (just in case no arguments are passed)
    const char* target_payload = "sandbox_default";

    // 2. Parse the CLI arguments
    // Example usage: sandbox_host.exe -module sandbox_imgui
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[ i ], "-module" ) == 0 && i + 1 < argc )
        {
            target_payload = argv[ i + 1 ];
            break;
        }
    }

// 3. Configure and run the runtime
    // runtime_config_t config = { .project_name  = target_payload,    // Use payload name as Window Title
    //                             .project_dll   = target_payload,    // The actual DLL to load
    //                             .window_width  = 1280,
    //                             .window_height = 720 };

    // return runtime_host_run( &config );

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

