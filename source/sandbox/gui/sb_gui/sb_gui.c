/*==============================================================================================

    sandbox/gui/sb_gui/sb_gui.c -- ImGui Demo Replication

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline.

    Just the Dear ImGui demo window and the volatile-widget pulse that rides along with it.  The
    feature demos this file used to also carry now live with the feature they demonstrate:

        Style Editor / Font Browser  -> sb_gui_style
        Panel Shell / HUD Overlay    -> sb_gui_example, "Layout"
        Root Region / Tab Groups     -> sb_gui_example, "Windows"
        Drag Reorder                 -> sb_gui_example, "Interact"
        Toolbars                     -> sb_gui_example, "Widgets"

==============================================================================================*/

#include <stdio.h>
#include <string.h>
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

// clang-format off

/*============================================================================================*/

struct
{
    bool show_main_menubar;

} demo_data;

/*============================================================================================*/
/* Volatile widget demo -- a purely cosmetic square that keeps pulsing on frames where the rest
   of the UI build is skipped (frame_begin returned false: mouse idle, nothing else animating).
   Proves the feature end to end: an ordinary gui()->rect_filled() call, wrapped in
   gui()->volatile_cb() so gui can replay it standalone on clean frames (frame_end runs the
   replay internally) with no other widget emit, no layout, no re-tessellation of anything else. */

static void
demo_volatile_pulse_cb( gui_id_t id, bool is_replay )
{
    (void)id;
    (void)is_replay;

    gui()->volatile_begin();
    f32 t = (f32)sys_tick_seconds();
    f32 s = 0.5f + 0.5f * sinf( t * 3.0f );
    u8  g = (u8)( 80.0f + 175.0f * s );
    u32 abgr = 0xFF000000u | ( (u32)g << 16 ) | ( (u32)g << 8 );   /* ABGR: pulsing cyan (B,G), alpha full */
    gui_rect_t r = gui()->canvas( 24.0f );
    r.w = 24.0f;
    gui()->draw_rect( r.x, r.y, r.w, r.h, abgr );

    /* CONTRACT: a volatile block must keep a FIXED LAYOUT FOOTPRINT.  The pixels inside it may
       change freely every frame (that is the whole point), but its size must not -- surrounding
       widgets are retained and only re-lay-out on real frames, so a block whose width jitters
       (e.g. "%.1f" gaining a digit) shoves its same_line neighbours around on real frames while
       they sit frozen on idle ones: visible flicker.  Fixed field widths + the mono font keep
       this line's footprint constant while the digits still animate. */
    f32 delta_time = gui()->get_delta_time();
    f32 ms  = delta_time * 1000.0f;
    f32 fps = ( delta_time > 0.0f ) ? 1.0f / delta_time : 0.0f;
    gui()->textf( "Application average %8.3f ms/frame (%7.1f FPS)", ms, fps );
    gui()->volatile_end();
}

/*============================================================================================*/
/* Demo setup                                                                                 */
/*============================================================================================*/

// Demonstrate creating a "main" fullscreen menu bar and populating it.
// Note the difference between BeginMainMenuBar() and BeginMenuBar():
// - BeginMenuBar() = menu-bar inside current window (which needs the ImGuiWindowFlags_MenuBar flag!)
// - BeginMainMenuBar() = helper to create menu-bar-sized window at the top of the main viewport + call BeginMenuBar() into it.

static bool show_demo = true;

static void show_example_main_menu_bar()
{
    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "Examples" ) )
        {
            gui()->menu_item( "Demo Window", NULL, &show_demo );
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

}

/*============================================================================================*/
/* Demo window                                                                                */
/*============================================================================================*/

static void
show_demo_window(bool* p_open)
{
    if ( demo_data.show_main_menubar ) 
    { 
        show_example_main_menu_bar(); 
    }

    // Exceptionally add an extra assert here for people confused about initial Dear ImGui setup
    // Most functions would return false if the window is collapsed or entirely clipped.
    gui_win_flags_t window_flags = 0;
    
    window_flags |= GUI_WIN_CAN_AUTOSIZE;  // Add a menu bar to the window
    
    // We demonstrate using the full window_begin() API
    gui()->window_set_next_size( 640.0f, 640.0f, GUI_COND_ONCE );
    static bool show_another_window = false;
    if (gui()->window_begin("Basic Gui Demo", window_flags))
    {
        gui()->stack();
        gui()->text("This is some useful text."); gui()->same_line(0);
        gui()->help_marker("This is a help marker for the text above.\nIt can be very useful to explain things.");

        gui()->checkbox("Demo Window", p_open);
        gui()->checkbox("Another Window", &show_another_window);

        static f32 f = 0.0f;
        gui()->slider_float("float", &f, 0.0f, 1.0f);
        gui()->separator_text("Inline color editor");
        gui()->text("Color widget:");
        gui()->stack_same_line(0.0f); gui()->help_marker("Click on the color square to open a color picker.\nCtrl+Click on individual component to input value.\n");
        static f32 color[4] = { 0.4f, 0.7f, 0.0f, 1.0f };
        gui()->color_edit3("MyColor##1", color, GUI_COLOR_EDIT_NONE);

        gui()->text("Color widget HSV with Alpha:");
        gui()->color_edit4("MyColor##2", color, GUI_COLOR_EDIT_DISPLAY_HSV);

        gui()->text("Color widget with Float Display:");
        gui()->color_edit4("MyColor##2f", color, GUI_COLOR_EDIT_FLOAT);

        static int counter = 0;
        if (gui()->button("Button"))
            counter++;
        gui()->same_line( -1 );
        gui()->textf("counter = %d", counter);

        /* Static placeholder text (not wired to a real per-frame delta) -- if this were made to
           recompute from an ever-growing clock every frame, its changing content would keep this
           window's command hash different frame to frame forever, which would keep frame_dirty()
           true forever and defeat the idle-skip entirely (the exact problem volatile widgets exist
           to route around; see demo_volatile_pulse_cb above for the widget that keeps animating anyway). */
        // gui()->textf("Application average %.3f ms/frame (%.1f FPS)", 6.061f, 165.0f);

        gui()->volatile_cb( "volatile_pulse_demo", demo_volatile_pulse_cb );
        gui()->text( "<- volatile widget: keeps pulsing on idle frames, no full rebuild" );

        for ( int i = 0; i < 40; i++ )
        {
            gui()->textf( "Line %d", i );
        }
    }
    gui()->window_end();

    if ( show_another_window )
    {
        gui_win_flags_t another_window_flags = GUI_WIN_CAN_AUTOSIZE;  // Add a menu bar to the window
        if ( gui()->window_begin("Another Window", another_window_flags ))
        {
            gui()->stack();
            gui()->text("Hello from another window!");
            if (gui()->button("Close Me"))
                show_another_window = false;
        }
        gui()->window_end();
    }
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* Load modules. */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    // mod_static( draw );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    core()->log_set_min_level( LOG_LEVEL_INFO );
    core_log_fn( LOG_LEVEL_DEBUG, "sb_gui", "debug log: modules loaded successfully" );

    /* ------------------------------------------------------------------------------ */
    /* One-call setup: gui owns the main window + render context end to end (boot path).
       os_chrome keeps the stock Win32 frame this demo has always had -- flip it off for a
       borderless window with the gui chrome shell auto-emitted each frame.  The frame hooks
       are the OS services gui cannot reach itself (it links only app + rhi); .debug arms
       the gui hotkey driver (see debug_enable in gui_api.h). */

    int      ret_code    = 1;
    bool     draw_inited = false;

    i32 vp = gui()->boot( &( gui_boot_desc_t ){
        .title     = "sb_gui",
        .w         = 1280, .h = 960,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO_16,
        .clock     = sys_tick_seconds,
        .sleep     = sys_sleep_milliseconds,
        .wait      = sys_wait_for_os_events_ms,
        .clear     = { 0.10f, 0.10f, 0.10f, 1.00f },
        .debug     = true,
    } );
    if ( vp == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui] gui->boot failed\n" );
        goto shutdown;
    }

    gui()->debug_enable( true );

    /* ------------------------------------------------------------------------------ */
    /* Setup Resources */

    // if ( !draw()->init() )
    // {
    //     fprintf( stderr, "[sb_gui] draw->init failed\n" );
    //     goto shutdown;
    // }
    // draw_inited = true;

    /* ------------------------------------------------------------------------------ */
    /* GUI Style */

    bool modify_style = true;
    if ( modify_style )
    {
        gui_style_t* style = gui()->style_get();

        // Recolor by SEED: change a source color, re-derive the 32-cell grid from it.  This is
        // the usual door -- every role fed by the seed moves together and keeps its ramp.
        style->palette.seed[GUI_SEED_SURFACE] = GUI_COLOR( 0x20, 0x20, 0x20, 0xFF );
        style->palette.seed[GUI_SEED_INK    ] = GUI_COLOR( 0xFF, 0xAA, 0x00, 0xFF );
        gui()->style_bake( style );

        // ...then disagree with the ramp on individual cells, if you want to.  Order matters:
        // a cell written before the bake would simply be overwritten by it.
        // style->col[GUI_ROLE_MARK][GUI_PHASE_IDLE] = GUI_COLOR( 0xFF, 0x40, 0x40, 0xFF );

        // Modify any skin (STYLE) knob -- metrics are authored for a baseline em=12
        style->var[GUI_VAR_PANEL_ROUND] = 0;    // Square windows
        style->var[GUI_VAR_ROUND]       = 0;    // No bevel on buttons
        // style->var[GUI_VAR_GAP]      = 12;   // More breathing room

        // Re-scale and apply the changes across the UI
        gui()->style_apply();
    }
    /* ------------------------------------------------------------------------------ */
    /* Start render loop.  boot_poll pumps the OS and routes every event (rhi swapchain
       resize, gui input + floater lifecycle); false = quit or main-window close.  Frame
       hooks and the debug driver were wired by boot() above. */

    f32 dt = 0.0f;

    while ( gui()->boot_poll( &dt ) )
    {
        /* ------------------------------------------------------------------------------ */
        /* Host-side debug keys.  The gui debug hotkeys (F1-F4 layers, F9 render view, F10
           dashboard, P/O overlays, C retained skip, F force redraw, I idle skip) are handled
           inside gui via debug_enable above. */

        /* M dumps the memory stats table: allocation sizes and usage. */
        if ( app()->key_pressed( APP_KEY_M ) )
        {
            gui_print_mem_stats();
        }

        /* ------------------------------------------------------------------------------ */
        /* The GUI emit and render frame loop */

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            show_example_main_menu_bar();
            
            if ( show_demo )
                show_demo_window( &show_demo );

            /* Closing the default context also auto-emits the debug overlays (perf/state/dashboard)
               last in its build.  Clean frames skip this whole scope; frame_end below replays the
               registered volatile_cb callbacks (see demo_volatile_pulse_cb above) internally. */

            gui()->ctx_end();
        }

        gui()->frame_end();

        /* Render + present: a balanced pair.  boot_present_begin opens the main surface's frame
           (cleared to the boot color) -- its bool gates host render passes, none here (see
           sb_gui_editor for that use); boot_present_end draws the gui, presents, and renders every
           owned floater.  Both minimized-safe. */

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        /* Frame pacing (built-in): spin at 4 ms (~250 Hz) by default; with idle skip on block
           on OS input while the UI is static, 16 ms (~60 Hz) while a widget animation settles. */

        gui()->boot_pace ( 4, 16 );
    }
    
    ret_code = 0;

shutdown:

    if ( vp != GUI_VP_INVALID ) gui()->shutdown();
    if ( draw_inited ) draw()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
