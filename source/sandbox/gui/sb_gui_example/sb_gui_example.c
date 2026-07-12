/*==============================================================================================

    sandbox/gui/sb_gui_example/sb_gui_example.c -- gui feature explorer.

    The end-to-end tour of the gui system: every feature group has a demo window (ex_demos.c)
    and the "Demos" picker steps through them.  This host is 100% gui-focused and runs the
    boot-tier easy-mode loop: gui()->boot() owns the window + render context (the borderless
    chrome shell auto-emits each frame), frame_poll/present_begin/present_end drive the frame.
    Rendering itself is not under test here (see sb_vulkan for that).

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"
#include "ex_demos.h"

// clang-format off

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* Load modules -- gui's full dependency set is just rhi + app (+ the engine core stack). */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_example] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* One-call setup: gui owns the main window + render context end to end (boot path).
       Borderless by default -- gui()->viewport_shell() is the chrome (titlebar drives OS move
       + caption buttons, borders resize) and is auto-emitted each frame; set .os_chrome = true
       to compare against the stock Win32 frame.  Default caps: every feature group compiled
       in -- this is the explorer, it needs them all. */
    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui example",
        .w     = 1280, .h = 720,
        .font  = GUI_FONT_JETBRAINS_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
        .debug = true,    // gui owns the debug hotkeys + overlays (see debug_enable, gui_api.h)
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_example] gui->boot failed\n" );
        goto shutdown;
    }

    /* Active gui demo index (see ex_demos.c); switched live with the keys below. */
    int active_demo = 0;

    /* Main loop -- frame_poll pumps the OS and routes events (rhi swapchain resize, gui input
       + floater lifecycle); false on quit or main-window close. */
    f32 dt = 0.0f;
    while ( gui()->frame_poll( &dt ) )
    {
        /* Demo selection: numpad +/- step through the table (with wrap).  The "Demos" picker
           window inside the frame does the same by click. */
        const int demo_count = ex_demo_count();
        if ( app()->key_pressed( APP_KEY_NP_ADD ) )
            active_demo = ( active_demo + 1 ) % demo_count;
        if ( app()->key_pressed( APP_KEY_NP_SUB ) )
            active_demo = ( active_demo + demo_count - 1 ) % demo_count;

        /* Build the UI -- balanced scopes; emit is skipped entirely on provably clean frames
           (frame_begin false), render then replays the preserved tessellation. */
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            ex_demos[ active_demo ].fn();
            active_demo = ex_demo_picker( active_demo );

            gui()->ctx_end();
        }
        gui()->frame_end();

        /* Present the main surface + every gui-owned floater (viewport reconcile, minimized
           guard, clear to the boot color -- all inside). */
        gui()->present_begin( NULL );
        gui()->present_end();

        /* Frame pacing: spin at 4 ms (~250 Hz); with idle skip on (I) block on OS input while
           the UI is static, 16 ms (~60 Hz) while a widget animation settles. */
        gui()->frame_pace( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();  /* also tears down the boot window + context */
    rhi()->shutdown();                               /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
// clang-format on
