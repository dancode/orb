/*==============================================================================================

    sandbox/gui_editor/sb_gui_editor.c -- Game-engine editor shell over the gui library.

    A traditional editor built entirely on gui(): menu bar + toolbar chrome, a dockspace with
    Hierarchy / Inspector / Console / Assets panels around a central Scene viewport, layout
    save/load, and a play-mode loop.  The "engine" underneath is a synthetic stub (ed_engine.c)
    -- just enough scene/entity/asset/log state to drive the panels -- and the Scene panel
    shows a real offscreen render (draw() primitives into an RHI texture, displayed through
    the gui's bindless image_texture widget).

    Unity build: this file is the only compilation unit; it includes the ed_* units below.

    Controls (Scene panel, UE-style, see ed_viewcam.h):  RMB look + WASD/QE/arrows fly
    (Shift boost, wheel = speed), LMB drive / click to select, RMB+LMB or MMB pan,
    Alt+LMB orbit, Alt+RMB dolly, wheel zoom.  ESC quits.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/gui/gui_host.h"

#include "ed.h"

/* Unity units -- order matters only in that ed_engine.c defines g_ed for the rest. */
#include "ed_engine.c"
#include "ed_viewcam.c"
#include "ed_viewport.c"
#include "ed_panels.c"
#include "ed_shell.c"

// clang-format off

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    /* Load modules. */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( draw );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_editor] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    /* ------------------------------------------------------------------------------ */
    /* RHI + window + draw + gui                                                       */

    int      ret_code    = 1;
    bool     rhi_inited  = false;
    bool     draw_inited = false;
    bool     gui_inited  = false;
    win_id_t win         = APP_WIN_INVALID;
    i32      ctx         = RHI_CTX_INVALID;
    gui_vp_t vp0         = GUI_VP_INVALID;

    if ( !rhi()->init() )
        goto shutdown;
    rhi_inited = true;

    win = app()->window_open( "sb_gui_editor", 0, 0, 1600, 900, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID )
        goto shutdown;

    ctx = rhi()->context_open( win );
    if ( ctx == RHI_CTX_INVALID )
        goto shutdown;

    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_gui_editor] draw->init failed\n" );
        goto shutdown;
    }
    draw_inited = true;

    gui_forward_caps_t caps = { .keyboard_nav = true, .tables = true, .docking = true };
    gui()->init_config_front( caps );

    if ( !gui()->init( GUI_FONT_ROBOTO_16 ) ) // GUI_FONT_JETBRAINS_16
    {
        fprintf( stderr, "[sb_gui_editor] gui->init failed\n" );
        goto shutdown;
    }
    gui_inited = true;

    gui()->set_retained_skip( true );

    /* OS services gui cannot reach itself (it links only app + rhi): the perf-overlay clock and
       the frame_pace sleep / idle wait.  Wired once; the loop below just calls frame_pace(). */
    gui()->set_frame_hooks( sys_tick_seconds, sys_sleep_milliseconds, sys_wait_for_os_events_ms );

    /* Debug driver: gui owns the debug hotkeys + overlay emission from here on --
       F1-F4 overlay layers, F9 render mode, F10 dashboard, P perf overlay, O state overlay,
       C retained skip, I idle skip (see debug_enable in gui_api.h). */
    gui()->debug_enable( true );

    /* Optional args:
         -theme <name>  start with a named theme (default is the square "dark")
         -play          enter play mode immediately (animating scene from frame one) */
    bool start_playing = false;
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[ i ], "-theme" ) == 0 && i + 1 < argc )
        {
            if ( !gui()->theme_set( argv[ i + 1 ] ) )
                fprintf( stderr, "[sb_gui_editor] unknown theme '%s'\n", argv[ i + 1 ] );
        }
        else if ( strcmp( argv[ i ], "-play" ) == 0 )
        {
            start_playing = true;
        }
    }

    vp0 = gui()->viewport_open( win );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_editor] gui viewport_open failed\n" );
        goto shutdown;
    }

    /* ------------------------------------------------------------------------------ */
    /* Editor state                                                                   */

    ed_engine_init();
    ed_viewport_init();
    if ( start_playing )
        ed_play();

    printf( "[sb_gui_editor] running -- ESC to quit\n" );
    printf( "[sb_gui_editor] Scene panel: RMB look + WASD fly, LMB drive/select, RMB+LMB/MMB pan, Alt+LMB orbit\n" );

    /* ------------------------------------------------------------------------------ */
    /* Main loop                                                                      */


    f64 last_time = sys_tick_seconds();

    while ( app()->pump_events() )
    {
        f64 now_time = sys_tick_seconds();
        f32 dt       = (f32)( now_time - last_time );
        last_time    = now_time;
        if ( dt > 0.1f ) dt = 0.1f;     /* clamp stalls (debugger, window drag) */

        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            rhi()->event( &ev );
            if ( gui()->event( &ev ) )
                continue;
            if ( ev.type == APP_EV_WIN_CLOSE )
                goto quit;
        }

        if ( app()->key_pressed( APP_KEY_ESCAPE ) || g_ed.request_quit )
            goto quit;

        /* ------------------------------------------------------------------------------ */

        app()->window_get_size( win, &g_ed.disp_w, &g_ed.disp_h );

        /* Simulation + scene-target upkeep (texture create/resize happens between frames). */
        ed_tick( dt );
        ed_viewport_maintain();

        /* While playing, the sim mutates state the panels display every frame -- pin the gui
           dirty so the Inspector/Console track it live.  Edit mode gets the retained skip.
           always_emit stays pinned on while debugging the viewport twist. */
        bool always_emit = true;
        gui()->set_force_redraw( always_emit || g_ed.mode == ED_MODE_PLAY );

        /* ------------------------------------------------------------------------------ */
        /* GUI emit.  frame_begin returns frame_dirty; the debug overlays (P/O/F10) emit
           themselves at ctx_end, and clean frames replay volatile widgets in frame_end.  */

        bool emitted = gui()->frame_begin( dt );
        if ( emitted )
        {
            /* The scene pass and target flip pair 1:1 with an emitted gui frame: the flipped
               texture index is baked into this frame's Scene-panel quad, and a clean (retained)
               frame must leave the previously displayed texture's content untouched. */
            ed_viewport_flip();
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            ed_shell_build();
            gui()->ctx_end();
        }

        gui()->frame_end();
        gui()->viewport_update();

        /* ------------------------------------------------------------------------------ */
        /* Render: offscreen scene pass, then swapchain clear + gui                        */

        if ( !app()->window_is_minimized( win ) )
        {
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                if ( emitted )
                    ed_viewport_render( cmd );

                rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                    .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                    .load_op  = RHI_LOAD_OP_CLEAR,
                    .store_op = RHI_STORE_OP_STORE,
                    .clear    = { 0.10f, 0.10f, 0.12f, 1.00f },
                }, 1, NULL );
                rhi()->cmd_end_rendering( cmd );

                gui()->render( vp0, cmd );
                rhi()->frame_end( ctx );
            }
        }

        gui()->viewport_render_floaters();

        /* Frame pacing (built-in): spin at 4 ms (~250 Hz) by default; with idle skip on (I) block
           on OS input while the UI is static, 16 ms (~60 Hz) while a widget animation settles. */
        gui()->frame_pace( 4, 16 );
    }

quit:
    ret_code = 0;

shutdown:
    ed_viewport_shutdown();
    if ( ctx != RHI_CTX_INVALID ) rhi()->context_destroy( ctx );
    if ( gui_inited ) gui()->shutdown();
    if ( draw_inited ) draw()->shutdown();
    if ( rhi_inited ) rhi()->shutdown();
    if ( win != APP_WIN_INVALID ) app()->window_close( win );
    mod_system_exit();
    return ret_code;
}

// clang-format on
/*============================================================================================*/
