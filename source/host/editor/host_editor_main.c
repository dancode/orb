/*==============================================================================================

    host_editor_main.c -- the developer "editor.exe" host.

    The real editor executable.  Same driving as sb_example_editor -- gui() as an OPTIONAL
    SERVICE under run_host_main: the host owns the borderless window (with the gui-drawn chrome
    shell), the rhi context, the loop, and the pacing; the descriptor wires gui's font / caps /
    debug, and the UI is emitted in on_gui (called only on dirty frames -- retained-cache skip).
    render draws the scene behind the gui composite (render path A); the host flushes gui over it.

    What makes this the HOST and not the sandbox: it parses launch params up front (host_common,
    pre-engine) -- -project seeds the future project mount, -dev arms idle-sleep pacing.  Everything
    below the arg parse is the same modern stack sb_example_editor validates.

    Q/R/D are developer hotkeys read from the CONSOLE (terminal focus only, via RUN_HOST_CONSOLE)
    so they can't be fumbled from the editor window -- see editor_handle_shortcuts.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_BORDERLESS [ | RUN_HOST_EDITOR_SLEEP ]

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "host/common/host_common.h"

#include "engine/sys/sys_host.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"

#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/gui/gui_api.h"
#include "runtime_modules/render/render_api.h"

#include "runtime/runtime_api.h"
#include "runtime/runtime_host.h"

MOD_USE_APP;
MOD_USE_RUN;
MOD_USE_GUI;

// clang-format off
/*==============================================================================================
    Host state
==============================================================================================*/

static bool s_show_scene = true;    /* submit the scene rect behind the gui                   */
static bool s_show_stats;           /* frame-clock readout window                             */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
editor_ready( void )
{
    /* gui and run are static services here -- their gateways bind directly, no fetch needed. */
    printf( "[editor] ready\n" );
    printf( "Dev keys (terminal focus): Q=quit  R=reload all  D=toggle sleep debug\n" );
}

/* Developer hotkeys.  DELIBERATELY read the CONSOLE (sys_key_pressed, terminal focus only) rather
   than the editor window, so these destructive dev actions can't be fumbled while working in the
   UI.  In-window user shortcuts would instead read app()->key_pressed fenced by
   gui()->want_capture_keyboard -- see gui_api.h.  These three are console-gated on purpose. */
static void
editor_handle_shortcuts( void )
{
    if ( sys_key_pressed( PLATFORM_KEY_Q ) )
    {
        printf( "[editor] Q -- quit\n" );
        run_host_quit();
        return;
    }

    if ( sys_key_pressed( PLATFORM_KEY_R ) )
    {
        printf( "[editor] R -- reload all\n" );
        mod_reload_all();
    }

    if ( sys_key_pressed( PLATFORM_KEY_D ) )
         run_host_sleep_debug_toggle();
}

/* Viewport scene feed -- the editor's per-frame render submission.  render()->draw_scene replays
   whatever is submitted here behind the gui composite (frame_cmd hand-off in run_host_main).  Kept
   separate from shell input above: this is the seam the real editor grows into (world tick ->
   visible-set cull -> submit).  A static rect proves the scene-under-gui path for now. */
static void
editor_submit_scene( void )
{
    if ( !render() || !s_show_scene )
        return;

    i32 ctx = run_host_ctx();
    i32 w = 0, h = 0;
    if ( rhi()->context_size( ctx, &w, &h ) && w > 0 && h > 0 )
    {
        const f32 slate[ 4 ] = { 0.16f, 0.20f, 0.28f, 1.0f };
        render()->submit_rect( ctx, ( f32 )w * 0.5f, ( f32 )h * 0.5f, ( f32 )w * 0.6f, ( f32 )h * 0.6f, slate );
    }
}

/* Per-frame host update -- every frame, widget-free (widgets go in on_gui, which the retained
   cache may skip).  Editor shell input, then the viewport scene feed -- each in its own helper. */
static void
editor_update( f32 dt )
{
    UNUSED( dt );

    editor_handle_shortcuts();
    editor_submit_scene();
}

/* UI emission -- dirty frames only.  The chrome shell is already emitted by run_host (first in
   this context's build); everything here lays out below its caption band. */
static void
editor_gui( f32 dt )
{
    UNUSED( dt );

    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "File" ) )
        {
            bool quit = false;
            if ( gui()->menu_item( "Quit", NULL, &quit ) )
                run_host_quit();
            gui()->menu_end();
        }
        if ( gui()->menu_begin( "View" ) )
        {
            gui()->menu_item( "Scene rect", NULL, &s_show_scene );
            gui()->menu_item( "Frame stats", NULL, &s_show_stats );
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

    f32 caption_h = gui()->viewport_caption_h( run_host_vp() );

    gui()->window_set_next_pos ( 40.0f, caption_h + 60.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 360.0f, 180.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Editor", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "orb editor host" );
        gui()->separator();
        gui()->textf( "gui() driven as a service from run_host_main." );
        gui()->checkbox( "Scene rect (behind gui)", &s_show_scene );
    }
    gui()->window_end();

    if ( s_show_stats )
    {
        gui()->window_set_next_pos ( 420.0f, caption_h + 60.0f, GUI_COND_ONCE );
        gui()->window_set_next_size( 300.0f, 160.0f, GUI_COND_ONCE );
        if ( gui()->window_begin( "Frame Stats", GUI_WIN_NONE ) )
        {
            gui()->stack();
            const run_clock_t* clk = run()->clock();
            gui()->textf( "frame  %llu", ( unsigned long long )clk->frame_number );
            gui()->textf( "time   %.1fs", clk->app_time );
            gui()->textf( "dt     %.2f ms", clk->dt * 1000.0f );
        }
        gui()->window_end();
    }
}

/* Main-window X pressed -- the veto point for "unsaved changes" flows.  Nothing to save yet,
   so log and allow the close. */
static bool
editor_close_request( void )
{
    printf( "[editor] close requested -- allowing\n" );
    return true;
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( core   ),    /* cvars, logging, memory arenas -- static       */
    RUN_SERVICE( app    ),    /* window, OS pump                               */
    RUN_SERVICE( rhi    ),    /* GPU backend -- static service                 */
    RUN_SERVICE( draw   ),    /* immediate primitives -- render's draw backend */
    RUN_SERVICE( gui    ),    /* immediate mode GUI -- OPTIONAL static service */
    RUN_MODULE ( render ),    /* scene frame owner -- gui composites over it   */
    { 0 }
};

static const gui_forward_caps_t k_gui_caps = {
    .keyboard_nav = true,
    .tables       = true,
    .docking      = true,
};

static const run_gui_desc_t k_gui_desc = {
    .font  = GUI_FONT_ROBOTO_16,
    .caps  = &k_gui_caps,
    .clear = { 0.10f, 0.10f, 0.12f, 1.00f },
    .debug = true,            /* P/O/F10 overlays, I idle skip, etc. */
};

int
main( int argc, char** argv )
{
    /* Pre-engine launch params (host_common): -project seeds the project mount, -dev arms
       idle-sleep pacing.  This is the seam that makes editor.exe a real host, not a sandbox. */
    launch_params_t params;
    host_args_parse( argc, argv, &params );

    if ( params.project_path[ 0 ] )
        printf( "[editor] project: %s\n", params.project_path );

    u32 flags = RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_BORDERLESS;
    if ( params.dev_mode )
        flags |= RUN_HOST_EDITOR_SLEEP;    /* block on OS input when idle -- lower dev-box heat */

    const run_host_desc_t desc = {
        .name             = "orb editor",
        .flags            = flags,
        .loop_mode        = RUN_LOOP_RUN,
        .window_width     = 1600,
        .window_height    = 900,
        .modules          = k_modules,
        .gui              = &k_gui_desc,
        .on_ready         = editor_ready,
        .on_update        = editor_update,
        .on_gui           = editor_gui,
        .on_close_request = editor_close_request,
    };

    return run_host_main( &desc, argc, argv );
}

// clang-format on
/*============================================================================================*/
