/*==============================================================================================

    runtime/host/runtime_host.c — Runtime host orchestrator implementation.

    Frame timing
    ------------
    sys_tick_reset() is called once to prime the counter before the loop. Each
    iteration measures elapsed time with sys_tick_milliseconds() (which reads
    since the last reset without resetting), sleeps the remainder, then calls
    sys_tick_reset() to latch the full frame duration as dt for the next tick.

    dt on the first frame is set to the frame target — a reasonable estimate
    that avoids a zero or garbage value reaching on_update.

    Hot-reload ordering
    -------------------
    mod_check_reloads() is called AFTER on_update() so that any API calls made
    during the update frame are finished before a DLL swap can occur.
    mod_system_flush_reloads() commits the swap at the defined quiescent point.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_api.h"
#include "engine/mod/mod.h"
#include "runtime/host/runtime_host.h"

/*==============================================================================================
    Internal helpers: wrap module loading via a configuration struct.
==============================================================================================*/

static bool
internal_load_modules( const rt_config_t* config )
{
    if ( !config->modules )
        return true;

    for ( int i = 0; config->modules[ i ].name != NULL; ++i )
    {
        const rt_module_entry_t* entry = &config->modules[ i ];
        bool                     ok;

#ifdef BUILD_STATIC

        ORB_ASSERT_MSG( entry->get_api != NULL,
                        "RUNTIME_MODULE entry missing get_api in BUILD_STATIC — "
                        "include the module's API header at the call site" );

        ok = mod_static_load( entry->name, entry->get_api() );
#else
        ok = mod_dynamic_load( entry->name );
#endif
        if ( ok == false )
        {
            fprintf( stderr, "[runtime:%s] failed to load module '%s': %s\n", config->host_name, entry->name,
                     mod_last_error() );
            return false;
        }
    }
    return true; /* we successfully loaded the modules */
}

/*==============================================================================================

    Public: runtime_host_run

==============================================================================================*/

bool
runtime_host_run( const rt_config_t* config )
{
    ORB_ASSERT_MSG( config != NULL, "runtime_host_run: config is NULL" );
    ORB_ASSERT_MSG( config->host_name != NULL, "runtime_host_run: host_name is NULL" );
    ORB_ASSERT_MSG( config->on_update != NULL, "runtime_host_run: on_update is required" );

    i32 frame_ms = ( config->frame_target_ms > 0 ) ? config->frame_target_ms : RUNTIME_DEFAULT_FRAME_MS;

    printf( "[runtime:%s] starting\n", config->host_name );

    /*--------------------------------------------------
        1. Boot module system
    --------------------------------------------------*/

    mod_system_init();

    /*--------------------------------------------------
        2. sys — always static; required by module system
    --------------------------------------------------*/

    if ( !mod_static_load( "sys", sys_get_mod_api() ) )
    {
        fprintf( stderr, "[runtime:%s] failed to register sys: %s\n", config->host_name, mod_last_error() );
        goto fail;
    }

    /*--------------------------------------------------
        3. core — optional; needs core_api.h once ready
    --------------------------------------------------*/

    if ( config->load_core == true )
    {
        /* TODO: mod_static_load( "core", core_get_mod_api() )
           Uncomment when core_api.h is available and core is ready to init.
           Also add: #include "engine/core/core_api.h" at the top of this file. */
        fprintf( stderr, "[runtime:%s] load_core requested but not yet wired — ignoring\n", config->host_name );
    }

    /*--------------------------------------------------
        4. Host-configured modules
    --------------------------------------------------*/

    if ( internal_load_modules( config ) == false )
    {
        goto fail;
    }

    /*--------------------------------------------------
        5. Init all in dependency order
    --------------------------------------------------*/

    if ( mod_init_all() == false )
    {
        fprintf( stderr, "[runtime:%s] mod_init_all failed: %s\n", config->host_name, mod_last_error() );
        goto fail;
    }

    mod_list_all();

    /*--------------------------------------------------
        6. Host init callback
    --------------------------------------------------*/

    if ( config->on_init && config->on_init( config->userdata ) == false )
    {
        fprintf( stderr, "[runtime:%s] on_init returned false — aborting\n", config->host_name );
        if ( config->on_exit )
            config->on_exit( config->userdata );
        mod_system_exit();
        return false;
    }

    /*--------------------------------------------------
        7. Main loop
    --------------------------------------------------*/

    /* Prime the timer. dt on the first frame is the configured target — a
       reasonable estimate rather than a zero or garbage value. */

    sys_api()->tick_reset();
    float dt = ( float )frame_ms / 1000.0f;

    while ( config->on_update( dt, config->userdata ) )
    {
        /* Hot-reload: check after update so no in-flight calls straddle a swap. */
        if ( config->enable_hot_reload )
        {
            mod_check_reloads();
            mod_system_flush_reloads();
        }

        /* Frame pacing: sleep the remainder of the target window. */
        i64 elapsed_ms = sys_api()->tick_milliseconds();
        i32 sleep_ms   = frame_ms - ( i32 )elapsed_ms;
        if ( sleep_ms > 0 )
            sys_api()->sleep_milliseconds( sleep_ms );

        /* Latch the full frame duration (update + sleep) for next tick's dt. */
        dt = ( float )sys_api()->tick_reset();
    }

    /*--------------------------------------------------
        8 & 9. Exit callbacks + shutdown
    --------------------------------------------------*/

    if ( config->on_exit )
        config->on_exit( config->userdata );

    mod_system_exit();
    printf( "[runtime:%s] clean shutdown\n", config->host_name );
    return true;

fail:
    mod_system_exit();
    return false;
}

/*==============================================================================================
    OLD VERSION
==============================================================================================*/

bool
runtime_host_run_old( const rt_config_t* config )
{
    UNUSED( config );

    // ---------------------------------------------------------

    // // Register the static foundation modules
    // mod_static_load( "core", core_get_mod_api() );
    // mod_static_load( "sys", sys_get_mod_api() );
    //
    // // Only register the windowing application layer if we aren't headless
    // if ( config->is_headless == false )
    // {
    //     mod_static_load( "app", app_get_mod_api() );
    // }
    //
    // // ---------------------------------------------------------
    // // 2. Load the Payloads
    // // ---------------------------------------------------------
    // // In the future, this list would be populated by reading a project.json,
    // // but for now, we load the standard dynamic payloads.
    //
    // if ( !config->is_headless )
    // {
    //     mod_load( "render" );
    //     mod_load( "audio" );
    // }
    //
    // // Load the specific project/sandbox DLL requested by the CLI
    // if ( config->project_dll && config->project_dll[ 0 ] != '\0' )
    // {
    //     mod_load( config->project_dll );
    // }
    //
    // // ---------------------------------------------------------
    // // 3. Resolve Dependencies and Initialize
    // // ---------------------------------------------------------
    // if ( !mod_init_all() )
    // {
    //     // If core_api isn't loaded yet, fallback to standard printf for critical failures
    //     // sys_api()->print_console( "CRITICAL: Failed to initialize module graph.\n" );
    //     return -1;
    // }

    // core_api()->log_info( "Runtime: Module graph initialized successfully." );

    /*
    *
    // ---------------------------------------------------------
    // 4. Setup the Application Window (If applicable)
    // ---------------------------------------------------------
    if ( !config->is_headless )
    {
        app_config_t app_cfg = {
            .title = config->project_name, .width = config->window_width, .height = config->window_height };
        app_api()->window_create( &app_cfg );
    }

    // ---------------------------------------------------------
    // 5. Timekeeping Setup
    // ---------------------------------------------------------
    uint64_t last_time = sys_api()->time_now_us();
    float    dt        = 0.016f;    // Seed with 60FPS
    bool     running   = true;

    core_api()->log_info( "Runtime: Entering main loop." );

    // ---------------------------------------------------------
    // 6. THE MAIN LOOP
    // ---------------------------------------------------------
    while ( running )
    {
        // A. Calculate Delta Time
        uint64_t current_time = sys_api()->time_now_us();
        dt                    = ( float )( current_time - last_time ) / 1000000.0f;
        last_time             = current_time;

        // B. Cap DT (Prevents physics explosions if the window is dragged/paused)
        if ( dt > 0.1f )
        {
            dt = 0.1f;
        }

        // C. Check File Timestamps for Hot-Reloads
        module_check_reloads();

        // D. OS Event Pumping
        if ( !config->is_headless )
        {
            app_api()->pump_events();
            if ( !app_api()->is_running() )
            {
                running = false;
            }
        }

        // E. The Global Module Tick
        // This traverses the initialized module list in dependency order
        // (e.g. Input -> Game Logic -> Physics -> Render)
        module_system_tick( dt );

        // F. Headless Exit Condition
        // If headless (e.g., a test suite), we need a way for a module to request an exit,
        // since there is no window 'X' button to close.
        if ( config->is_headless && sys_api()->exit_requested() )
        {
            running = false;
        }
    }

    // ---------------------------------------------------------
    // 7. Teardown
    // ---------------------------------------------------------
    // core_api()->log_info( "Runtime: Shutting down." );
    //
    // if ( !config->is_headless )
    // {
    //     app_api()->window_destroy();
    // }

    // Triggers module_exit() on all modules in reverse dependency order
    module_system_exit();

    */
    return 0;
}

/*============================================================================================*/