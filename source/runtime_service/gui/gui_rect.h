#ifndef GUI_RECT_H
#define GUI_RECT_H
/*==============================================================================================

    runtime_service/gui/gui_rect.h -- GUI_RECT: the leaf rect kit.

    Pure stateless carve math over the geometry types -- no pen, no region state, no draw,
    no gui context.  Depends on nothing but base types, so it is usable anywhere, even
    outside gui.  Layout of any kind is a rect PRODUCER and a widget is a rect CONSUMER;
    these are the verbs both sides share.  The vtable half of this library (content_rect,
    split, carve, anchor) lives in the GUI_RECT section of gui_api.h.

==============================================================================================*/

#include "orb.h"

// clang-format off
/*==============================================================================================
    Geometry
==============================================================================================*/

/* standard math vectors */
typedef struct { f32 x, y; }        gui_vec2_t;
typedef struct { f32 x, y, w, h; }  gui_rect_t;

/* Edge insets, in pixels. Region padding -- the gap between a region's box and where its layout
   starts (REGION_PAD_DEFAULT).  Breathing room *inside* a widget's frame is a per-widget style concern
   (WIDGET_PAD), not a layout one; spacing *between* cells is gap_x / gap_y. */
typedef struct { f32 l, r, t, b; }  gui_pad_t;

/*==============================================================================================
    Rect Algebra

    Rect algebra -- pure helpers for custom-draw placement (canvas() regions).  Stateless, so they
    live inline with the geometry types they operate on.  The cut_* family is the "rectcut" idiom:
    each slices a strip off one edge of *r, shrinks *r to the remainder, and returns the slice --
    chain them to carve a canvas into label columns / content panes the way the row / column tracks
    carve a region, instead of hand-computing absolute offsets.

    gui_rect_t bar    = gui_rect_cut_top( &r, 24.0f );   // 24px strip off the top; r shrinks
    gui_rect_t labels = gui_rect_cut_left( &r, 80.0f );  // 80px label column; r is the rest
==============================================================================================*/

/* Shrink r inward by per-edge insets. */
static inline gui_rect_t
gui_rect_inset( gui_rect_t r, gui_pad_t p )
{
    return ( gui_rect_t ){ r.x + p.l, r.y + p.t, r.w - p.l - p.r, r.h - p.t - p.b };
}

/* Shrink r inward by the same margin on every edge (the common uniform-inset case). */
static inline gui_rect_t
gui_rect_pad( gui_rect_t r, f32 a )
{
    return ( gui_rect_t ){ r.x + a, r.y + a, r.w - (2.0f * a), r.h - (2.0f * a) };
}

/* Center point of r. */
static inline gui_vec2_t
gui_rect_center( gui_rect_t r )
{
    return ( gui_vec2_t ){ r.x + r.w * 0.5f, r.y + r.h * 0.5f };
}

/* True when (x,y) lies in r -- left / top inclusive, right / bottom exclusive, so abutting rects
   partition the plane with no overlap (the pixel-coverage convention). */
static inline bool
gui_rect_contains( gui_rect_t r, f32 x, f32 y )
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* rectcut: slice `a` px off the left of *r, return the slice, leave *r as the remainder. */
static inline gui_rect_t
gui_rect_cut_left( gui_rect_t* r, f32 a )
{
    if ( a > r->w ) a = r->w;
    gui_rect_t cut = ( gui_rect_t ){ r->x, r->y, a, r->h };
    r->x += a;
    r->w -= a;
    return cut;
}

/* rectcut: slice `a` px off the right of *r. */
static inline gui_rect_t
gui_rect_cut_right( gui_rect_t* r, f32 a )
{
    if ( a > r->w ) a = r->w;
    r->w -= a;
    return ( gui_rect_t ){ r->x + r->w, r->y, a, r->h };
}

/* rectcut: slice `a` px off the top of *r. */
static inline gui_rect_t
gui_rect_cut_top( gui_rect_t* r, f32 a )
{
    if ( a > r->h ) a = r->h;
    gui_rect_t cut = ( gui_rect_t ){ r->x, r->y, r->w, a };
    r->y += a;
    r->h -= a;
    return cut;
}

/* rectcut: slice `a` px off the bottom of *r. */
static inline gui_rect_t
gui_rect_cut_bottom( gui_rect_t* r, f32 a )
{
    if ( a > r->h ) a = r->h;
    r->h -= a;
    return ( gui_rect_t ){ r->x, r->y + r->h, r->w, a };
}

/*==============================================================================================
    Content Alignment

    Where a widget's natural-sized content sits inside the cell it is handed.

    Two independent axes, ORed together; 0 (LEFT | TOP) is the default and matches the original
    behavior.  A region carries one alignment (gui()->align, or the `align` field of a layout
    descriptor), persisting like the row template until changed.  It governs *content* placement
    (a text label, an image) -- a widget whose frame fills the cell (button, input) still fills it,
    and only its label/glyphs follow the alignment.  rect_align() is the single placement seam.

==============================================================================================*/

typedef enum
{
    GUI_ALIGN_LEFT    = 0,            // horizontal: against the left edge (default)
    GUI_ALIGN_HCENTER = 1 << 0,       // horizontal: centered
    GUI_ALIGN_RIGHT   = 1 << 1,       // horizontal: against the right edge

    GUI_ALIGN_TOP     = 0,            // vertical: against the top edge (default)
    GUI_ALIGN_VCENTER = 1 << 2,       // vertical: centered
    GUI_ALIGN_BOTTOM  = 1 << 3,       // vertical: against the bottom edge

    GUI_ALIGN_CENTER  = GUI_ALIGN_HCENTER | GUI_ALIGN_VCENTER,   // both axes centered

} gui_align_t;

/*==============================================================================================
    Placement adapters -- position a self-sized box inside an existing rect, the free-placement
    companion to split / carve (which divide a rect into adjacent panels).  These never touch the
    layout pen: they take a parent rect and return a child rect, so they compose with content_rect,
    push_layout_overlay and each other, and an overlay is just several placements over one area in
    draw order.  Pure rect math, so inline here with the cut_* / inset helpers above.

        gui_rect_t hud = gui()->content_rect();
        draw_minimap( gui_anchor_box( hud, 160, 160, GUI_ALIGN_RIGHT | GUI_ALIGN_TOP,    pad8 ) );
        draw_health ( gui_anchor_box( hud, 220,  18, GUI_ALIGN_LEFT  | GUI_ALIGN_BOTTOM, pad8 ) );
==============================================================================================*/

/* Seat a self-sized nat_w x nat_h box inside `area` per the gui_align_t flags -- the same rule a
   widget uses to place its label/symbol, now callable on any rect.  0 (LEFT | TOP) hugs the corner. */
static inline gui_rect_t
gui_rect_align( gui_rect_t area, f32 nat_w, f32 nat_h, gui_align_t align )
{
    f32 x = ( align & GUI_ALIGN_HCENTER ) ? area.x + ( area.w - nat_w ) * 0.5f
          : ( align & GUI_ALIGN_RIGHT   ) ? area.x +   area.w - nat_w
                                          : area.x;
    f32 y = ( align & GUI_ALIGN_VCENTER ) ? area.y + ( area.h - nat_h ) * 0.5f
          : ( align & GUI_ALIGN_BOTTOM  ) ? area.y +   area.h - nat_h
                                          : area.y;
    return ( gui_rect_t ){ x, y, nat_w, nat_h };
}

/* Pin a fixed w x h box to a corner / edge of `area`, inset from that edge by margin `m`.  The HUD
   idiom (health bottom-left, minimap top-right, crosshair centered): align over a padded rect. */
static inline gui_rect_t
gui_anchor_box( gui_rect_t area, f32 w, f32 h, gui_align_t align, gui_pad_t m )
{
    return gui_rect_align( gui_rect_inset( area, m ), w, h, align );
}

// clang-format on
/*============================================================================================*/
#endif    // GUI_RECT_H
