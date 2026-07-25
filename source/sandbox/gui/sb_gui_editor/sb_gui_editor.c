/*==============================================================================================

    sandbox/gui/sb_gui_editor/sb_gui_editor.c -- Game-engine editor shell over the gui library.

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
    /* One-call setup: gui owns the main window + render context end to end (boot path).
       The window is borderless with the chrome shell auto-emitted each frame; set
       .os_chrome = true to compare against the stock Win32 frame.  The frame hooks are the
       OS services gui cannot reach itself (it links only app + rhi); .debug arms the gui
       hotkey driver (F1-F4 overlays, F9 render mode, F10 dashboard, P perf, O state,
       C retained skip, I idle skip -- see debug_enable in gui_api.h). */

    int      ret_code    = 1;
    bool     draw_inited = false;

    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB Editor -- sb_gui_editor",
        .w     = 1600, .h = 900,
        .font  = GUI_FONT_ROBOTO_16,        // GUI_FONT_JETBRAINS_16
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.10f, 0.10f, 0.12f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_editor] gui->boot failed\n" );
        goto shutdown;
    }

    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_gui_editor] draw->init failed\n" );
        goto shutdown;
    }
    draw_inited = true;

    gui()->set_retained_skip( true );

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

    /* ------------------------------------------------------------------------------ */
    /* Editor state                                                                   */

    ed_engine_init();
    ed_viewport_init();
    if ( start_playing )
        ed_play();

    printf( "[sb_gui_editor] running -- ESC to quit\n" );
    printf( "[sb_gui_editor] Scene panel: RMB look + WASD fly, LMB drive/select, RMB+LMB/MMB pan, Alt+LMB orbit\n" );

    /* ------------------------------------------------------------------------------ */
    /* Main loop.  boot_poll pumps the OS and routes every event (rhi swapchain resize,
       gui input + floater lifecycle); false = quit or main-window close.  Input still
       reads through app()'s snapshot API as before.                                   */

    f32 dt = 0.0f;

    while ( gui()->boot_poll( &dt ) )
    {
        if ( app()->key_pressed( APP_KEY_ESCAPE ) || g_ed.request_quit )
            goto quit;

        /* ------------------------------------------------------------------------------ */

        app()->window_get_size( (i32)vp0, &g_ed.disp_w, &g_ed.disp_h );

        /* Simulation + scene-target upkeep (texture create/resize happens between frames). */
        ed_tick( dt );
        ed_viewport_maintain();

        /* Scene-pass scheduling -- mimic a real editor viewport: run the offscreen scene pass
           when (a) the toolbar Realtime toggle is on, (b) the sim is playing (it mutates state
           the panels and the scene display every frame), or (c) anything the pass draws changed
           since last frame -- camera pose, entity pool, selection, target recreate
           (ed_scene_changed).  Camera motion self-sustains: each emitted frame advances the
           pose, which marks the next frame changed, until the glide damps to rest.  A frame
           that needs the scene pass must also emit the gui (the flipped texture index bakes
           into the Scene quad), so it forces frame_dirty; everything else -- typing in a panel,
           hovering a button -- emits without touching the scene, and a static editor goes
           fully clean/retained.

           set_force_redraw is written EDGE-TRIGGERED (only when the requirement changes), not
           every frame: the debug selector menu's Force redraw checkbox toggles the same flag
           inside gui, and an unconditional per-frame write here would clobber that toggle the
           next time ed_scene_changed() flips (any Scene viewport interaction -- hover, camera
           move, resize).  debug_hotkeys_armed() is the other half: while the menu is up, it owns
           the flag outright, so this write stands down rather than re-asserting scene_render
           over whatever the checkbox says.  s_want_force_prev is deliberately left unwritten
           while stood down, so the first check after the menu closes compares against this
           editor's own last-asserted value and resyncs immediately if the two now disagree. */
        bool scene_render = g_ed.realtime || g_ed.mode == ED_MODE_PLAY || ed_scene_changed();
        static bool s_want_force_prev = false;
        if ( !gui()->debug_hotkeys_armed() && scene_render != s_want_force_prev )
        {
            gui()->set_force_redraw( scene_render );
            s_want_force_prev = scene_render;
        }

        /* ------------------------------------------------------------------------------ */
        /* GUI emit.  frame_begin returns frame_dirty; the debug overlays (P/O/F10) emit
           themselves at ctx_end, and clean frames replay volatile widgets in frame_end.  */

        bool emitted = gui()->frame_begin( dt );
        if ( emitted )
        {
            /* The target flip pairs 1:1 with the scene pass, NOT with the emit: only a frame
               that re-renders the scene may retarget the Scene quad (the flipped index bakes
               into it here, the pass writes that texture below).  An emit without the scene
               pass keeps the quad on the same texture -- its content is never touched while
               displayed, and the Scene window's unchanged hash lets the gui go clean after. */
            if ( scene_render )
                ed_viewport_flip();
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            ed_shell_build();
            gui()->ctx_end();
        }

        gui()->frame_end();

        /* ------------------------------------------------------------------------------ */
        /* Render.  boot_present_begin opens the main surface's frame (clear included) and hands
           out the live cmd for the offscreen scene pass; boot_present_end draws the gui over it,
           presents, and renders the floaters -- called unconditionally (minimized-safe).  */

        rhi_cmd_t cmd;
        if ( gui()->boot_present_begin( &cmd ) )
        {
            if ( emitted && scene_render )
                ed_viewport_render( cmd );
        }
        gui()->boot_present_end();

        /* Frame pacing (built-in): spin at 4 ms (~250 Hz) by default; with idle skip on (I) block
           on OS input while the UI is static, 16 ms (~60 Hz) while a widget animation settles. */
        gui()->frame_pace( 4, 16 );
    }

quit:
    ret_code = 0;

shutdown:
    ed_viewport_shutdown();
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();  /* also tears down the boot window + context */
    if ( draw_inited ) draw()->shutdown();
    rhi()->shutdown();                               /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

// clang-format on
/*============================================================================================*/
