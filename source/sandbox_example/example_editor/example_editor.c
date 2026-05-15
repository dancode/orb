/*==============================================================================================

    sandbox_editor_main.c — EDITOR / DEV SANDBOX shape.

    Windowed, hot-reloadable, console-assisted. The primary development environment:
    edit a module, rebuild the DLL, watch it swap in while the window keeps running.

    The window close button quits via app_api()->pump_events() returning false.
    Q on the keyboard is an alternative quit for keyboard-first workflows.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/sys/sys.h"
#include "engine/mod/mod_host.h"
#include "engine/app/app.h"

#include "runtime/run_host.h"

#include "runtime_module/example/example.h"
MOD_DEFINE_API_PTR( example_api_t, example );

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
editor_ready( void )
{
    HOST_FETCH_API( example_api_t, example );

    printf( "Keys: Q=quit  R=reload all  F=arm failure  V=verify\n" );
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
        example_api()->fail_next_reload();

    if ( sys_key_pressed( PLATFORM_KEY_V ) )
    {
        printf( "[editor] V — verify\n" );
        example_api()->example_function_1();
    }

    example_api()->update( dt );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = { RUN_SERVICE( app ),    /* window, OS pump */
                                                RUN_MODULE( render ),  /* renderer — null-safe if not present */
                                                RUN_MODULE( example ),
                                                { 0 } };

static const run_host_desc_t    k_desc      = {
            .name      = "sandbox_editor",
            .flags     = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD,
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