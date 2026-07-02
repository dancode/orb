/*==============================================================================================

    sandbox/imgui/sb_imgui.c -- ImGui Demo Replication

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/gui/gui_host.h"
#include "developer/dev_font/dev_font.h"

// clang-format off

/*============================================================================================*/

struct
{
    bool show_main_menubar;

} demo_data;

/*============================================================================================*/
/* Font browser state                                                                          */
/*============================================================================================*/

#define FB_FONT_MAX 32
#define FB_NAME_MAX 128

typedef struct
{
    char names[ FB_FONT_MAX ][ FB_NAME_MAX ];
    int  count;
    int  sel;
    bool scanned;
    i32  size_px;
    u32  preview_id;
    bool preview_ready;
    char preview_ttf [ FB_NAME_MAX ];
    i32  preview_size;
    char custom_text [ 256 ];
    char status      [ 256 ];
    bool status_ok;

} font_browser_t;

static font_browser_t s_fb;

static bool
fb_scan_cb( const char* filename, const char* full_path, void* userdata )
{
    UNUSED( full_path );
    UNUSED( userdata );
    if ( s_fb.count < FB_FONT_MAX )
        snprintf( s_fb.names[ s_fb.count++ ], FB_NAME_MAX, "%s", filename );
    return true;
}

static void
fb_scan( void )
{
    s_fb.count = 0;
    char src[ 512 ];
    if ( dev_font_source_dir( src, sizeof( src ) ) )
    {
        sys_file_glob( src, "*.ttf", fb_scan_cb, NULL );
        sys_file_glob( src, "*.otf", fb_scan_cb, NULL );
    }
    s_fb.scanned = true;
    snprintf( s_fb.status, sizeof( s_fb.status ), "Found %d font(s) in font_source/", s_fb.count );
    s_fb.status_ok = true;
}

static void
show_font_browser( bool* p_open )
{
    /* Lazy init on first open. */
    if ( !s_fb.scanned )
    {
        s_fb.size_px = 16;
        snprintf( s_fb.custom_text, sizeof( s_fb.custom_text ),
                  "The quick brown fox jumps over the lazy dog." );
        fb_scan();
    }


    // gui()->window_set_next_pos( 320.0f, 60.0f, GUI_COND_ONCE );
    // gui()->window_set_next_size( 128.0f, 128.0f, GUI_COND_ONCE );
    if ( !gui()->window_begin( "Font Browser", GUI_WIN_CLOSEABLE | GUI_WIN_CAN_AUTOSIZE ))
    {
        /* window_begin returns false for both collapsed and X-closed windows.
           Only clear p_open when the window was actually closed (X clicked). */
        if ( p_open && !gui()->window_is_open( "Font Browser" ) )
            *p_open = false;
        gui()->window_end();
        return;
    }

    bool skip_body =  false;
    if ( skip_body )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* --- Source ---------------------------------------------------------------- */
    gui()->separator_text( "Source" );

    /* Left panel: combo + slider stacked.  Right panel: tall "Bake & Preview" button. */
    static const char* bake_label = "Bake & Preview";
    gui()->split_begin( "##src", gui()->button_width( bake_label ) );

        gui()->stack();
        const char* combo_label = ( s_fb.count > 0 ) ? s_fb.names[ s_fb.sel ] : "(no fonts)";
        if ( gui()->combo_begin( "##ttf", combo_label, GUI_COMBO_NONE ) )
        {
            for ( int i = 0; i < s_fb.count; i++ )
            {
                bool sel = ( i == s_fb.sel );
                if ( gui()->selectable( s_fb.names[ i ], &sel ) )
                    s_fb.sel = i;
            }
            gui()->combo_end();
        }
        gui()->slider_int( "##size", &s_fb.size_px, 6, 64 );

    gui()->split_next();

        gui()->stack();
        gui()->disabled_begin( s_fb.count == 0 );
        bool bake = gui()->button_fill( bake_label );
        gui()->disabled_end();

    gui()->split_end();

    /* Refresh + status below the source row. */
    gui()->stack();
    if ( gui()->small_button( "Refresh List" ) )
        fb_scan();

    if ( s_fb.status[ 0 ] )
    {
        if ( s_fb.status_ok )
            gui()->text_disabled( s_fb.status );
        else
            gui()->text_colored( GUI_COLOR( 0xFF, 0x60, 0x60, 0xFF ), s_fb.status );
    }

    /* --- Bake ------------------------------------------------------------------ */
    if ( bake && s_fb.count > 0 )
    {
        /* Re-baking the same font at the same size re-uploads an identical atlas and forces a
           GPU drain in the reload path (see font_slot_load) for nothing.  If the requested
           font+size is already the live preview, skip it. */
        bool same = s_fb.preview_ready
                 && s_fb.size_px == s_fb.preview_size
                 && strcmp( s_fb.names[ s_fb.sel ], s_fb.preview_ttf ) == 0;

        char path[ 512 ];
        if ( same )
        {
            snprintf( s_fb.status, sizeof( s_fb.status ), "Already loaded: %s at %d px",
                      s_fb.preview_ttf, s_fb.preview_size );
            s_fb.status_ok = true;
        }
        else if ( dev_font_get( s_fb.names[ s_fb.sel ], s_fb.size_px, path, sizeof( path ) ) )
        {
            if ( !s_fb.preview_ready )
            {
                u32 id = gui()->font_load( path );
                if ( id )
                {
                    s_fb.preview_id    = id;
                    s_fb.preview_ready = true;
                }
            }
            else
            {
                gui()->font_load_into( s_fb.preview_id, path );
            }
            snprintf( s_fb.preview_ttf, sizeof( s_fb.preview_ttf ), "%s",
                      s_fb.names[ s_fb.sel ] );
            s_fb.preview_size = s_fb.size_px;
            snprintf( s_fb.status, sizeof( s_fb.status ), "Loaded: %s at %d px",
                      s_fb.preview_ttf, s_fb.preview_size );
            s_fb.status_ok = true;
        }
        else
        {
            snprintf( s_fb.status, sizeof( s_fb.status ), "Error: %s", dev_font_last_error() );
            s_fb.status_ok = false;
        }
    }

    /* --- Preview --------------------------------------------------------------- */
    if ( s_fb.preview_ready )
    {
        gui()->separator_text( "Preview" );

        gui()->input_text_with_hint( "##custom", "Custom preview text...",
                                     s_fb.custom_text, sizeof( s_fb.custom_text ) );
        gui()->spacing( 0.0f );

        /* NOTE -- this preview is NOT isolated to these lines.  The renderer has no per-run font:
           text commands store only position/colour/clip (see GUI_CMD_TEXT in gui_01_emit_draw.c), and the
           glyph atlas + UVs are resolved at DEFERRED tessellation time from one global active font
           (tess_text_n / font_atlas_idx in gui_02_build_tess.c).  So whichever font is active when the
           frame tessellates draws the ENTIRE frame -- push_font/pop_font here cannot scope a second
           font onto just the preview.  A true side-by-side preview would need the preview glyphs
           rendered through a separate texture/path decoupled from the global font state; that is not
           possible with the current single-global-font model. */
        gui()->push_font( s_fb.preview_id );
        gui()->stack();
        if ( s_fb.custom_text[ 0 ] )
            gui()->text( s_fb.custom_text );
        gui()->text( "ABCDEFGHIJKLMNOPQRSTUVWXYZ" );
        gui()->text( "abcdefghijklmnopqrstuvwxyz" );
        gui()->text( "0123456789  !@#$%^&*()-+=[]{};" );
        gui()->pop_font();

        /* --- Apply ------------------------------------------------------------- */
        gui()->separator_text( "Apply" );
        gui()->textf( "Preview: %s  %d px", s_fb.preview_ttf, s_fb.preview_size );
        if ( gui()->button( "Use Font" ) )
            gui()->font_use( s_fb.preview_id );
    }

    gui()->window_end();
}

/*============================================================================================*/
/* Split-panel helper demo                                                                     */
/*                                                                                              */
/* Recursive rect splits over gui()->split + push_layout_overlay: a fixed sidebar beside a filling */
/* content column, and that column carved top-to-bottom into header / body / footer.  Known     */
/* sizes, single pass, plain gui_rect_t locals -- no layout tree, no cached heights.            */
/*============================================================================================*/

static void
show_split_demo( bool* p_open )
{
    static const char* WIN = "Split Panels";
    if ( !gui()->window_begin( WIN, GUI_WIN_CLOSEABLE ) )
    {
        if ( p_open && !gui()->window_is_open( WIN ) )
            *p_open = false;
        gui()->window_end();
        return;
    }

    gui()->stack();
    gui()->text_wrapped( "One gui()->carve form describes the whole nested layout: a column split "
                         "(80px sidebar + fill content), the content track itself cut into rows "
                         "(header / body / footer).  Leaf rects come back in reading order." );

    /* The entire layout as one flat form -- structure lives in where the CUT/END sentinels sit.
       Leaves stream back in reading order: 0 sidebar, 1 header, 2 body, 3 footer. */
    static const f32 FORM[] =
    {
        GUI_CUT_X,                  /* root: cut the band into columns           */
            80.0f,                  /*   leaf 0 : 80px sidebar                    */
            1.0f, GUI_CUT_Y,        /*   fill content column, cut into rows:      */
                28.0f,              /*       leaf 1 : 28px header                 */
                1.0f,               /*       leaf 2 : fill body                   */
                28.0f,              /*       leaf 3 : 28px footer                 */
            GUI_END,                /*   close rows                               */
            // 128.0f,                  /*   leaf 4 : 128px right sidebar             */
        GUI_END,                    /* close columns                              */
    };

    /* A fixed-height band carved from the region's available area. */
    gui_rect_t band = gui()->content_rect();
    band.h = 180.0f;

    gui_rect_t cell[ GUI_LAYOUT_COLS ];
    u32        n = gui()->carve( FORM, band, -1.0f, cell, GUI_LAYOUT_COLS );
    if ( n >= 4 )
    {
        /* Sidebar -- a stack of nav buttons. */
        gui()->push_layout_overlay( cell[ 0 ] );
            gui()->stack();
            gui()->button( "Nav A" );
            gui()->button( "Nav B" );
            gui()->button( "Nav C" );
        gui()->pop_layout();

        /* Header. */
        gui()->push_layout_overlay( cell[ 1 ] );
            gui()->stack();
            gui()->text( "Header" );
        gui()->pop_layout();

        /* Body. */
        gui()->push_layout_overlay( cell[ 2 ] );
            gui()->child_begin( "##body", 0.0f, 0.0f, GUI_WIN_NO_CLIP ); 
                gui()->stack();
                gui()->text( "Body content fills the middle." );
                gui()->text( "The layout is one flat f32 form." );
                gui()->text( "Each leaf is a plain gui_rect_t." );
            gui()->child_end();
        gui()->pop_layout();

        /* Footer. */
        gui()->push_layout_overlay( cell[ 3 ] );
            gui()->stack();
            gui()->text_disabled( "Footer" );
        gui()->pop_layout();
    }

    /* The panels used absolute rects, so the window pen has not moved -- reserve the band. */
    gui()->dummy( 0.0f, band.h );

    gui()->window_end();
}

/*============================================================================================*/
/* HUD / overlay placement demo                                                                */
/*                                                                                              */
/* Free placement over one content area -- the companion to split/carve.  Each element takes    */
/* the HUD rect and returns its own rect (no pen, no flow), so the order below is just draw      */
/* order: a stretched top bar (gui()->anchor mixing per-axis stretch + point), corner-anchored   */
/* minimap / health / ammo (gui_anchor_box), a fraction-pinned banner (gui()->anchor pivot), and */
/* a centered crosshair (gui_rect_align).  Real widgets drop into an anchored rect via            */
/* push_layout_overlay.                                                                              */
/*============================================================================================*/

static void
show_hud_demo( bool* p_open )
{
    static const char* WIN = "HUD Overlay";
    if ( !gui()->window_begin( WIN, GUI_WIN_CLOSEABLE ) )
    {
        if ( p_open && !gui()->window_is_open( WIN ) )
            *p_open = false;
        gui()->window_end();
        return;
    }

    gui()->stack();
    gui()->text_wrapped( "Overlay placement: every element positions itself inside one HUD rect via "
                         "gui_anchor_box (corners), gui()->anchor (stretch / fraction) and "
                         "gui_rect_align (center).  No layout pen -- draw order is z order." );

    /* The HUD viewport: a fixed-height band carved from the region's available area. */
    gui_rect_t hud = gui()->content_rect();
    hud.h = 260.0f;

    const gui_pad_t  pad   = { 10, 10, 10, 10 };
    const u32        back  = 0xC0141820;   /* ABGR: dark translucent backdrop  */
    const u32        panel = 0xE0283038;   /* a HUD panel fill                 */
    const u32        ink   = 0xFFE0E8F0;   /* near-white text                  */
    const u32        good  = 0xFF50C878;   /* health green                     */
    const u32        warn  = 0xFF30A0FF;   /* ammo amber                       */

    gui()->draw_rect( hud.x, hud.y, hud.w, hud.h, back );

    /* Top status bar -- one anchor, two axis behaviors: stretch across X (min.x 0 -> max.x 1, the
       off.l / off.r become margins), point-pin to the top on Y (min.y == max.y == 0, fixed height). */
    {
        gui_anchor_t a = { .min = { 0.0f, 0.0f }, .max = { 1.0f, 0.0f },
                           .size = { 0.0f, 22.0f }, .pivot = { 0.0f, 0.0f },
                           .off  = { 10, 10, 10, 0 } };
        gui_rect_t bar = gui()->anchor( hud, a );
        gui()->draw_rect( bar.x, bar.y, bar.w, bar.h, panel );
        gui()->draw_text_in( bar, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ink, " Sector 7 - Clear" );
        gui()->draw_text_in( bar, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ink, "12:04  " );
    }

    /* Minimap -- fixed box anchored to the top-right corner with a uniform margin. */
    {
        gui_rect_t mm = gui_anchor_box( hud, 92.0f, 92.0f, GUI_ALIGN_RIGHT | GUI_ALIGN_TOP,
                                        ( gui_pad_t ){ 10, 42, 10, 10 } );
        gui()->draw_rect( mm.x, mm.y, mm.w, mm.h, panel );
        gui()->draw_circle( mm.x + mm.w * 0.5f, mm.y + mm.h * 0.5f, 5.0f, true, 0.0f, good );
        gui()->draw_text_in( mm, GUI_ALIGN_CENTER | GUI_ALIGN_BOTTOM, ink, "MAP" );
    }

    /* Health bar -- anchored bottom-left; a real progress_bar widget fills the anchored rect. */
    {
        gui_rect_t hb = gui_anchor_box( hud, 200.0f, 20.0f, GUI_ALIGN_LEFT | GUI_ALIGN_BOTTOM, pad );
        gui()->push_layout_overlay( hb );
            gui()->stack();
            gui()->push_style_color( GUI_COL_WIDGET_FG, good );
            gui()->progress_bar( 0.72f, "HP 72/100" );
            gui()->pop_style_color( 1 );
        gui()->pop_layout();
    }

    /* Ammo readout -- anchored bottom-right, drawn directly. */
    {
        gui_rect_t am = gui_anchor_box( hud, 120.0f, 40.0f, GUI_ALIGN_RIGHT | GUI_ALIGN_BOTTOM, pad );
        gui()->draw_rect( am.x, am.y, am.w, am.h, panel );
        gui()->draw_text_in( am, GUI_ALIGN_CENTER, warn, "24 / 120" );
    }

    /* Wave banner -- point-anchored 50% across, near the top, hung off its own center (pivot 0.5)
       so it stays visually centered regardless of width. */
    {
        gui_anchor_t a = { .min = { 0.5f, 0.18f }, .max = { 0.5f, 0.18f },
                           .size = { 120.0f, 24.0f }, .pivot = { 0.5f, 0.5f } };
        gui_rect_t banner = gui()->anchor( hud, a );
        gui()->draw_rect( banner.x, banner.y, banner.w, banner.h, panel );
        gui()->draw_text_in( banner, GUI_ALIGN_CENTER, ink, "WAVE 3" );
    }

    /* Crosshair -- a fixed box centered in the HUD; gui_rect_align is the pure-center case. */
    {
        gui_rect_t cr = gui_rect_align( hud, 18.0f, 18.0f, GUI_ALIGN_CENTER );
        f32 cx = cr.x + cr.w * 0.5f, cy = cr.y + cr.h * 0.5f;
        gui()->draw_line( cx - 9.0f, cy, cx + 9.0f, cy, 2.0f, ink );
        gui()->draw_line( cx, cy - 9.0f, cx, cy + 9.0f, 2.0f, ink );
    }

    /* Placement used absolute rects, so reserve the band so the window sizes around it. */
    gui()->dummy( 0.0f, hud.h );

    gui()->window_end();
}

/*============================================================================================*/
/* Demo setup                                                                                  */
/*============================================================================================*/

// Demonstrate creating a "main" fullscreen menu bar and populating it.
// Note the difference between BeginMainMenuBar() and BeginMenuBar():
// - BeginMenuBar() = menu-bar inside current window (which needs the ImGuiWindowFlags_MenuBar flag!)
// - BeginMainMenuBar() = helper to create menu-bar-sized window at the top of the main viewport + call BeginMenuBar() into it.

static bool show_demo             = true;
static bool show_font_browser_win = true;
static bool show_split_win        = true;
static bool show_hud_win          = true;
static void show_example_main_menu_bar()
{
    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "Examples" ) )
        {
            gui()->menu_item( "Demo Window",    NULL, &show_demo );
            gui()->menu_item( "Font Browser",   NULL, &show_font_browser_win );
            gui()->menu_item( "Split Panels",   NULL, &show_split_win );
            gui()->menu_item( "HUD Overlay",    NULL, &show_hud_win );
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

    bool skip_body =  false;
    if ( skip_body )
    {
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

    // GUI_FONT_NONE
    if ( !gui()->init( GUI_FONT_NONE ) ) {
         fprintf( stderr, "[sb_gui] gui->init failed\n" );
         goto shutdown;
    }
    gui_inited = true;

    // bool use_stb_font = true;
    // if ( use_stb_font )
    // {
    //     dev_font_init( NULL );
    //     char font_path[ 512 ];
    //     if ( dev_font_get( "JetBrainsMonoNL-Regular.ttf", 16, font_path, sizeof( font_path ) ) )
    //         gui()->font_load( font_path );
    //     else
    //         gui()->font_load( "assets/font/jetbrains_regular_16.orb_font" );
    // }
    // else
    // {
    //     gui()->font_load( "assets/font/jetbrains_regular_16.orb_font" );
    // }

    gui()->set_retained_skip( true );

    vp0 = gui()->viewport_open( win );
    if ( vp0 == GUI_VP_INVALID ) {
        fprintf( stderr, "[sb_gui] gui viewport_open (primary) failed\n" );
        goto shutdown;
    }

    /* ------------------------------------------------------------------------------ */
    /* GUI Style */

    bool modify_style = false;
    if ( modify_style )
    {
        gui_style_t* style = gui()->style_get();

        // Modify any colors
        style->colors[GUI_COL_WINDOW_BG] = GUI_COLOR( 0x20, 0x20, 0x20, 0xFF );
        style->colors[GUI_COL_TEXT]      = GUI_COLOR( 0xFF, 0xAA, 0x00, 0xFF );

        // Modify any layout metrics (authored for a baseline em=12)
        style->win_rounding    = 0;     // Square windows
        style->widget_rounding = 0;     // No bevel on buttons
        // style->widget_gap      = 12;    // More breathing room

        // Re-scale and apply the changes across the UI
        gui()->style_apply();
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

        gui()->debug_enable( true );

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

            if ( show_demo )
                show_demo_window( &show_demo );

            /* Force-open on transition (first show or menu re-open); not every frame or the X
               button close gets overridden before window_begin sees it. */
            static bool s_font_browser_prev = false;
            if ( show_font_browser_win && !s_font_browser_prev )
                gui()->window_set_open( "Font Browser", true );
            s_font_browser_prev = show_font_browser_win;
            if ( show_font_browser_win )
                show_font_browser( &show_font_browser_win );

            static bool s_split_prev = false;
            if ( show_split_win && !s_split_prev )
                gui()->window_set_open( "Split Panels", true );
            s_split_prev = show_split_win;
            if ( show_split_win )
                show_split_demo( &show_split_win );

            static bool s_hud_prev = false;
            if ( show_hud_win && !s_hud_prev )
                gui()->window_set_open( "HUD Overlay", true );
            s_hud_prev = show_hud_win;
            if ( show_hud_win )
                show_hud_demo( &show_hud_win );

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
    dev_font_shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
