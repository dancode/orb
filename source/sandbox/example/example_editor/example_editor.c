/*==============================================================================================

    example_editor.c -- EDITOR / DEV SANDBOX shape.

    Windowed, hot-reloadable, console-assisted -- and the reference for driving gui() as an
    OPTIONAL SERVICE from the runtime host: the host owns the window (borderless, with the
    gui-drawn chrome shell), the rhi context, the loop, and the pacing; the descriptor wires
    gui's font/caps/debug and the UI itself is emitted in on_gui, which run_host_main calls
    only on dirty frames (retained-cache skip).  Compare sb_gui / sb_gui_editor, which use
    gui()->boot -- the test-bed easy path where gui owns the main window end to end.

    Also the reference for the render+gui COMPOSITE path (render path A with gui live):
    the render module draws the scene (a rect submitted from on_update), closes its pass,
    and the host flushes the gui over it via gui()->render( vp0, render()->frame_cmd() ).

    The window close button routes through on_close_request (veto point for save prompts).
    Q on the keyboard is an alternative quit for keyboard-first workflows.

    Loop:  RUN_LOOP_RUN
    Flags: RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD | RUN_HOST_EDITOR_SLEEP | RUN_HOST_BORDERLESS

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

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

static i32  s_clicks;               /* button press counter -- interaction-driven, safe when clean  */
static bool s_check;                /* demo checkbox                                                */
static bool s_realtime;             /* force a full emit every frame (tests set_force_redraw)       */
static bool s_show_second;          /* second window toggle -- tear it off to test floaters         */
static bool s_show_scene = true;    /* submit the scene rect behind the gui                   */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
editor_ready( void )
{
    /* gui and run are static services here -- their gateways bind directly, no fetch needed. */
    printf( "Keys: Q=quit  R=reload all  D=toggle sleep debug\n" );
    printf( "Drag a window out of the main surface to tear off a gui-owned floater.\n" );
}

/* Game logic -- every frame, widget-free.  The Realtime toggle is applied here edge-triggered
   (an unconditional per-frame write would clobber gui's own F debug hotkey). */
static void
editor_update( f32 dt )
{
    UNUSED( dt );

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

    static bool s_realtime_prev = false;
    if ( gui() && s_realtime != s_realtime_prev )
    {
        gui()->set_force_redraw( s_realtime );
        s_realtime_prev = s_realtime;
    }

    /* Scene submission -- render's draw_scene draws this behind the gui composite
       (render()->frame_cmd hand-off in run_host_main).  A static rect is enough to
       prove the scene-under-gui path; the real editor viewport is a later milestone. */
    if ( render() && s_show_scene )
    {
        i32 ctx = run_host_ctx();
        i32 w = 0, h = 0;
        if ( rhi()->context_size( ctx, &w, &h ) && w > 0 && h > 0 )
        {
            const f32 orange[ 4 ] = { 0.95f, 0.55f, 0.15f, 1.0f };
            render()->submit_rect( ctx, ( f32 )w * 0.5f, ( f32 )h * 0.5f, 220.0f, 220.0f, orange );
        }
    }
}

/* UI emission -- dirty frames only.  The chrome shell is already emitted by run_host (first
   in this context's build); everything here lays out below its caption band. */
static void
editor_gui( f32 dt )
{
    UNUSED( dt );

    f32 caption_h = gui()->viewport_caption_h( run_host_vp() );

    /* Menu bar -- insets below the chrome caption on its own. */
    if ( gui()->main_menu_bar_begin() )
    {
        bool clicked = false;
        if ( gui()->menu_begin( "File" ) )
        {
            if ( gui()->menu_item( "Quit", NULL, &clicked ) )
                run_host_quit();
            gui()->menu_end();
        }
        if ( gui()->menu_begin( "Window" ) )
        {
            gui()->menu_item( "Second Window", NULL, &s_show_second );
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

    gui()->window_set_next_pos ( 40.0f, caption_h + 60.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 360.0f, 240.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Runtime Host", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "gui() driven as a service from run_host_main" );
        gui()->separator();

        if ( gui()->button( "Click me" ) )
            s_clicks++;
        gui()->textf( "clicks: %d", s_clicks );

        gui()->checkbox( "Demo checkbox", &s_check );
        gui()->checkbox( "Realtime (force redraw)", &s_realtime );
        gui()->checkbox( "Second window", &s_show_second );
        gui()->checkbox( "Scene rect (behind gui)", &s_show_scene );
    }
    gui()->window_end();

    if ( s_show_second )
    {
        gui()->window_set_next_pos ( 440.0f, caption_h + 60.0f, GUI_COND_ONCE );
        gui()->window_set_next_size( 320.0f, 200.0f, GUI_COND_ONCE );
        if ( gui()->window_begin( "Second Window", GUI_WIN_NONE ) )
        {
            gui()->stack();
            const run_clock_t* clk = run()->clock();
            gui()->textf( "frame  %llu", (unsigned long long)clk->frame_number );
            gui()->textf( "time   %.1fs", clk->app_time );
            gui()->textf( "Drag me out to tear off a floater." );
        }
        gui()->window_end();
    }
}

/* Main-window X pressed -- the veto point for "unsaved changes" flows.  Nothing to save
   here, so log and allow the close. */
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

static const run_host_desc_t    k_desc      = {
            .name             = "sandbox_editor",
            .flags            =  RUN_HOST_CONSOLE | RUN_HOST_HOT_RELOAD
                                | RUN_HOST_BORDERLESS, // | RUN_HOST_EDITOR_SLEEP, 
            .loop_mode        = RUN_LOOP_RUN,
            .window_width     = 1280,
            .window_height    = 800,
            .modules          = k_modules,
            .gui              = &k_gui_desc,
            .on_ready         = editor_ready,
            .on_update        = editor_update,
            .on_gui           = editor_gui,
            .on_close_request = editor_close_request,
};

int
main( int argc, char** argv )
{
    return run_host_main( &k_desc, argc, argv );
}

// clang-format on
/*============================================================================================*/
