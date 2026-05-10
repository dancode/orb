/*==============================================================================================

    runtime/host/runtime_host.c : shared host module to bootstrap a "runtime" derived host.

==============================================================================================*/

#include "runtime/host/runtime_host.h"    // The runtime host API and config struct

// Engine Foundation
#include "engine/mod/mod.h"
#include "engine/mod/mod_api.h"
#include "engine/core/core_api.h"
#include "engine/sys/sys_api.h"
#include "engine/app/app_api.h"

// Runtime Services (The resident systems)
#include "runtime/services/jobs/rt_jobs.h"

int
runtime_host_run( const runtime_config_t* config )
{
    UNUSED( config );
       
    // ---------------------------------------------------------

    // Register the static foundation modules
    mod_static_load( "core", core_get_mod_api() );
    mod_static_load( "sys", sys_get_mod_api() );
        
    // Only register the windowing application layer if we aren't headless
    if ( config->is_headless == false )
    {
        mod_static_load( "app", app_get_mod_api() );
    }
           
    // ---------------------------------------------------------
    // 2. Load the Payloads
    // ---------------------------------------------------------
    // In the future, this list would be populated by reading a project.json,
    // but for now, we load the standard dynamic payloads.

    if ( !config->is_headless )
    {
        mod_load( "render" );
        mod_load( "audio" );
    }

    // Load the specific project/sandbox DLL requested by the CLI
    if ( config->project_dll && config->project_dll[ 0 ] != '\0' )
    {
        mod_load( config->project_dll );
    }


     
    // ---------------------------------------------------------
    // 3. Resolve Dependencies and Initialize
    // ---------------------------------------------------------
    if ( !mod_init_all() )
    {
        // If core_api isn't loaded yet, fallback to standard printf for critical failures
        // sys_api()->print_console( "CRITICAL: Failed to initialize module graph.\n" );
        return -1;
    }

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