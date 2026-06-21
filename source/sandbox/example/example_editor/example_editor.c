/*==============================================================================================

    sandbox_editor_main.c — EDITOR / DEV SANDBOX shape.

    Windowed, hot-reloadable, console-assisted. The primary development environment:
    edit a module, rebuild the DLL, watch it swap in while the window keeps running.

    The window close button quits via app()->pump_events() returning false.
    Q on the keyboard is an alternative quit for keyboard-first workflows.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/sys/sys_host.h"
#include "engine/mod/mod_host.h"


#include "engine/app/app_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_modules/render/render_api.h"
#include "runtime_modules/example/example_api.h"

#include "runtime/runtime_api.h"
#include "runtime/runtime_host.h"


MOD_USE_APP;
MOD_USE_RUN;
// MOD_USE_RENDER;
MOD_USE_EXAMPLE;


/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
editor_ready( void )
{
    MOD_HOST_FETCH_API( example );

    printf( "Keys: Q=quit  R=reload all  F=arm failure  V=verify  D=toggle sleep debug\n" );
}

static void
editor_update( f32 dt )
{
    /* keyboard quit — redundant with window-close but useful in a headless run */
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[editor] Q — quit\n" );
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[editor] R — reload all\n" );
        mod_reload_all();
    }

    if ( sys_key_pressed( PLATFORM_KEY_F ) )
        example()->fail_next_reload();

    if ( sys_key_pressed( PLATFORM_KEY_V ) )
    {
        printf( "[editor] V — verify\n" );
        example()->example_function_1();
    }

    if ( sys_key_pressed( PLATFORM_KEY_D ) )
        run_host_sleep_debug_toggle();

    example()->update( dt );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( app     ),   /* window, OS pump */
    RUN_SERVICE( rhi     ),   /* GPU backend — static service */
    RUN_MODULE ( render  ),   /* renderer — hot-reloadable DLL */
    RUN_MODULE ( example ),
    { 0 }
};

static const run_host_desc_t    k_desc      = {
            .name      = "sandbox_editor",
            .flags     = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_EDITOR_SLEEP,
            .loop_mode = RUN_LOOP_RUN,
            .modules   = k_modules,
            .on_ready  = editor_ready,
            .on_update = editor_update,
};

int
main( int argc, char** argv )
{
    return run_host_main( &k_desc, argc, argv );
}

/*============================================================================================*/