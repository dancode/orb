/*==============================================================================================

    sandbox/gui/sb_gui_render/sb_gui_render.c -- render feature matrix.

    A renderer test bed, not a GUI application: output comes straight from the draw_* primitives
    with no windows, panels or widgets in the way, so what lands on screen is exactly what the
    tessellator and the gui shader produced.  Each page is a categorical sweep -- one primitive
    family crossed with the style/fx state that modifies it -- laid out on a fixed grid so a
    malformed cell is visible by comparison with its neighbours.

    This is the browsable catalogue of everything the 2D render system can emit: a caller
    composing a custom widget or a bespoke bit of chrome scans a page, finds the shape that
    fits, and reads its cell for the call that made it.  Every draw_* verb on gui_api.h's custom
    draw / shape-catalog surface has a cell somewhere in here; the exceptions (draw_texture_in,
    node-graph internals, the full baked-shape effect matrix) either need an app-supplied asset
    this sandbox has none of, or already have their own exhaustive demo in sb_gui_sdf.

    Number keys 1-9 then 0 jump straight to one of the first ten pages; left/right arrow steps to
    the previous/next page and wraps, so the catalogue is not capped at ten pages.  The page list
    is the extension point.

==============================================================================================*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

/*==============================================================================================
    Shared ink
==============================================================================================*/

#define INK       GUI_COLOR( 0xE8, 0xE0, 0xD0, 0xFF )
#define INK_DIM   GUI_COLOR( 0x8A, 0x88, 0x80, 0xFF )
#define AMBER     GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF )
#define TEAL      GUI_COLOR( 0x20, 0xC0, 0xB0, 0xFF )
#define PLUM      GUI_COLOR( 0xB0, 0x60, 0xE0, 0xFF )
#define ROSE      GUI_COLOR( 0xE0, 0x50, 0x70, 0xFF )
#define INK_FAINT GUI_COLOR( 0x30, 0x30, 0x38, 0xFF )

/* Component-wise lerp between two packed ABGR colours -- used to shade the draw_rects() batch
   row so each bar in the single call reads as a distinct sample rather than a flat block. */
static u32
lerp_col( u32 a, u32 b, f32 t )
{
    u8 ar = ( u8 )( a           & 0xFF ), ag = ( u8 )( ( a >> 8  ) & 0xFF );
    u8 ab = ( u8 )( ( a >> 16 ) & 0xFF ), aa = ( u8 )( ( a >> 24 ) & 0xFF );
    u8 br = ( u8 )( b           & 0xFF ), bg = ( u8 )( ( b >> 8  ) & 0xFF );
    u8 bb = ( u8 )( ( b >> 16 ) & 0xFF ), ba = ( u8 )( ( b >> 24 ) & 0xFF );

    u8 r  = ( u8 )( ar + ( f32 )( br - ar ) * t );
    u8 g  = ( u8 )( ag + ( f32 )( bg - ag ) * t );
    u8 bl = ( u8 )( ab + ( f32 )( bb - ab ) * t );
    u8 al = ( u8 )( aa + ( f32 )( ba - aa ) * t );
    return GUI_COLOR( r, g, bl, al );
}

/*==============================================================================================
    Grid -- every page walks the same cell layout, so cells line up across pages and a
    misbehaving one stands out against its row.
==============================================================================================*/

#define GRID_COLS    6       // cells per row -- every page's cell()/row_wide() calls share this
#define GRID_X      60.0f    // left margin of the first column
#define GRID_Y      60.0f    // top of the first row (below the hint line)
#define CELL_W     180.0f    // cell box, label strip included
#define CELL_H     160.0f
#define CELL_GAP    32.0f
#define LABEL_H     30.0f    // strip at the bottom of a cell holding its caption

/* Cell n of a page: its drawing area, with the caption written under it. */
static gui_rect_t
cell( i32 index, i32 columns, const char* caption )
{
    i32 col = index % columns;
    i32 row = index / columns;

    gui_rect_t box = {
        GRID_X + ( f32 )col * ( CELL_W + CELL_GAP ),
        GRID_Y + ( f32 )row * ( CELL_H + CELL_GAP ),
        CELL_W, CELL_H,
    };

    gui_rect_t strip = box;
    strip.y += box.h - LABEL_H;
    strip.h  = LABEL_H;
    gui()->draw_text_in( strip, GUI_ALIGN_CENTER, INK_DIM, caption );

    box.h -= LABEL_H;
    return box;
}

/* A cell spanning `span` grid columns starting at column `col` of row `row` -- for a wide
   demonstration (the draw_rects batch strip) that would otherwise cramp into one 180 px
   column. */
static gui_rect_t
row_wide( i32 row, i32 col, i32 span, const char* caption )
{
    gui_rect_t box = {
        GRID_X + ( f32 )col * ( CELL_W + CELL_GAP ), GRID_Y + ( f32 )row * ( CELL_H + CELL_GAP ),
        ( f32 )span * CELL_W + ( f32 )( span - 1 ) * CELL_GAP, CELL_H,
    };

    gui_rect_t strip = box;
    strip.y += box.h - LABEL_H;
    strip.h  = LABEL_H;
    gui()->draw_text_in( strip, GUI_ALIGN_CENTER, INK_DIM, caption );

    box.h -= LABEL_H;
    return box;
}

#define TWEAK_PANEL_W       0.0f
#define TWEAK_PANEL_COLW    320.0f

/* Column of tweak controls beside grid row `row`, top-aligned with that row's cells instead of
   stacking every page's controls into one tall column down the side of the whole grid.  `id_str`
   must be unique per call within a frame (region ids don't fold in push_id) -- "<page>_rowN" is
   enough.  About four rows of one row2() pair (label+slider, then a lone control) fit before the
   region runs past CELL_H; a caller with more than that should open a second row region rather
   than let this one overflow into the row below. */
static void
panel_row_begin( i32 row, const char* id_str )
{
    f32 x = GRID_X + ( f32 )GRID_COLS * ( CELL_W + CELL_GAP );
    f32 y = GRID_Y + ( f32 )row * ( CELL_H + CELL_GAP );
    gui()->region_begin( id_str, x, y, TWEAK_PANEL_W, CELL_H, GUI_REGION_FG,
                         GUI_VP_MAIN, GUI_WIN_NOSCROLL | GUI_WIN_NO_CLIP );
    gui()->row2( TWEAK_PANEL_COLW, 0 );
    gui()->field_label_left( 80 );
}

static void
panel_row_end( void )
{
    gui()->region_end();
}

/* Cell centre -- the radial verbs (circle, ngon, star, arc family) take a centre + radius
   rather than a box. */
static gui_vec2_t
cell_center( gui_rect_t r )
{
    return ( gui_vec2_t ){ r.x + r.w * 0.5f, r.y + r.h * 0.5f };
}

static f32
cell_radius( gui_rect_t r )
{
    return ( r.h < r.w ? r.h : r.w ) * 0.5f - 4.0f;
}

/*==============================================================================================
    Assets -- a tiny procedural sprite and baked shape, registered once at boot, so the icons
    page can show draw_sprite_in / draw_brush / draw_shape_in without depending on any file on
    disk.  The engine's own built-in icon set (save/folder/file/...) loads automatically at gui
    boot and needs no registration here.
==============================================================================================*/

#define SPRITE_SRC 16
#define SHAPE_SRC  64

static gui_sprite_id_t s_sprite_swatch = GUI_SPRITE_NONE;
static gui_shape_id_t  s_shape_diamond = GUI_SHAPE_NONE;

static void
load_catalogue_assets( void )
{
    /* A 16x16 two-tone checker, straight-alpha RGBA8, so draw_sprite_in / GUI_BRUSH_SPRITE /
       GUI_BRUSH_NINE all have real art to stretch or slice. */
    static u8 sprite_rgba[ SPRITE_SRC * SPRITE_SRC * 4 ];
    for ( u32 y = 0; y < SPRITE_SRC; ++y )
        for ( u32 x = 0; x < SPRITE_SRC; ++x )
        {
            bool light = ( ( x / 4u ) + ( y / 4u ) ) % 2u == 0u;
            u8*  px    = &sprite_rgba[ ( y * SPRITE_SRC + x ) * 4u ];
            px[ 0 ]    = light ? 0xFF : 0x20;
            px[ 1 ]    = light ? 0xA0 : 0xC0;
            px[ 2 ]    = light ? 0x20 : 0xB0;
            px[ 3 ]    = 0xFF;
        }
    s_sprite_swatch = gui()->register_sprite( "sb_render_swatch", SPRITE_SRC, SPRITE_SRC, sprite_rgba );
    gui()->sprite_set_slice( s_sprite_swatch, ( gui_pad_t ){ 4, 4, 4, 4 } );

    /* A plain diamond silhouette: enough to show draw_shape_in and the ambient draw_set_shape
       hookup (draw_glow / draw_ring wearing a non-rectangular field).  The full baked-shape
       effect matrix (borders, insets, pulses, cuts) is sb_gui_sdf's job, not this catalogue's. */
    static u8 shape_cov[ SHAPE_SRC * SHAPE_SRC ];
    for ( u32 y = 0; y < SHAPE_SRC; ++y )
        for ( u32 x = 0; x < SHAPE_SRC; ++x )
        {
            f32 u = ( ( f32 )x + 0.5f ) / ( f32 )SHAPE_SRC - 0.5f;
            f32 v = ( ( f32 )y + 0.5f ) / ( f32 )SHAPE_SRC - 0.5f;
            shape_cov[ y * SHAPE_SRC + x ] = ( fabsf( u ) + fabsf( v ) <= 0.38f ) ? 255u : 0u;
        }
    s_shape_diamond = gui()->register_shape( "sb_render_diamond", SHAPE_SRC, SHAPE_SRC, shape_cov, NULL );
}

/*==============================================================================================
    Live tweak panel -- a fixed-position region (no window chrome, no drag, no dock) pinned at a
    caller-chosen screen rect, to see how a HUD-style control reads next to the catalogue grid.
    build_frame opens it once per frame around the active page's build() call, so any slider,
    checkbox, etc. a page emits lands in this region's stack and appends as a new row -- the
    page owns what it puts there, the panel just owns the box and the vertical flow.
==============================================================================================*/

#define TWEAK_EASE_TIME     0.50f
#define TWEAK_EASE_FUNC     GUI_EASE_OUT_CUBIC
#define TWEAK_ROUND_WIDTH   16.0f
#define TWEAK_TILE_MAX      120

static i32 s_tweak_tile_count = TWEAK_TILE_MAX;

/*==============================================================================================
    Page 1 -- fills: the fast path a plain draw_rect takes, the general SDF box catalogue built
    on top of it, and the batched form for drawing many rects in one command.
==============================================================================================*/

static void
page_fills( void )
{
    gui_rect_t r;

    //------------------------------------------------------------------------------------------
    // row 0 -- the fast path: draw_rect emits ONE quad with no SDF field at all
    //------------------------------------------------------------------------------------------

    panel_row_begin( 0, "fills_row0" );
        
    r = cell( 0, GRID_COLS, "draw_rect (fast path)" );    

    /* slider -- width and height */
    static float rect_w = CELL_W;
    static float rect_h = CELL_H - LABEL_H;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "width", &rect_w, 0, CELL_W, 1.0f );
    if ( gui()->button( "reset##1" )) { rect_w = r.w; }
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "height", &rect_h, 0, CELL_H - LABEL_H, 1.0f );
    if ( gui()->button( "reset##2" )) { rect_h = r.h; }

    /* draw -- (fast path) rect */

    gui()->draw_rect( r.x, r.y, rect_w, rect_h, AMBER );

    /* slider -- draw count */
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_int( "count", &s_tweak_tile_count, 1, TWEAK_TILE_MAX, NULL );
    if ( gui()->button( "reset##3" )) { s_tweak_tile_count = TWEAK_TILE_MAX; }    

    /* draw -- (batched fast path) draw_rects() emit N of them in ONE command */
    r = row_wide( 0, 1, GRID_COLS - 1, "draw_rects() -- checker / 1 call (tweak panel sets count)" );
    {
        enum { CHECK_COLS = 12, CHECK_ROWS_MAX = TWEAK_TILE_MAX / CHECK_COLS };
        gui_rect_col_t tiles[ TWEAK_TILE_MAX ];
        f32 tw = r.w / ( f32 )CHECK_COLS, th = r.h / ( f32 )CHECK_ROWS_MAX;
        for ( i32 i = 0; i < s_tweak_tile_count; ++i )
        {
            i32   x = i % CHECK_COLS, y = i / CHECK_COLS;
            bool  light = ( x + y ) % 2 == 0;
            tiles[ i ] = ( gui_rect_col_t ) {
                r.x + ( f32 )x * tw + 1.0f, r.y + ( f32 )y * th + 1.0f, 
                tw - 2.0f, th - 2.0f, light ? AMBER : TEAL,
            };
        }
        gui()->draw_rects( tiles, s_tweak_tile_count );
    }
    
    panel_row_end();

    //------------------------------------------------------------------------------------------
    // rows 1 - basic shape rects -- the general box catalogue: rounding, border, gradient and 
    // circle all resolve through the same SDF quad, just with more of the record populated.
    //------------------------------------------------------------------------------------------

    panel_row_begin( 1, "fills_row1" );

    /* rect rounding */
    static float round_w = TWEAK_ROUND_WIDTH;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "round", &round_w, 0, CELL_H * 0.5, 1.0f );
    if ( gui()->button( "reset##5" )) { round_w =TWEAK_ROUND_WIDTH; }
    
    /* rect border */
    static float border = 2.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "border", &border, 1.0f, 16.0f, 1.0f );
    if ( gui()->button( "reset##6" )) { border = 2.0f; }

    /* rect border alignment */
    static float border_align = 0.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "align", &border_align, 0, 1.0f, 0.1f );
    if ( gui()->button( "reset##7" )) { border_align = 0.0f; }
    gui()->draw_set_border_align( border_align );

    /* circle radius */
    static float radius = 64.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "radius", &radius, 0, cell_radius( r ), 1.0f );
    if ( gui()->button( "reset##8" )) { radius = cell_radius( r ); }

    r = cell( 6, GRID_COLS, "round rect" );
    gui()->draw_round_rect( r, round_w, round_w, round_w, round_w, 0.0f, TEAL );

    r = cell( 7, GRID_COLS, "round rect (border)" );
    gui()->draw_round_rect( r, round_w, round_w, round_w, round_w, border, TEAL );
    
    r = cell( 8, GRID_COLS, "round rect (circle)" );
    { gui_vec2_t c = cell_center( r ); gui()->draw_circle( c.x, c.y, radius, 0.0f, TEAL ); }
    
    r = cell( 9, GRID_COLS, "round rect (border)" );
    { gui_vec2_t c = cell_center( r ); gui()->draw_circle( c.x, c.y, radius, border, TEAL ); }

    gui()->draw_set_border_align( 0.0f );
    panel_row_end();

    //------------------------------------------------------------------------------------------
    // row 2 -- gradients: the linear/radial/conic blend the fill quad can carry.
    //------------------------------------------------------------------------------------------

    panel_row_begin( 2, "fills_row2" );

    static float grad_round = 14.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "round", &grad_round, 0, CELL_H * 0.5f, 1.0f );
    if ( gui()->button( "reset##9" )) { grad_round = 14.0f; }

    /* draw_round_rect_gradient's mid overloads 0 as its "unset, use linear" sentinel and clamps
       out anything >= 0.999 for the same log()-domain reason -- 0.02..0.98 keeps the slider's
       whole span inside the range that actually bends the ramp, instead of spending both ends on
       a dead zone that reads identically to 0.5. */
    static float grad_mix = 0.5f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "mix", &grad_mix, 0.02f, 0.98f, 0.02f );
    if ( gui()->button( "reset##10" )) { grad_mix = 0.5f; }

    static float grad_angle_deg = 45.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "angle", &grad_angle_deg, 0.0f, 360.0f, 1.0f );
    if ( gui()->button( "reset##11" )) { grad_angle_deg = 45.0f; }

    r = cell( 12, GRID_COLS, "gradient v" );
    gui()->draw_gradient( r, AMBER, PLUM, true );

    r = cell( 13, GRID_COLS, "gradient h" );
    gui()->draw_gradient( r, AMBER, PLUM, false );

    r = cell( 14, GRID_COLS, "rr gradient (linear)" );
    gui()->draw_round_rect_gradient( r, grad_round, AMBER, TEAL, GUI_GRAD_LINEAR, 0.0f, grad_mix );

    r = cell( 15, GRID_COLS, "rr gradient (radial)" );
    gui()->draw_round_rect_gradient( r, grad_round, AMBER, TEAL, GUI_GRAD_RADIAL, 0.0f, grad_mix );

    r = cell( 16, GRID_COLS, "rr gradient (conic)" );
    gui()->draw_round_rect_gradient( r, grad_round, AMBER, TEAL, GUI_GRAD_CONIC, gui_radians( grad_angle_deg ), grad_mix );

    panel_row_end();

    //------------------------------------------------------------------------------------------
    // row 3 -- frame: a single quad compositing an inner fill with an outer stroke.
    //------------------------------------------------------------------------------------------

    panel_row_begin( 3, "fills_row3" );

    static float frame_round = 16.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "round", &frame_round, 0, CELL_H * 0.5f, 1.0f );
    if ( gui()->button( "reset##12" )) { frame_round = 16.0f; }

    static float frame_border = 2.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "border", &frame_border, 1.0f, 16.0f, 1.0f );
    if ( gui()->button( "reset##13" )) { frame_border = 2.0f; }

    static float frame_border_align = 0.0f;
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "align", &frame_border_align, 0, 1.0f, 0.1f );
    if ( gui()->button( "reset##14" )) { frame_border_align = 0.0f; }
    
    panel_row_end();

    //------------------------------------------------------------------------------------------
    // draw_round_frame is draw_round_rect's dual-color sibling: rounding is a parameter, not the
    // ambient, so this cell -- a plain userspace draw, not a widget paint -- never touches
    // draw_set_rounding at all.
    //------------------------------------------------------------------------------------------

    panel_row_begin( 4, "fills_row4" );

    r = cell( 18, GRID_COLS, "frame (bg + border)" );

    gui()->draw_set_border_align( frame_border_align );
    gui()->draw_round_frame( r, frame_round, INK_FAINT, TEAL, frame_border );
    gui()->draw_set_border_align( 0.0f );

    /* draw_round_rect takes its four corner radii independently -- this cell is the one place in
       the page that exercises that, instead of the uniform round_w every other rounded cell uses. */
    static float corner_tl = TWEAK_ROUND_WIDTH, corner_tr = TWEAK_ROUND_WIDTH, 
                 corner_br = TWEAK_ROUND_WIDTH, corner_bl = TWEAK_ROUND_WIDTH;

    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "tl", &corner_tl, 0, CELL_H * 0.5f, 1.0f );
    if ( gui()->button( "reset##15" )) { corner_tl = TWEAK_ROUND_WIDTH; }
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "tr", &corner_tr, 0, CELL_H * 0.5f, 1.0f );
    if ( gui()->button( "reset##16" )) { corner_tr = TWEAK_ROUND_WIDTH; }
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "br", &corner_br, 0, CELL_H * 0.5f, 1.0f );
    if ( gui()->button( "reset##17" )) { corner_br = TWEAK_ROUND_WIDTH; }
    gui()->next_slider_animate( TWEAK_EASE_FUNC, TWEAK_EASE_TIME );
    gui()->slider_float_step( "bl", &corner_bl, 0, CELL_H * 0.5f, 1.0f );
    if ( gui()->button( "reset##18" )) { corner_bl = TWEAK_ROUND_WIDTH; }

    r = cell( 24, GRID_COLS, "round rect (per-corner)" );
    gui()->draw_round_rect( r, corner_tl, corner_tr, corner_br, corner_bl, 0.0f, TEAL );

    panel_row_end();
}

/*==============================================================================================
    Page 2 -- symbols: the widget glyph marks (menu ticks, tree arrows, close crosses, grips).
==============================================================================================*/

static void
page_symbols( void )
{
    panel_row_begin( 0, "symbols_row1" );
    gui()->row2( TWEAK_PANEL_COLW, 0 );
    gui()->field_label_left( 96 );
    
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "check_mark" );

    static float check_size = 0.0f;
    gui()->slider_float_step( "rect size", &check_size, 0, r.w, 1.0f );    
    if ( gui()->button( "reset" )) { check_size = r.w; }
    r.w = check_size;

    panel_row_end();

    gui()->draw_check_mark( r, INK );

    r = cell( 1, GRID_COLS, "arrow (up)" );
    gui()->draw_arrow( r, GUI_DIR_UP, INK );

    r = cell( 2, GRID_COLS, "arrow (down)" );
    gui()->draw_arrow( r, GUI_DIR_DOWN, INK );

    r = cell( 3, GRID_COLS, "arrow (left)" );
    gui()->draw_arrow( r, GUI_DIR_LEFT, INK );

    r = cell( 4, GRID_COLS, "arrow (right)" );
    gui()->draw_arrow( r, GUI_DIR_RIGHT, INK );

    r = cell( 5, GRID_COLS, "arrow_pointing_at" );
    { gui_vec2_t c = cell_center( r ); gui()->draw_arrow_pointing_at( c.x, c.y, r.h * 0.3f, GUI_DIR_DOWN, AMBER ); }

    r = cell( 6, GRID_COLS, "chevron (up)" );
    gui()->draw_chevron( r, GUI_DIR_UP, 3.0f, INK );

    r = cell( 7, GRID_COLS, "chevron (down)" );
    gui()->draw_chevron( r, GUI_DIR_DOWN, 3.0f, INK );

    r = cell( 8, GRID_COLS, "plus_minus (+)" );
    gui()->draw_plus_minus( r, true, 3.0f, INK );

    r = cell( 9, GRID_COLS, "plus_minus (-)" );
    gui()->draw_plus_minus( r, false, 3.0f, INK );

    r = cell( 10, GRID_COLS, "close" );
    gui()->draw_close( r, ROSE );

    r = cell( 11, GRID_COLS, "bullet" );
    { gui_vec2_t c = cell_center( r ); gui()->draw_bullet( c.x, c.y, r.h * 0.2f, TEAL ); }

    r = cell( 12, GRID_COLS, "grip" );
    gui()->draw_grip( r, INK_DIM );
}

/*==============================================================================================
    Page 3 -- shapes: the box family, radial shapes and circular sectors -- everything the shape
    catalog resolves as a single SDF quad.
==============================================================================================*/

static void
page_shapes( void )
{
    gui_rect_t r; gui_vec2_t c; f32 rad;

    r = cell( 0, GRID_COLS, "round_rect" );
    gui()->draw_round_rect( r, 14.0f, 14.0f, 14.0f, 14.0f, 0.0f, TEAL );

    r = cell( 1, GRID_COLS, "round_rect (mixed)" );
    gui()->draw_round_rect( r, 4.0f, 24.0f, 4.0f, 24.0f, 0.0f, PLUM );

    r = cell( 2, GRID_COLS, "round_rect (stroked)" );
    gui()->draw_round_rect( r, 14.0f, 14.0f, 14.0f, 14.0f, 2.0f, TEAL );

    r = cell( 3, GRID_COLS, "box_xf (rotated)" );
    gui()->draw_box_xf( r, 10.0f, 0.0f, gui_radians( 18.0f ), AMBER );

    r = cell( 4, GRID_COLS, "rect_cut (subtract)" );
    {
        gui_rect_t cut = { r.x + r.w * 0.5f, r.y - 10.0f, r.w * 0.6f, r.h * 0.6f };
        gui()->draw_rect_cut( r, 10.0f, cut, 6.0f, 1.0f, TEAL );
    }

    r = cell( 5, GRID_COLS, "circle" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_circle( c.x, c.y, rad, 0.0f, PLUM );

    r = cell( 6, GRID_COLS, "circle (stroked)" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_circle( c.x, c.y, rad, 3.0f, AMBER );

    r = cell( 7, GRID_COLS, "ngon (hex)" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_ngon( c.x, c.y, rad, 6, 0.0f, 0.0f, TEAL );

    r = cell( 8, GRID_COLS, "ngon (oct, stroked)" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_ngon( c.x, c.y, rad, 8, 0.0f, 3.0f, PLUM );

    r = cell( 9, GRID_COLS, "star" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_star( c.x, c.y, rad, 5, 0.0f, 0.0f, 0.0f, AMBER );

    r = cell( 10, GRID_COLS, "arc" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_arc( c.x, c.y, rad, gui_radians( 0.0f ), gui_radians( 270.0f ), 4.0f, TEAL );

    r = cell( 11, GRID_COLS, "arc_dashed" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_arc_dashed( c.x, c.y, rad, gui_radians( 0.0f ), gui_radians( 300.0f ), 4.0f, 6.0f, 4.0f, PLUM );

    r = cell( 12, GRID_COLS, "arc_gradient" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_arc_gradient( c.x, c.y, rad, gui_radians( -90.0f ), gui_radians( 180.0f ), 5.0f, AMBER, TEAL );

    r = cell( 13, GRID_COLS, "pie" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_pie( c.x, c.y, rad, gui_radians( -40.0f ), gui_radians( 120.0f ), AMBER );

    r = cell( 14, GRID_COLS, "progress_arc" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_progress_arc( c.x, c.y, rad, 0.65f, 4.0f, TEAL );
}

/*==============================================================================================
    Page 4 -- lines, curves and paths: straight / dashed / capsule strokes, the polyline family,
    beziers, and the auto-filleted / spline path forms.
==============================================================================================*/

static void
page_lines( void )
{
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "line" );
    gui()->draw_line( r.x + 10.0f, r.y + 10.0f, r.x + r.w - 10.0f, r.y + r.h - 10.0f, 2.0f, TEAL );

    r = cell( 1, GRID_COLS, "dashed_line" );
    gui()->draw_dashed_line( r.x + 10.0f, r.y + 10.0f, r.x + r.w - 10.0f, r.y + r.h - 10.0f, 6.0f, 4.0f, 2.0f, PLUM );

    r = cell( 2, GRID_COLS, "capsule" );
    gui()->draw_capsule( r.x + 16.0f, r.y + r.h * 0.5f, r.x + r.w - 16.0f, r.y + r.h * 0.5f, 18.0f, AMBER );

    r = cell( 3, GRID_COLS, "capsule_outline" );
    gui()->draw_capsule_outline( r.x + 16.0f, r.y + r.h * 0.5f, r.x + r.w - 16.0f, r.y + r.h * 0.5f, 18.0f, 3.0f, TEAL );

    r = cell( 4, GRID_COLS, "polyline (open)" );
    {
        gui_vec2_t pts[ 4 ] = {
            { r.x + 10.0f, r.y + r.h - 10.0f }, { r.x + r.w * 0.4f, r.y + 10.0f },
            { r.x + r.w * 0.6f, r.y + r.h - 10.0f }, { r.x + r.w - 10.0f, r.y + 10.0f },
        };
        gui()->draw_polyline( pts, 4, 2.0f, GUI_STROKE_CENTER, false, PLUM );
    }

    r = cell( 5, GRID_COLS, "polyline (closed)" );
    {
        gui_vec2_t pts[ 4 ] = {
            { r.x + r.w * 0.5f, r.y + 10.0f }, { r.x + r.w - 10.0f, r.y + r.h * 0.5f },
            { r.x + r.w * 0.5f, r.y + r.h - 10.0f }, { r.x + 10.0f, r.y + r.h * 0.5f },
        };
        gui()->draw_polyline( pts, 4, 2.0f, GUI_STROKE_CENTER, true, AMBER );
    }

    r = cell( 6, GRID_COLS, "bezier_quad" );
    gui()->draw_bezier_quad( r.x + 10.0f, r.y + r.h - 10.0f, r.x + r.w * 0.5f, r.y + 6.0f,
                              r.x + r.w - 10.0f, r.y + r.h - 10.0f, 2.0f, TEAL );

    r = cell( 7, GRID_COLS, "bezier_cubic" );
    gui()->draw_bezier_cubic( r.x + 10.0f, r.y + r.h - 10.0f, r.x + r.w * 0.3f, r.y + 6.0f,
                               r.x + r.w * 0.7f, r.y + r.h - 6.0f, r.x + r.w - 10.0f, r.y + 10.0f, 2.0f, PLUM );

    r = cell( 8, GRID_COLS, "rounded_path" );
    {
        gui_vec2_t pts[ 4 ] = {
            { r.x + 10.0f, r.y + r.h - 10.0f }, { r.x + 10.0f, r.y + 10.0f },
            { r.x + r.w - 10.0f, r.y + 10.0f }, { r.x + r.w - 10.0f, r.y + r.h - 10.0f },
        };
        gui()->draw_rounded_path( pts, 4, 12.0f, 2.0f, false, AMBER );
    }

    r = cell( 9, GRID_COLS, "smooth_path" );
    {
        gui_vec2_t pts[ 5 ] = {
            { r.x + 10.0f, r.y + r.h * 0.5f }, { r.x + r.w * 0.3f, r.y + 10.0f },
            { r.x + r.w * 0.5f, r.y + r.h - 10.0f }, { r.x + r.w * 0.7f, r.y + 10.0f },
            { r.x + r.w - 10.0f, r.y + r.h * 0.5f },
        };
        gui()->draw_smooth_path( pts, 5, 2.0f, false, TEAL );
    }

    r = cell( 10, GRID_COLS, "wire" );
    gui()->draw_wire( r.x + 10.0f, r.y + r.h * 0.25f, r.x + r.w - 10.0f, r.y + r.h * 0.75f, 20.0f, 80.0f, 2.0f, PLUM );

    /* path_line_to / path_stroke -- the retained sibling of draw_polyline: build the point run
       across several calls, then stroke it in one shot.  Same tessellation, different calling
       convention -- useful when the points come from a loop rather than a fixed array. */
    r = cell( 11, GRID_COLS, "path_line_to / path_stroke" );
    gui()->path_line_to( r.x + 10.0f, r.y + r.h * 0.5f );
    gui()->path_line_to( r.x + r.w * 0.3f, r.y + 10.0f );
    gui()->path_line_to( r.x + r.w * 0.5f, r.y + r.h - 10.0f );
    gui()->path_line_to( r.x + r.w * 0.7f, r.y + 10.0f );
    gui()->path_line_to( r.x + r.w - 10.0f, r.y + r.h * 0.5f );
    gui()->path_stroke( 2.0f, GUI_STROKE_CENTER, false, TEAL );
}

/*==============================================================================================
    Page 5 -- patterns and gradients: what a shape is filled WITH.
==============================================================================================*/

static void
page_patterns( void )
{
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "checker" );
    gui()->draw_checker( r, 12.0f, GUI_COLOR( 0x40, 0x40, 0x48, 0xFF ), GUI_COLOR( 0x24, 0x24, 0x2A, 0xFF ) );

    r = cell( 1, GRID_COLS, "grid" );
    gui()->draw_grid( r, 16.0f, 1.0f, r.x, r.y, INK_DIM );

    r = cell( 2, GRID_COLS, "hatch" );
    gui()->draw_hatch( r, 10.0f, 2.0f, TEAL );

    r = cell( 3, GRID_COLS, "stripes" );
    gui()->draw_stripes( r, 10.0f, 3.0f, gui_radians( 45.0f ), PLUM );

    r = cell( 4, GRID_COLS, "round_rect_gradient (linear)" );
    gui()->draw_round_rect_gradient( r, 12.0f, AMBER, TEAL, GUI_GRAD_LINEAR, gui_radians( 90.0f ), 0.5f );

    r = cell( 5, GRID_COLS, "round_rect_gradient (radial)" );
    gui()->draw_round_rect_gradient( r, 12.0f, AMBER, TEAL, GUI_GRAD_RADIAL, 0.0f, 0.5f );

    r = cell( 6, GRID_COLS, "round_rect_gradient (conic)" );
    gui()->draw_round_rect_gradient( r, 12.0f, AMBER, TEAL, GUI_GRAD_CONIC, gui_radians( 0.0f ), 0.5f );
}

/*==============================================================================================
    Page 6 -- light and shadow: the soft single-quad surfaces, each paired here with the caster
    it would sit behind or on top of in real chrome.
==============================================================================================*/

static void
page_shadow( void )
{
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "shadow" );
    gui()->draw_shadow( r, 16.0f, GUI_COLOR( 0x00, 0x00, 0x00, 0x90 ) );

    r = cell( 1, GRID_COLS, "drop_shadow" );
    gui()->draw_drop_shadow( r, 14.0f, 4.0f, 6.0f, GUI_COLOR( 0x00, 0x00, 0x00, 0xA0 ) );
    gui()->draw_round_rect( r, 8.0f, 8.0f, 8.0f, 8.0f, 0.0f, PLUM );

    r = cell( 2, GRID_COLS, "inset_shadow" );
    gui()->draw_round_rect( r, 8.0f, 8.0f, 8.0f, 8.0f, 0.0f, INK_FAINT );
    gui()->draw_inset_shadow( r, 12.0f, GUI_COLOR( 0x00, 0x00, 0x00, 0x90 ) );

    r = cell( 3, GRID_COLS, "edge_shadow" );
    gui()->draw_round_rect( r, 4.0f, 4.0f, 4.0f, 4.0f, 0.0f, INK_FAINT );
    gui()->draw_edge_shadow( r, GUI_EDGE_BOTTOM, r.h * 0.6f, GUI_COLOR( 0x00, 0x00, 0x00, 0xA0 ) );

    r = cell( 4, GRID_COLS, "glow (drawn under)" );
    gui()->draw_glow( r, 18.0f, AMBER );
    gui()->draw_round_rect( r, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f, INK );

    r = cell( 5, GRID_COLS, "round_rect_shadow" );
    gui()->draw_round_rect_shadow( r, 4.0f, 24.0f, 4.0f, 24.0f, 10.0f, GUI_COLOR( 0x00, 0x00, 0x00, 0x90 ) );
    gui()->draw_round_rect( r, 4.0f, 24.0f, 4.0f, 24.0f, 0.0f, TEAL );
}

/*==============================================================================================
    Page 7 -- sets and meters: repeated copies of one cell, folded in the fragment so N copies
    still cost one quad.
==============================================================================================*/

static void
page_sets( void )
{
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "dot_grid" );
    gui()->draw_dot_grid( r, 6, 4, 24.0f, 24.0f, 7.0f, TEAL );

    r = cell( 1, GRID_COLS, "ticks (horizontal)" );
    gui()->draw_ticks( r, 12, 2.0f, r.h * 0.6f, false, AMBER );

    r = cell( 2, GRID_COLS, "ticks (vertical)" );
    gui()->draw_ticks( r, 8, 2.0f, r.w * 0.6f, true, PLUM );

    r = cell( 3, GRID_COLS, "dial_ticks" );
    gui()->draw_dial_ticks( r, 12, 2.0f, 10.0f, 0.0f, TEAL );

    r = cell( 4, GRID_COLS, "meter" );
    gui()->draw_meter( r, 10, 0.7f, AMBER, GUI_COLOR( 0x40, 0x40, 0x48, 0xFF ) );
}

/*==============================================================================================
    Page 8 -- clock-driven motion: everything here animates in the fragment on the shared shader
    clock, so request_redraw() is called every frame this page is up (the clock advancing does
    not by itself schedule one).
==============================================================================================*/

static void
page_motion( void )
{
    gui()->request_redraw();

    gui_rect_t r; gui_vec2_t c; f32 rad;

    r = cell( 0, GRID_COLS, "pulse" );
    gui()->draw_pulse( r, 1.0f, 0.6f, 0.0f, AMBER );

    r = cell( 1, GRID_COLS, "swell" );
    gui()->draw_swell( r, 1.0f, 10.0f, 0.0f, TEAL );

    r = cell( 2, GRID_COLS, "ripple" );
    c = cell_center( r ); rad = cell_radius( r );
    gui()->draw_ripple( c.x, c.y, rad * 0.4f, 3.0f, 24.0f, 1.0f, 0.0f, PLUM );

    r = cell( 3, GRID_COLS, "ring" );
    gui()->draw_ring( r, 6.0f, AMBER );

    r = cell( 4, GRID_COLS, "round_rect_dashed (ants)" );
    gui()->draw_round_rect_dashed( r, 10.0f, 2.0f, 8.0f, 6.0f, 40.0f, TEAL );

    r = cell( 5, GRID_COLS, "border_tracer" );
    gui()->draw_border_tracer( r, 10.0f, 3.0f, 0.25f, 0.6f, PLUM );

    r = cell( 6, GRID_COLS, "border_progress" );
    gui()->draw_border_progress( r, 10.0f, 3.0f, 1.0f, 0.4f, AMBER );

    r = cell( 7, GRID_COLS, "spinner" );
    gui()->draw_spinner( r, 1.2f, 3.0f, TEAL );

    r = cell( 8, GRID_COLS, "dot_spinner (tail)" );
    gui()->draw_dot_spinner( r, 10, 4.0f, 1.0f, PLUM, GUI_COLOR( 0xB0, 0x60, 0xE0, 0x00 ) );
}

/*==============================================================================================
    Page 9 -- text effects: alignment, clipping, the transformed run, and the outline / shadow /
    ambient-edge forms.  SDF-font-only effects still no-op safely on a coverage font.
==============================================================================================*/

static void
page_text( void )
{
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "text_in (left)" );
    gui()->draw_text_in( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, INK, "left" );

    r = cell( 1, GRID_COLS, "text_in (center)" );
    gui()->draw_text_in( r, GUI_ALIGN_CENTER, INK, "center" );

    r = cell( 2, GRID_COLS, "text_in (right)" );
    gui()->draw_text_in( r, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, INK, "right" );

    r = cell( 3, GRID_COLS, "text_clipped" );
    gui()->draw_text_clipped( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, INK, "a caption too long for its cell" );

    r = cell( 4, GRID_COLS, "text_xf (scaled/rot)" );
    { gui_vec2_t c = cell_center( r ); gui()->draw_text_xf( c.x - 24.0f, c.y, AMBER, "spin", 1.4f, gui_radians( 20.0f ) ); }

    r = cell( 5, GRID_COLS, "text_outline" );
    gui()->draw_text_outline( r.x + 8.0f, r.y + r.h * 0.5f, "outline", INK, GUI_COLOR( 0x00, 0x00, 0x00, 0xFF ) );

    r = cell( 6, GRID_COLS, "text_shadow" );
    gui()->draw_text_shadow( r.x + 8.0f, r.y + r.h * 0.5f, "shadow", INK, GUI_COLOR( 0x00, 0x00, 0x00, 0xA0 ), 2.0f, 2.0f );

    r = cell( 7, GRID_COLS, "ambient text_edge" );
    {
        f32 save_w; u32 save_col;
        gui()->draw_get_text_edge( &save_w, &save_col );
        gui()->draw_set_text_edge( 2.0f, GUI_COLOR( 0x00, 0x00, 0x00, 0xFF ) );
        gui()->draw_text( r.x + 8.0f, r.y + r.h * 0.5f, AMBER, "edge" );
        gui()->draw_set_text_edge( save_w, save_col );
    }
}

/*==============================================================================================
    Page 10 -- icons, sprites and baked shapes: the atlas-backed art kinds, all of which batch
    into the same draw call as text and fills.  The built-in icon set (save/folder/file/gear/
    grid/wire/view) loads from PNGs under assets/icons at gui boot; a blank cell here means that
    file is missing on disk, not that the draw call failed -- find_icon returns GUI_ICON_NONE and
    the cell is skipped rather than drawing a broken quad.
==============================================================================================*/

static void
page_icons( void )
{
    gui_rect_t r;

    static const char* s_icon_names[ 7 ] = { "save", "folder", "file", "gear", "grid", "wire", "view" };
    for ( i32 i = 0; i < 7; ++i )
    {
        r = cell( i, 6, s_icon_names[ i ] );
        gui_icon_id_t id = gui()->find_icon( s_icon_names[ i ] );
        if ( id != GUI_ICON_NONE ) gui()->draw_icon_in( r, id, 0u );
    }

    r = cell( 7, GRID_COLS, "draw_icon_xf (rotated)" );
    {
        gui_icon_id_t id = gui()->find_icon( "gear" );
        if ( id != GUI_ICON_NONE ) gui()->draw_icon_xf( r, id, AMBER, gui_radians( 25.0f ) );
    }

    r = cell( 8, GRID_COLS, "draw_sprite_in" );
    if ( s_sprite_swatch != GUI_SPRITE_NONE ) gui()->draw_sprite_in( r, s_sprite_swatch, 0u );

    r = cell( 9, GRID_COLS, "draw_shape_in (baked)" );
    if ( s_shape_diamond != GUI_SHAPE_NONE ) gui()->draw_shape_in( r, s_shape_diamond, TEAL );

    r = cell( 10, GRID_COLS, "shape + ambient fx" );
    if ( s_shape_diamond != GUI_SHAPE_NONE )
    {
        gui()->draw_set_shape( s_shape_diamond );
        gui()->draw_glow( r, 14.0f, AMBER );
        gui()->draw_ring( r, 3.0f, INK );
        gui()->draw_set_shape( GUI_SHAPE_NONE );
    }
}

/*==============================================================================================
    Page 11 -- brushes: the fill-kind indirection behind draw_brush -- solid, gradient, sprite
    and nine-slice, plus the flip/tile flags each of the latter two reads.
==============================================================================================*/

static void
page_brushes( void )
{
    gui_rect_t r;

    r = cell( 0, GRID_COLS, "SOLID" );
    gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_SOLID, .col_a = TEAL } );

    r = cell( 1, GRID_COLS, "GRADIENT (vertical)" );
    gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_GRADIENT, .col_a = AMBER, .col_b = PLUM,
                                            .flags = GUI_BRUSH_VERTICAL } );

    r = cell( 2, GRID_COLS, "GRADIENT (horizontal)" );
    gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_GRADIENT, .col_a = AMBER, .col_b = PLUM } );

    r = cell( 3, GRID_COLS, "SPRITE" );
    if ( s_sprite_swatch != GUI_SPRITE_NONE )
        gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_SPRITE, .sprite = s_sprite_swatch, .col_a = INK } );

    r = cell( 4, GRID_COLS, "SPRITE (flip x/y)" );
    if ( s_sprite_swatch != GUI_SPRITE_NONE )
        gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_SPRITE, .sprite = s_sprite_swatch, .col_a = INK,
                                                .flags = GUI_BRUSH_FLIP_X | GUI_BRUSH_FLIP_Y } );

    r = cell( 5, GRID_COLS, "NINE" );
    if ( s_sprite_swatch != GUI_SPRITE_NONE )
        gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_sprite_swatch } );

    r = cell( 6, GRID_COLS, "NINE (tile)" );
    if ( s_sprite_swatch != GUI_SPRITE_NONE )
        gui()->draw_brush( r, &( gui_brush_t ){ .kind = GUI_BRUSH_NINE, .sprite = s_sprite_swatch,
                                                .flags = GUI_BRUSH_TILE } );
}

/*==============================================================================================
    Pages
==============================================================================================*/

typedef struct
{
    const char* name;          // shown in the hint line
    void ( *build )( void );   // draws the page's cells

} page_t;

static const page_t s_pages[] = {
    { "fills",             page_fills    },
    { "symbols",           page_symbols  },
    { "shapes",            page_shapes   },
    { "lines + paths",     page_lines    },
    { "patterns + grads",  page_patterns },
    { "light + shadow",    page_shadow   },
    { "sets + meters",     page_sets     },
    { "motion",            page_motion   },
    { "text",              page_text     },
    { "icons + sprites",   page_icons    },
    { "brushes",           page_brushes  },
};

#define PAGE_COUNT ( ( i32 )( sizeof s_pages / sizeof s_pages[ 0 ] ) )

static i32 s_page = 0;

static void
build_frame( void )
{
    /* page switching: number keys 1-9 then 0 jump to one of the first ten pages directly; left/
       right arrows step and wrap, reaching pages beyond the number-key range.  Fenced so a page
       that ever takes text input keeps its keys. */

    if ( !gui()->want_capture_keyboard() )
    {
        for ( i32 k = 0; k < PAGE_COUNT && k < 10; ++k )
        {
            app_key_t key = ( k < 9 ) ? ( app_key_t )( APP_KEY_1 + k ) : APP_KEY_0;
            if ( gui()->is_key_pressed( key ) )
                s_page = k;
        }

        if ( gui()->is_key_pressed( APP_KEY_RIGHT ) ) s_page = ( s_page + 1 ) % PAGE_COUNT;
        if ( gui()->is_key_pressed( APP_KEY_LEFT  ) ) s_page = ( s_page - 1 + PAGE_COUNT ) % PAGE_COUNT;
    }

    char hint[ 128 ];
    snprintf( hint, sizeof hint, "sb_gui_render -- page %d/%d: %s (arrows or 1-9,0)",
              s_page + 1, PAGE_COUNT, s_pages[ s_page ].name );
    gui()->draw_text( 12.0f, 8.0f, INK_DIM, hint );

    /* Each page opens its own row-aligned tweak strips (panel_row_begin/end) around whatever
       cell rows need live controls -- there is no page-spanning panel here anymore. */
    s_pages[ s_page ].build();
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_render] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title      = "ORB -- gui render matrix",
        .w          = 1920, .h = 1024,
        .os_chrome  = true,
        .font       = GUI_FONT_CASCADIA_MONO,
        .clock      = sys_tick_seconds,
        .sleep      = sys_sleep_milliseconds,
        .wait       = sys_wait_for_os_events_ms,
        .clear      = { 0.05f, 0.05f, 0.05f, 1.00f },
        .debug       = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_render] gui->boot failed\n" );
        goto shutdown;
    }

    load_catalogue_assets();

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame();
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        gui()->boot_pace( 4, 16 ); // 4, 16
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
