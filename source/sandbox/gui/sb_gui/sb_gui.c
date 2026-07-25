/*==============================================================================================

    sandbox/gui/sb_gui/sb_gui.c -- ImGui Demo Replication

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline.

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
    bool show_atlas;   /* independent of preview_ready -- shows the active font's atlas as-is */
    bool atlas_2x;     /* draw the atlas at 2x native pixel size instead of 1x */

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
    gui()->window_set_next_size( 640.0f, 640.0f, GUI_COND_ONCE );
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
        gui()->new_line( -1.0f );

        /* NOTE -- this preview is NOT isolated to these lines.  The renderer has no per-run font:
           text commands store only position/colour/clip (see GUI_CMD_TEXT in gui_emit_draw.c), and the
           glyph atlas + UVs are resolved at DEFERRED tessellation time from one global active font
           (tess_text_n / font_atlas_idx in gui_build_tess.c).  So whichever font is active when the
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

    /* --- Atlas ------------------------------------------------------------------ */
    /* Independent of the bake/preview flow above -- shows the CURRENTLY ACTIVE font's atlas
       (whatever the app booted with, or last font_use'd), so checking packing/coverage doesn't
       require baking anything through this window first. */
    gui()->separator_text( "Atlas" );
    if ( gui()->button( "Show Atlas" ) )
        s_fb.show_atlas = !s_fb.show_atlas;
    gui()->same_line( -1 );
    gui()->checkbox( "2x", &s_fb.atlas_2x );

    if ( s_fb.show_atlas )
    {
        u32 active_id = gui()->font_active_id();
        u32 atlas_idx = gui()->font_atlas_idx( active_id );
        if ( atlas_idx )
        {
            gui_vec2_t asz = gui()->font_atlas_size( active_id );

            gui()->textf( "Active font #%u -- %.0f x %.0f px  (bindless #%u)",
                          active_id, asz.x, asz.y, atlas_idx );

            /* Native resolution (or 2x via the checkbox) -- no fit-to-window scaling, so packing/
               coverage reads exactly as baked.  Same red-channel-only nuance as before: the atlas
               is R8_UNORM coverage, sampled here through the RGBA path, so glyph ink renders red,
               not white. */
            f32 scale = s_fb.atlas_2x ? 2.0f : 1.0f;
            gui()->image_texture( atlas_idx, asz.x * scale, asz.y * scale, 0 );
        }
        else
        {
            gui()->text_disabled( "No active font atlas." );
        }
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
            // 128.0f,                  /*   leaf 4 : 128px right sidebar         */
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
            gui()->child_begin( "##body", 0.0f, 0.0f, 0 );   /* clip content to the body rect */
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
    gui()->empty( 0.0f, band.h );

    gui()->window_end();
}

/*============================================================================================
    HUD / overlay placement demo                                                                

    Free placement over one content area -- the companion to split/carve.  Each element takes    
    the HUD rect and returns its own rect (no pen, no flow), so the order below is just draw      
    order: a stretched top bar (gui()->anchor mixing per-axis stretch + point), corner-anchored   
    minimap / health / ammo (gui_anchor_box), a fraction-pinned banner (gui()->anchor pivot),   
    and a centered crosshair (gui_rect_align).  Real widgets drop into an anchored rect via     
    push_layout_overlay.                                                                        
==============================================================================================*/

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
    gui()->empty( 0.0f, hud.h );

    gui()->window_end();
}

/*============================================================================================*/
/* Root region demo                                                                            */
/*                                                                                              */
/* gui_region_begin/end: a fixed rect with a few widgets and no window chrome (no title, no      */
/* drag, no close button).  The "Move" button below proves the position is just a caller-owned  */
/* value -- unlike a window, a region cannot be dragged, so relocating it is entirely the app's  */
/* job each frame.                                                                              */
/*============================================================================================*/

static void
show_region_demo( void )
{
    static const struct { f32 x, y; } spots[] =
    {
        {  40.0f, 340.0f }, { 500.0f, 340.0f }, { 500.0f, 520.0f }, {  40.0f, 520.0f },
    };
    static int slot = 0;

    gui()->region_begin( "Region Demo", spots[ slot ].x, spots[ slot ].y, 260.0f, 160.0f,
                         GUI_REGION_BG, GUI_WIN_NOSCROLL );
        gui()->stack();
        gui()->text( "A region: fixed rect, no window chrome." );
        gui()->textf( "pos %.0f, %.0f", spots[ slot ].x, spots[ slot ].y );
        if ( gui()->button( "Move" ) )
            slot = ( slot + 1 ) % ( sizeof( spots ) / sizeof( spots[ 0 ] ) );
        gui()->textf( "hover:%d active:%d capture:%d",
                      gui()->is_item_hovered(), gui()->is_item_active(),
                      gui()->want_capture_mouse() );

    gui()->row2( 0.5f, 0.5f );
    gui()->button( "A" );                       // default: shrinks to label, seated by mod.align
    gui()->next_item_fit( 1.0f );
    gui()->button( "B" );                       // overridden: stretches to fill its half

    gui()->region_end();
}

/*============================================================================================*/
/* Drag and drop demo                                                                          */
/*                                                                                              */
/* Exercises the gui drag-and-drop API end to end: every list row is both a drag SOURCE          */
/* (drag_source_begin + drag_payload_set + a cursor tooltip preview) and a drop TARGET           */
/* (drag_target_begin + drag_payload_accept on the release frame).  Drop a row onto another      */
/* row to insert before it -- within one list (reorder) or across lists (move) -- or onto a      */
/* list's append button.  The payload is a tiny (list, index) struct copied by value.            */
/*============================================================================================*/

#define DD_LIST_CAP  8
#define DD_NAME_CAP  24

typedef struct { i32 list; i32 idx; } dd_ref_t;

static char s_dd_items[ 2 ][ DD_LIST_CAP ][ DD_NAME_CAP ];
static i32  s_dd_count[ 2 ];
static bool s_dd_init;
static char s_dd_status[ 96 ];

/* Move item (sl,si) so it lands at slot di of list dl (di < 0 or past the end = append). */
static void
dd_move( i32 sl, i32 si, i32 dl, i32 di )
{
    if ( sl != dl && s_dd_count[ dl ] >= DD_LIST_CAP )
    {
        snprintf( s_dd_status, sizeof( s_dd_status ), "List %c is full.", 'A' + dl );
        return;
    }

    char tmp[ DD_NAME_CAP ];
    memcpy( tmp, s_dd_items[ sl ][ si ], DD_NAME_CAP );

    for ( i32 i = si; i + 1 < s_dd_count[ sl ]; ++i )              /* remove from source */
        memcpy( s_dd_items[ sl ][ i ], s_dd_items[ sl ][ i + 1 ], DD_NAME_CAP );
    s_dd_count[ sl ]--;

    if ( dl == sl && di > si )
        di--;                                                       /* removal shifted the slot */
    if ( di < 0 || di > s_dd_count[ dl ] )
        di = s_dd_count[ dl ];

    for ( i32 i = s_dd_count[ dl ]; i > di; --i )                   /* open the hole */
        memcpy( s_dd_items[ dl ][ i ], s_dd_items[ dl ][ i - 1 ], DD_NAME_CAP );
    memcpy( s_dd_items[ dl ][ di ], tmp, DD_NAME_CAP );
    s_dd_count[ dl ]++;

    snprintf( s_dd_status, sizeof( s_dd_status ), "Moved '%s' to list %c slot %d.",
              tmp, 'A' + dl, di );
}

/* One list column: rows are sources + targets; the trailing button appends a drop. */
static void
dd_list_column( i32 list )
{
    gui()->push_id_int( list );
    gui()->stack();
    gui()->textf( "List %c (%d)", 'A' + list, s_dd_count[ list ] );

    for ( i32 i = 0; i < s_dd_count[ list ]; ++i )
    {
        gui()->push_id_int( i );

        bool sel = false;
        gui()->selectable( s_dd_items[ list ][ i ], &sel );

        /* Source: dragging this row carries its (list, index). */
        if ( gui()->drag_source_begin( GUI_DRAG_NONE ) )
        {
            dd_ref_t ref = { list, i };
            gui()->drag_payload_set( "DD_ITEM", &ref, sizeof( ref ) );
            gui()->textf( "Move '%s'", s_dd_items[ list ][ i ] );   /* cursor preview */
            gui()->drag_source_end();
        }

        /* Target: dropping another row here inserts it before this row. */
        if ( gui()->drag_target_begin() )
        {
            const gui_drag_payload_t* p = gui()->drag_payload_accept( "DD_ITEM", GUI_DRAG_NONE );
            if ( p )
            {
                dd_ref_t ref;
                memcpy( &ref, p->data, sizeof( ref ) );
                dd_move( ref.list, ref.idx, list, i );
            }
            gui()->drag_target_end();
        }

        gui()->pop_id();
    }

    gui()->small_button( "( drop to append )" );
    if ( gui()->drag_target_begin() )
    {
        const gui_drag_payload_t* p = gui()->drag_payload_accept( "DD_ITEM", GUI_DRAG_NONE );
        if ( p )
        {
            dd_ref_t ref;
            memcpy( &ref, p->data, sizeof( ref ) );
            dd_move( ref.list, ref.idx, list, -1 );
        }
        gui()->drag_target_end();
    }

    gui()->pop_id();
}

static void
show_dragdrop_demo( bool* p_open )
{
    static const char* WIN = "Drag and Drop";
    if ( !gui()->window_begin( WIN, GUI_WIN_CLOSEABLE ) )
    {
        if ( p_open && !gui()->window_is_open( WIN ) )
            *p_open = false;
        gui()->window_end();
        return;
    }

    if ( !s_dd_init )
    {
        static const char* seed_a[] = { "Apple", "Banana", "Cherry", "Dates" };
        static const char* seed_b[] = { "Iron", "Copper", "Silver" };
        for ( i32 i = 0; i < 4; ++i ) snprintf( s_dd_items[ 0 ][ i ], DD_NAME_CAP, "%s", seed_a[ i ] );
        for ( i32 i = 0; i < 3; ++i ) snprintf( s_dd_items[ 1 ][ i ], DD_NAME_CAP, "%s", seed_b[ i ] );
        s_dd_count[ 0 ] = 4;
        s_dd_count[ 1 ] = 3;
        snprintf( s_dd_status, sizeof( s_dd_status ), "Drag a row onto a row or an append button." );
        s_dd_init = true;
    }

    gui()->stack();
    gui()->text_wrapped( "Every row is a drag source AND a drop target: drag one onto another to "
                         "insert before it (same list = reorder, other list = move), or onto the "
                         "append button." );
    gui()->separator();

    gui()->row2( 0.5f, 0.5f );
    gui()->child_begin( "##list_a", 0.0f, 220.0f, GUI_WIN_NONE );
    dd_list_column( 0 );
    gui()->child_end();
    gui()->child_begin( "##list_b", 0.0f, 220.0f, GUI_WIN_NONE );
    dd_list_column( 1 );
    gui()->child_end();

    gui()->stack();
    gui()->text_disabled( s_dd_status );
    gui()->textf( "drag_active: %d", gui()->drag_active() );

    gui()->window_end();
}

/*============================================================================================*/
/* Tab groups -- windows merged onto one floating frame (window_tab / title-bar drop gesture)  */
/*============================================================================================*/

static void
tab_group_member( const char* title, const char* body, f32 seed_x )
{
    gui()->window_set_next_pos ( seed_x, 340.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 260.0f, 170.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( title, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( body );
        gui()->textf( "tabbed: %s", gui()->window_is_docked( title ) ? "yes" : "no" );
    }
    gui()->window_end();
}

static void
show_tabgroup_demo( bool* p_open )
{
    /* NO_TAB_TARGET: this is a control panel -- it explains the feature, it must not itself
       become a tab (its early-out below would then skip the member windows).  A drop on its
       title bar shows no chip. */
    static const char* WIN = "Tab Groups";
    gui()->window_set_next_size( 340.0f, 240.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( WIN, GUI_WIN_CLOSEABLE | GUI_WIN_NO_TAB_TARGET ) )
    {
        gui()->stack();
        gui()->text_wrapped( "Drag one window's title bar onto another's and drop on the center chip "
                             "to merge them into a floating tab group -- no split panes.  Drag a tab "
                             "out of the strip to pull a window back out (the group dissolves when one "
                             "tab remains).  Drag the strip's empty band to move the group, its edges "
                             "to resize, tabs sideways to reorder." );
        gui()->separator();

        if ( gui()->button( "Group programmatically" ) )
        {
            gui()->window_tab( "Tab Beta",  "Tab Alpha" );
            gui()->window_tab( "Tab Gamma", "Tab Alpha" );
        }
        if ( gui()->button( "Ungroup all" ) )
        {
            gui()->dock_undock( "Tab Beta" );
            gui()->dock_undock( "Tab Gamma" );
            gui()->dock_undock( "Tab Alpha" );
        }
    }
    else if ( p_open && !gui()->window_is_open( WIN ) )
    {
        *p_open = false;
    }
    gui()->window_end();

    /* Member windows emit unconditionally: a window tabbed behind another returns false from
       window_begin, and gating siblings on that would blank the whole demo. */
    tab_group_member( "Tab Alpha", "The Alpha window.  Drop another window on this title bar to grow "
                                   "a tab group around it.", 60.0f );
    tab_group_member( "Tab Beta",  "The Beta window.  Drag this by its title bar onto Alpha's.", 340.0f );
    tab_group_member( "Tab Gamma", "The Gamma window.  Groups take a third tab just the same.", 620.0f );
}

/*============================================================================================*/
/* Style / theme editor                                                                        */
/*                                                                                             */
/* Pick a built-in theme, then tune every SKIN + METRIC knob live.  The flow is the one the    */
/* gui style API is built around: theme_list/theme_set switch named presets; style_peek reads   */
/* the base style WITHOUT marking it anonymous (so the theme combo keeps naming the active      */
/* theme until an edit lands); style_get commits an edit (theme goes "(custom)"); style_apply   */
/* rescales the active metrics.  Widgets edit a local copy of the base style and the whole copy */
/* is committed once per frame only when something actually changed -- so merely opening the    */
/* window never disturbs the theme.                                                             */
/*============================================================================================*/

/* One color slot -> a color_edit4 bound to the packed u32 field.  Returns true on edit. */
static bool
se_color( const char* label, u32* field )
{
    f32 c[ 4 ] = {
        (f32)(   *field         & 0xFF ) / 255.0f,
        (f32)( ( *field >> 8  ) & 0xFF ) / 255.0f,
        (f32)( ( *field >> 16 ) & 0xFF ) / 255.0f,
        (f32)( ( *field >> 24 ) & 0xFF ) / 255.0f,
    };
    if ( gui()->color_edit4( label, c, GUI_COLOR_EDIT_NONE ) )
    {
        u8 r = (u8)( c[ 0 ] * 255.0f + 0.5f ), g = (u8)( c[ 1 ] * 255.0f + 0.5f );
        u8 b = (u8)( c[ 2 ] * 255.0f + 0.5f ), a = (u8)( c[ 3 ] * 255.0f + 0.5f );
        *field = GUI_COLOR( r, g, b, a );
        return true;
    }
    return false;
}

/* One u8 metric/skin scalar -> an integer slider over [lo,hi].  Returns true on edit. */
static bool
se_u8( const char* label, u8* field, i32 lo, i32 hi )
{
    i32 v = *field;
    if ( gui()->slider_int( label, &v, lo, hi ) )
    {
        *field = (u8)v;
        return true;
    }
    return false;
}

/* One enum-valued u8 knob -> a combo of named variants.  Returns true on selection change. */
static bool
se_enum( const char* label, u8* field, const char* const* names, i32 count )
{
    i32  cur     = ( *field < (u8)count ) ? *field : 0;
    bool changed = false;
    if ( gui()->combo_begin( label, names[ cur ], GUI_COMBO_NONE ) )
    {
        for ( i32 i = 0; i < count; ++i )
        {
            bool sel = ( i == cur );
            if ( gui()->selectable( names[ i ], &sel ) )
            {
                *field  = (u8)i;
                changed = true;
            }
        }
        gui()->combo_end();
    }
    return changed;
}

/* Display names for the color slots, indexed by gui_col_t. */
static const char* const k_col_names[ GUI_COL_COUNT ] =
{
    [ GUI_COL_TEXT          ] = "Text",
    [ GUI_COL_TEXT_DIM      ] = "Text Dim",
    [ GUI_COL_WINDOW_BG     ] = "Window BG",
    [ GUI_COL_CHILD_BG      ] = "Child BG",
    [ GUI_COL_TITLE_BG      ] = "Title BG",
    [ GUI_COL_BORDER        ] = "Border",
    [ GUI_COL_WIDGET_BG     ] = "Widget BG",
    [ GUI_COL_WIDGET_HOT    ] = "Widget Hot",
    [ GUI_COL_WIDGET_ACT    ] = "Widget Active",
    [ GUI_COL_WIDGET_FG     ] = "Widget FG",
    [ GUI_COL_CHECK_MARK    ] = "Check Mark",
    [ GUI_COL_SLIDER_TRACK  ] = "Slider Track",
    [ GUI_COL_RESIZE_HOT    ] = "Resize Hot",
    [ GUI_COL_INPUT_BG      ] = "Input BG",
    [ GUI_COL_INPUT_FOCUS   ] = "Input Focus",
    [ GUI_COL_CURSOR        ] = "Cursor",
    [ GUI_COL_NAV_HIGHLIGHT ] = "Nav Highlight",
    [ GUI_COL_NAV_CAPTURE   ] = "Nav Capture",
    [ GUI_COL_FOCUS_BORDER  ] = "Focus Border",
    [ GUI_COL_USER_0        ] = "User 0",
    [ GUI_COL_USER_1        ] = "User 1",
    [ GUI_COL_USER_2        ] = "User 2",
    [ GUI_COL_USER_3        ] = "User 3",
    [ GUI_COL_USER_4        ] = "User 4",
    [ GUI_COL_USER_5        ] = "User 5",
    [ GUI_COL_USER_6        ] = "User 6",
    [ GUI_COL_USER_7        ] = "User 7",
};

static void
show_style_editor( bool* p_open )
{
    static const char* WIN = "Style Editor";
    gui()->window_set_next_size( 340.0f, 620.0f, GUI_COND_ONCE );
    if ( !gui()->window_begin( WIN, GUI_WIN_CLOSEABLE ) )
    {
        if ( p_open && !gui()->window_is_open( WIN ) )
            *p_open = false;
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* --- Theme -----------------------------------------------------------------------------
       Theme controls go FIRST so a preset switch happens before we snapshot the base style
       below -- the tuning widgets then reflect the newly selected theme the same frame. */
    gui()->separator_text( "Theme" );

    const char* active = gui()->theme_get();            /* NULL after an anonymous edit */
    u32         n_themes;
    const gui_theme_t* themes = gui()->theme_list( &n_themes );

    /* [ combo (fill) | Reset (fixed) ] as an explicit two-track row, NOT combo + same_line(button):
       a fill widget leaves the pen at the region's right edge, so a same_line natural-width button
       reaches past content_w and (content_w chasing that) the row grows without bound each frame.
       A fixed second track parks the button in a content_w-independent cell instead.  The combo hides
       its own label ("##") since the separator above already titles the section. */
    f32 reset_w = gui()->button_width( "Reset" );
    gui()->row_cols( 0.0f, (f32[]){ 1.0f, reset_w, GUI_END } );

    if ( gui()->combo_begin( "##Theme", active ? active : "(custom)", GUI_COMBO_NONE ) )
    {
        for ( u32 i = 0; i < n_themes; ++i )
        {
            bool sel = ( active && strcmp( active, themes[ i ].name ) == 0 );
            if ( gui()->selectable( themes[ i ].name, &sel ) )
                gui()->theme_set( themes[ i ].name );
        }
        gui()->combo_end();
    }

    if ( gui()->button( "Reset" ) )
        gui()->theme_reset();   /* revert to the active theme's authored values, clear stacks */

    gui()->stack();   /* back to a single full-width column for the rest of the panel */

    if ( !active )
        gui()->text_disabled( "Edited -- pick a theme above to revert." );

    /* --- Snapshot --------------------------------------------------------------------------
       Read the base through style_peek (does not disturb the theme name).  Widgets edit this
       local copy; the whole copy is committed once at the end only if something changed. */
    gui_style_t work    = *gui()->style_peek();
    bool        changed = false;

    /* --- Colors ---------------------------------------------------------------------------- */
    gui()->separator_text( "Colors" );
    f32 label_width = gui()->text_size( "widget_rounding" ).x;
    gui()->form( GUI_LABEL_RIGHT, label_width );
    // gui()->field_label_left( 90.0f );

    for ( u32 i = 0; i < GUI_COL_COUNT; ++i )
        changed |= se_color( k_col_names[ i ], &work.colors[ i ] );

    /* --- Metrics (can move rects) ---------------------------------------------------------- */
    gui()->separator_text( "Metrics" );
    changed |= se_u8( "line_size",     &work.line_size,     8, 48  );
    changed |= se_u8( "widget_gap",    &work.widget_gap,    0, 24  );
    changed |= se_u8( "widget_pad",    &work.widget_pad,    0, 24  );
    changed |= se_u8( "min_cell_w",    &work.min_cell_w,    8, 128 );
    changed |= se_u8( "grid_quantum",  &work.grid_quantum,  0, 32  );
    changed |= se_u8( "win_border",    &work.win_border,    0, 8   );
    changed |= se_u8( "win_title_h",   &work.win_title_h,   12, 48 );
    changed |= se_u8( "checkbox_sz",   &work.checkbox_sz,   8, 32  );
    changed |= se_u8( "slider_knob_w", &work.slider_knob_w, 4, 32  );

    /* --- Skin: rounding + fine geometry (paint only) --------------------------------------- */
    gui()->separator_text( "Skin - Rounding" );
    changed |= se_u8( "win_rounding",     &work.win_rounding,     0, 24 );
    changed |= se_u8( "widget_rounding",  &work.widget_rounding,  0, 16 );
    changed |= se_u8( "grab_rounding",    &work.grab_rounding,    0, 16 );
    changed |= se_u8( "win_focus_border", &work.win_focus_border, 0, 8  );
    changed |= se_u8( "checkmark_pad",   &work.checkmark_pad,   0, 12 );
    changed |= se_u8( "cursor_w",        &work.cursor_w,        1, 6  );
    changed |= se_u8( "cursor_inset",    &work.cursor_inset,    0, 12 );

    /* --- Skin: shape variants (enum combos) ------------------------------------------------ */
    gui()->separator_text( "Skin - Shapes" );
    static const char* const nm_check   [] = { "Tick", "Disc", "Cross" };
    static const char* const nm_bullet  [] = { "Disc", "Square" };
    static const char* const nm_arrow   [] = { "Filled", "Chevron" };
    static const char* const nm_sep     [] = { "Solid", "Dashed" };
    static const char* const nm_progress[] = { "Solid", "Gradient" };
    static const char* const nm_knob    [] = { "Bar", "Circle" };
    static const char* const nm_menu    [] = { "Plain", "Box" };
    changed |= se_enum( "check_style",     &work.check_style,     nm_check,    3 );
    changed |= se_enum( "bullet_style",    &work.bullet_style,    nm_bullet,   2 );
    changed |= se_enum( "arrow_style",     &work.arrow_style,     nm_arrow,    2 );
    changed |= se_enum( "separator_style", &work.separator_style, nm_sep,      2 );
    changed |= se_enum( "progress_style",  &work.progress_style,  nm_progress, 2 );
    changed |= se_enum( "slider_knob",     &work.slider_knob,     nm_knob,     2 );
    changed |= se_enum( "menu_check",      &work.menu_check,      nm_menu,     2 );
    
    gui()->form( GUI_LABEL_RIGHT, 0.0 );

    /* --- Live sample of what the knobs above affect ---------------------------------------- */
    gui()->separator_text( "Preview" );
    static bool  sample_check = true;
    static f32   sample_val   = 0.4f;
    static i32   sample_int   = 3;
    gui()->checkbox( "Checkbox", &sample_check );
    gui()->button( "Button" );

    gui()->form( GUI_LABEL_RIGHT, label_width );
    gui()->slider_float( "Slider", &sample_val, 0.0f, 1.0f );
    gui()->slider_int( "Steps", &sample_int, 0, 10 );
    gui()->progress_bar( sample_val, NULL );

    /* Commit once: writing through style_get marks the theme anonymous (an intentional edit),
       then style_apply rescales the active metrics from the new base. */
    if ( changed )
    {
        *gui()->style_get() = work;
        gui()->style_apply();
    }

    gui()->window_end();
}

/*============================================================================================*/
/* Toolbar icon strip demo                                                                     */
/*                                                                                              */
/* Exercises gui_toolbar.c end to end: toolbar_begin/end (id scope + bar(), caller-scaled via    */
/* scale_push/scale_pop), toolbar_button (press), toolbar_toggle (latched state),                */
/* toolbar_separator, and toolbar_dropdown_begin/end (a split button opening an arbitrary-widget */
/* popup -- here plain menu_item rows, proving the popup body is not limited to a fixed row      */
/* type).  A second strip reuses the same icon ids and widget id strings to prove                */
/* toolbar_begin's id scope keeps two strips from colliding, scaled larger to prove mixed        */
/* toolbar sizes coexist.                                                                        */
/*============================================================================================*/

static void
tb_make_save( u8* p, i32 n )
{
    /* A floppy-disk silhouette: body, a clipped top-right corner, and a label window cut out. */
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            bool body  = ( x >= 3 && x <= 28 && y >= 3 && y <= 28 );
            bool notch = ( x >= 20 && y <= 8 );
            bool label = ( x >= 7 && x <= 24 && y >= 15 && y <= 25 );
            p[ y * n + x ] = ( body && !notch && !label ) ? 255 : 0;
        }
}

static void
tb_make_grid( u8* p, i32 n )
{
    /* A 3x3 grid glyph, for a grid-snap style toggle.  2px strokes: the icon atlas samples
       NEAREST with no mip / box filter, and this glyph draws well below its native 32x32 in a
       toolbar cell, so a 1px line falls between sampled texels and vanishes -- 2px survives the
       minification. */
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            bool in     = ( x >= 3 && x <= 28 && y >= 3 && y <= 28 );
            bool border = in && ( x <= 4 || x >= 27 || y <= 4 || y >= 27 );
            bool vline  = in && ( ( x >= 11 && x <= 12 ) || ( x >= 19 && x <= 20 ) );
            bool hline  = in && ( ( y >= 11 && y <= 12 ) || ( y >= 19 && y <= 20 ) );
            p[ y * n + x ] = ( border || vline || hline ) ? 255 : 0;
        }
}

static void
tb_make_wire( u8* p, i32 n )
{
    /* A boxed X, for a wireframe style toggle. */
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            bool in    = ( x >= 3 && x <= 28 && y >= 3 && y <= 28 );
            i32  dx    = x - y;
            i32  dy    = x + y - ( n - 1 );
            bool diag1 = dx >= -1 && dx <= 1;
            bool diag2 = dy >= -1 && dy <= 1;
            p[ y * n + x ] = ( in && ( diag1 || diag2 ) ) ? 255 : 0;
        }
}

static void
tb_make_view( u8* p, i32 n )
{
    /* An eye glyph (lens ring + pupil), for the view-mode dropdown. */
    f32 cx = (f32)n * 0.5f, cy = (f32)n * 0.5f;
    for ( i32 y = 0; y < n; ++y )
        for ( i32 x = 0; x < n; ++x )
        {
            f32  px    = (f32)x + 0.5f - cx, py = (f32)y + 0.5f - cy;
            f32  lens  = ( px * px ) / ( 15.0f * 15.0f ) + ( py * py ) / ( 8.0f * 8.0f );
            f32  pupil = sqrtf( px * px + py * py );
            bool ring  = lens <= 1.0f && lens >= 0.55f;
            bool dot   = pupil <= 4.0f;
            p[ y * n + x ] = ( ring || dot ) ? 255 : 0;
        }
}

static void
show_toolbar_demo( bool* p_open )
{
    // F:\orb>bin\image_tool.exe split assets/icon_source/general_1.png 8 8 -key -out assets/icon_dump/

    static const char* WIN = "Toolbar Icons";
    if ( !gui()->window_begin( WIN, GUI_WIN_CLOSEABLE | GUI_WIN_CAN_AUTOSIZE ) )
    {
        if ( p_open && !gui()->window_is_open( WIN ) )
            *p_open = false;
        gui()->window_end();
        return;
    }

    static gui_icon_id_t ic_save = GUI_ICON_NONE;
    static gui_icon_id_t ic_grid = GUI_ICON_NONE;
    static gui_icon_id_t ic_wire = GUI_ICON_NONE;
    static gui_icon_id_t ic_view = GUI_ICON_NONE;
    static gui_icon_id_t ic_dl   = GUI_ICON_NONE;   // loaded from disk (PNG), not procedural
    if ( ic_save == GUI_ICON_NONE )
    {
        static u8 buf[ 32 * 32 ];
        tb_make_save( buf, 32 ); ic_save = gui()->register_icon( "tb_save", 32, 32, buf );
        tb_make_grid( buf, 32 ); ic_grid = gui()->register_icon( "tb_grid", 32, 32, buf );
        tb_make_wire( buf, 32 ); ic_wire = gui()->register_icon( "tb_wire", 32, 32, buf );
        tb_make_view( buf, 32 ); ic_view = gui()->register_icon( "tb_view", 32, 32, buf );

        /* Demonstrate the from-disk icon path: decode assets/icon/folder_icon.png to R8
           coverage and register it exactly like the procedural icons above.  load_icon resolves
           the path itself (asset_path, engine-relative like the built-in fonts). */
        ic_dl = gui()->load_icon( "folder", "assets/icon/audio2.png" );
    }

    static bool grid_snap   = true;
    static bool wireframe   = false;
    static i32  save_clicks = 0;
    static i32  view_mode   = 0;
    static const char* const view_names[] = { "Lit", "Unlit", "Wireframe" };

    gui()->stack();
    gui()->text_wrapped( "toolbar_begin/end brackets a bar() run, id-scoped so two strips never "
                         "collide.  It does not push a scale itself -- the caller wraps it in "
                         "scale_push/scale_pop, so one app can mix toolbar sizes.  toolbar_button "
                         "presses, toolbar_toggle latches, toolbar_dropdown_begin/end opens an "
                         "arbitrary-widget popup (here, plain menu_item rows) anchored below the "
                         "split button." );
    gui()->separator();

    gui()->scale_push( GUI_SCALE_BAR );
    gui()->toolbar_begin( "main" );
        if ( gui()->toolbar_button( "##save", ic_save, "Save (Ctrl+S)" ) )
            save_clicks++;
        gui()->toolbar_toggle( "##grid", ic_grid, &grid_snap, "Grid Snap" );
        gui()->toolbar_toggle( "##wire", ic_wire, &wireframe, "Wireframe" );
        gui()->toolbar_separator();
        if ( gui()->toolbar_button( "##folder", ic_dl, "Folder (loaded from folder_icon.png)" ) )
            save_clicks++;
        gui()->toolbar_separator();
        if ( gui()->toolbar_dropdown_begin( "##view", ic_view, "View Mode" ) )
        {
            for ( i32 i = 0; i < 3; ++i )
            {
                bool sel = ( i == view_mode );
                if ( gui()->menu_item( view_names[ i ], NULL, &sel ) )
                    view_mode = i;
            }
            gui()->toolbar_dropdown_end();
        }
    gui()->toolbar_end();
    gui()->scale_pop();

    gui()->separator();
    gui()->textf( "save clicks: %d", save_clicks );
    gui()->textf( "grid snap: %s", grid_snap ? "on" : "off" );
    gui()->textf( "wireframe: %s", wireframe ? "on" : "off" );
    gui()->textf( "view mode: %s", view_names[ view_mode ] );

    /* A second strip, same icon ids and widget id strings -- proves toolbar_begin's push_id
       scope keeps it from colliding with the first strip's state.  Scaled GUI_SCALE_ROOMY
       (larger than the first strip's GUI_SCALE_BAR) to prove mixed toolbar sizes coexist. */
    gui()->separator_text( "Second strip (independent id scope, larger scale)" );
    static bool locked = false;
    gui()->scale_push( GUI_SCALE_ROOMY );
    gui()->toolbar_begin( "secondary" );
        gui()->toolbar_toggle( "##grid", ic_grid, &locked, "Lock" );
        if ( gui()->toolbar_button( "##save", ic_save, "Save (secondary)" ) )
            save_clicks++;
    gui()->toolbar_end();
    gui()->scale_pop();

    gui()->window_end();
}

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

static bool show_demo             = false;
static bool show_font_browser_win = false;
static bool show_split_win        = false;
static bool show_hud_win          = false;
static bool show_region_win       = false;
static bool show_dragdrop_win     = false;
static bool show_tabgroup_win     = false;
static bool show_style_win        = false;
static bool show_toolbar_win      = false;

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
            gui()->menu_item( "Region Demo",    NULL, &show_region_win );
            gui()->menu_item( "Drag and Drop",  NULL, &show_dragdrop_win );
            gui()->menu_item( "Tab Groups",     NULL, &show_tabgroup_win );
            gui()->menu_item( "Style Editor",   NULL, &show_style_win );
            gui()->menu_item( "Toolbar Icons",  NULL, &show_toolbar_win );
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

    /* Static placeholder text (not wired to a real per-frame delta) -- if this were made to
       recompute from an ever-growing clock every frame, its changing content would keep this
       window's command hash different frame to frame forever, which would keep frame_dirty()
       true forever and defeat the idle-skip entirely (the exact problem volatile widgets exist
       to route around; see demo_volatile_pulse_cb above for the widget that keeps animating anyway). */
    // gui()->textf("Application average %.3f ms/frame (%.1f FPS)", 6.061f, 165.0f);

    gui()->volatile_cb( "volatile_pulse_demo", demo_volatile_pulse_cb );
    // gui()->same_line( 0 );
    gui()->text( "<- volatile widget: keeps pulsing on idle frames, no full rebuild" );
    
    for ( int i = 0; i < 40; i++ )
    {
        gui()->textf( "Line %d", i );
    }

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
    /* One-call setup: gui owns the main window + render context end to end (boot path).
       os_chrome keeps the stock Win32 frame this demo has always had -- flip it off for a
       borderless window with the gui chrome shell auto-emitted each frame.  The frame hooks
       are the OS services gui cannot reach itself (it links only app + rhi); .debug arms
       the gui hotkey driver (see debug_enable in gui_api.h). */

    int      ret_code    = 1;
    bool     draw_inited = false;

    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "sb_gui",
        .w         = 1280, .h = 720,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO_16,   // GUI_FONT_JETBRAINS_16
        .clock     = sys_tick_seconds,
        .sleep     = sys_sleep_milliseconds,
        .wait      = sys_wait_for_os_events_ms,
        .clear     = { 0.15f, 0.15f, 0.20f, 1.00f },
        .debug     = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui] gui->boot failed\n" );
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

    /* gui()->init already loaded the built-in preset font above; dev_font_init is still needed
       here for the Font Browser demo (show_font_browser), which scans/bakes fonts from disk
       independent of whatever font the GUI itself started with. */
    dev_font_init( NULL );

    gui()->set_retained_skip( true );

    /* ------------------------------------------------------------------------------ */
    /* GUI Style */

    bool modify_style = false;
    if ( modify_style )
    {
        gui_style_t* style = gui()->style_get();

        // Modify any colors
        style->colors[GUI_COL_WINDOW_BG] = GUI_COLOR( 0x20, 0x20, 0x20, 0xFF );
        style->colors[GUI_COL_TEXT]      = GUI_COLOR( 0xFF, 0xAA, 0x00, 0xFF );

        // Modify any skin (STYLE) knobs -- metrics are authored for a baseline em=12
        style->win_rounding    = 0;         // Square windows
        style->widget_rounding = 0;         // No bevel on buttons
        style->grab_rounding = 0;
        // style->widget_gap      = 12;     // More breathing room

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

            if ( show_region_win )
                show_region_demo();

            static bool s_dragdrop_prev = false;
            if ( show_dragdrop_win && !s_dragdrop_prev )
                gui()->window_set_open( "Drag and Drop", true );
            s_dragdrop_prev = show_dragdrop_win;
            if ( show_dragdrop_win )
                show_dragdrop_demo( &show_dragdrop_win );

            static bool s_tabgroup_prev = false;
            if ( show_tabgroup_win && !s_tabgroup_prev )
                gui()->window_set_open( "Tab Groups", true );
            s_tabgroup_prev = show_tabgroup_win;
            if ( show_tabgroup_win )
                show_tabgroup_demo( &show_tabgroup_win );

            static bool s_style_prev = false;
            if ( show_style_win && !s_style_prev )
                gui()->window_set_open( "Style Editor", true );
            s_style_prev = show_style_win;
            if ( show_style_win )
                show_style_editor( &show_style_win );

            static bool s_toolbar_prev = false;
            if ( show_toolbar_win && !s_toolbar_prev )
                gui()->window_set_open( "Toolbar Icons", true );
            s_toolbar_prev = show_toolbar_win;
            if ( show_toolbar_win )
                show_toolbar_demo( &show_toolbar_win );

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

        /* Frame pacing (built-in): spin at 4 ms (~250 Hz) by default; with idle skip on (I) block
           on OS input while the UI is static, 16 ms (~60 Hz) while a widget animation settles. */
        gui()->frame_pace( 4, 16 );
    }
    
    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();  /* also tears down the boot window + context */
    if ( draw_inited ) draw()->shutdown();
    rhi()->shutdown();                               /* no-op if boot never initialized it */
    dev_font_shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
