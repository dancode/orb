/*==============================================================================================

    sandbox/imgui/sb_imgui.c -- ImGui Demo Replication

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline.

==============================================================================================*/

#include <stdio.h>

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

    // if (ImGui::BeginMainMenuBar())
    // {
    //     if (ImGui::BeginMenu("File"))
    //     {
    //         ShowExampleMenuFile();
    //         ImGui::EndMenu();
    //     }
    //     if (ImGui::BeginMenu("Edit"))
    //     {
    //         if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
    //         if (ImGui::MenuItem("Redo", "Ctrl+Y", false, false)) {} // Disabled item
    //         ImGui::Separator();
    //         if (ImGui::MenuItem("Cut", "Ctrl+X")) {}
    //         if (ImGui::MenuItem("Copy", "Ctrl+C")) {}
    //         if (ImGui::MenuItem("Paste", "Ctrl+V")) {}
    //         ImGui::EndMenu();
    //     }
    //     ImGui::EndMainMenuBar();
    // }
}

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
    if (!gui()->window_begin("Dear ImGui Demo", window_flags))
    {
        // Early out if the window is collapsed, as a optimization.
        gui()->window_end();
        return;
    }
    gui()->stack();
    gui()->text("This is some useful text."); gui()->same_line(0);
    gui()->help_marker("This is a help marker for the text above.\nIt can be very useful to explain things.");

    static bool show_another_window = false;
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

    gui()->textf("Application average %.3f ms/frame (%.1f FPS)", 
        1000.0f / sys_tick_seconds() /* placeholder for framerate */, 
        60.0f /* placeholder for fps */);

    gui()->window_end();

    if (show_another_window)
    {
        gui_win_flags_t another_window_flags = GUI_WIN_CAN_AUTOSIZE;  // Add a menu bar to the window
        if (gui()->window_begin("Another Window", another_window_flags ))
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
    mod_static( draw );
    mod_static( gui );

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
    /* Setup RHI + Window */

    int         ret_code   = 1;
    bool        rhi_inited = false;
    bool        draw_inited = false;
    bool        gui_inited  = false;
    win_id_t    win         = APP_WIN_INVALID;
    i32         ctx         = RHI_CTX_INVALID;
    gui_vp_t    vp0         = GUI_VP_INVALID;

    if ( !rhi()->init() ) {
        goto shutdown;
    }
    rhi_inited = true;

    win = app()->window_open( "sb_gui", 0, 0, 1280, 720, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID ) {
        goto shutdown;
    }

    ctx = rhi()->context_open( win );
    if ( ctx == RHI_CTX_INVALID ) {
        goto shutdown;
    }

    /* ------------------------------------------------------------------------------ */
    /* Setup Resources */

    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_gui] draw->init failed\n" );
        goto shutdown;
    }
    draw_inited = true;

    /* ------------------------------------------------------------------------------ */
    /* Setup GUI */

    if ( !gui()->init() ) {
         fprintf( stderr, "[sb_gui] gui->init failed\n" );
         goto shutdown;
    }
    gui_inited = true;
    
    gui()->font_load( "fonts/jetbrains_regular_16.orb_font" );

    vp0 = gui()->viewport_open( win );
    if ( vp0 == GUI_VP_INVALID ) {
        fprintf( stderr, "[sb_gui] gui viewport_open (primary) failed\n" );
        goto shutdown;
    }

    /* ------------------------------------------------------------------------------ */
    /* Start render loop. */

    printf( "[sb_gui] running -- ESC to quit\n" );
    printf( "[sb_gui] gui demos: F1-F4 debug overlay\n" );
    printf( "[sb_gui] P cycles the perf overlay: off -> FPS -> +timings -> +render counts\n" );
    printf( "[sb_gui] F6 cycles the render view: normal -> wireframe -> batch colors\n" );

    int perf_mode = 0;

    f64 last_time = sys_tick_seconds();
    
    while ( app()->pump_events() )
    {
        f64 now_time = sys_tick_seconds();
        f32 dt       = (f32)( now_time - last_time );
        last_time    = now_time;

        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            rhi()->event( &ev );
            if ( gui()->event( &ev ) )
                continue; /* handled */

            if ( ev.type == APP_EV_WIN_CLOSE )
                goto shutdown;
        }

        /* ------------------------------------------------------------------------------ */
        /* Debug overlay layers (Debug build only): toggle each with the F1-F4 keys.
           F1 window frames   F2 widget interaction rects   F3 resize bands  F4 clip rects. */

        // gui()->debug_enable( true );

        /* Perf overlay: P cycles off -> FPS -> +timings -> +render counts (mod 4). */
        if ( app()->key_pressed( APP_KEY_P ) )
            perf_mode = ( perf_mode + 1 ) % 5;

        /* F6 cycles the debug render view: normal -> wireframe -> batch. */
        if ( app()->key_pressed( APP_KEY_F9 ))
        {
            gui_render_mode_t m = ( gui()->debug_get_render_mode() + 1 ) % GUI_RENDER_MODE_COUNT;
            gui()->debug_set_render_mode( m );
            static const char* names[] = { "normal", "wireframe", "batch" };
            printf( "[sb_gui] render mode: %s\n", names[ m ] );
        }

        /* C toggles Level 2 retained skip: skips tessellation on unchanged frames (hash upfront). */
        if ( app()->key_pressed( APP_KEY_C ) )
        {
            bool on = !gui()->retained_skip();
            gui()->set_retained_skip( on );
            printf( "[sb_vulkan] retained skip: %s\n", on ? "on (skip tess if unchanged)" : "off (always tess)" );
        }

        /* ------------------------------------------------------------------------------ */
        /* The GUI emit and render frame loop */
           
        gui()->frame_begin( dt );

        if ( gui()->frame_dirty() )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            show_example_main_menu_bar();

            if ( show_demo)
                 show_demo_window(&show_demo);

            gui()->perf_overlay( sys_tick_seconds, perf_mode );

            gui()->ctx_end();
        }

        gui()->frame_end();
        gui()->viewport_update();

        if ( !app()->window_is_minimized( win ) )
        {
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                // Clear the swapchain color attachment to a solid color (e.g., light blue)
                rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                    .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                    .load_op  = RHI_LOAD_OP_CLEAR,
                    .store_op = RHI_STORE_OP_STORE,
                    .clear    = { 0.15f, 0.15f, 0.20f, 1.00f },
                }, 1, NULL );
                rhi()->cmd_end_rendering( cmd );
                gui()->render( vp0, cmd );
                rhi()->frame_end( ctx );
            }
        }

        gui()->viewport_render_floaters();

        sys_sleep_milliseconds( 4 );
    }
    
    ret_code = 0;

shutdown:
    if ( ctx != RHI_CTX_INVALID ) rhi()->context_destroy( ctx );
    if ( gui_inited ) gui()->shutdown();
    if ( draw_inited ) draw()->shutdown();
    if ( rhi_inited ) rhi()->shutdown();
    if ( win != APP_WIN_INVALID ) app()->window_close( win );
    mod_system_exit();
    return ret_code;
}

// clang-format on
