#ifndef GUI_RECT_H
#define GUI_RECT_H
/*==============================================================================================

    runtime_service/gui/rect/gui_rect.h -- GUI_RECT: the leaf rect kit.

    Pure stateless carve math over the geometry types -- no pen, no region state, no draw,
    no gui context.  Depends on nothing but base types, so it is usable anywhere, even
    outside gui.  Layout of any kind is a rect PRODUCER and a widget is a rect CONSUMER;
    these are the verbs both sides share.  The vtable half of this library (content_rect,
    split, carve, anchor) lives in the GUI_RECT section of gui_api.h.  The compiled half
    (color blend + alignment placement, rect/gui_rect_core.c) is declared at the bottom;
    the root unit gui_rect.c is the whole library's translation unit.

==============================================================================================*/

#include "orb.h"

// clang-format off
/*==============================================================================================
    Loud-overflow reporting (here since R11 -- every unit's pools stand on this leaf kit)

    Every fixed pool in the gui follows the same saturation rule: never fail hard, never be
    silent.  The overflowing site degrades gracefully (drop / share / evict) but reports ONCE
    per run so the symptom traces to its cap instead of reading as a rendering or input bug.
    This macro is the report half: printf so the message reaches plain consoles (the engine log
    may not be up yet), fflush so it lands before a follow-up ORB_ASSERT_MSG_ONCE can trap.
    The using .c file provides <stdio.h> (every unit root includes it).
==============================================================================================*/

#define GUI_WARN_ONCE( ... )                              \
    do                                                    \
    {                                                     \
        static bool s_gui_warned_once;                    \
        if ( !s_gui_warned_once )                         \
        {                                                 \
            printf( "[gui] WARNING: " __VA_ARGS__ );      \
            fflush( stdout );                             \
            s_gui_warned_once = true;                     \
        }                                                 \
    } while ( 0 )

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

/*==============================================================================================
    Shared stateless helpers -- small pure scalar/geometry helpers used across every unit
    (the render backend needs rect_intersect for clip nesting).  static inline so each TU
    gets its own copy with no linkage; they touch nothing but their arguments.
==============================================================================================*/

/* Clamp t to [0,1] -- the saturate used by slider + scrollbar drag mapping. */
static inline f32
saturate( f32 t ) { return t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t ); }

/* Clamp v to [lo,hi]. */
static inline f32
clampf( f32 v, f32 lo, f32 hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

/* Overlap of two rects (zero-size when they do not overlap).  Nested regions intersect their
   clip with the parent so a child never scissors or hit-tests past it. */
static inline gui_rect_t
rect_intersect( gui_rect_t a, gui_rect_t b )
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = ( a.x + a.w < b.x + b.w ) ? a.x + a.w : b.x + b.w;
    f32 y1 = ( a.y + a.h < b.y + b.h ) ? a.y + a.h : b.y + b.h;
    f32 w  = x1 - x0 > 0.0f ? x1 - x0 : 0.0f;
    f32 h  = y1 - y0 > 0.0f ? y1 - y0 : 0.0f;
    return ( gui_rect_t ){ x0, y0, w, h };
}

/*==============================================================================================
    Compiled half (rect/gui_rect_core.c) -- the non-inline primitives every layer above
    consumes.  Still pure: no ambient state, no draw, no gui context.
==============================================================================================*/

/* Linear blend between two ABGR colors at t in [0,1] (0 = ca, 1 = cb). */
u32 col_lerp( u32 ca, u32 cb, f32 t );

/* Horizontal / vertical placement of an extent `len` within a cell span, reading the matching
   gui_align_t bits -- the scalar halves of gui_rect_align for callers that own one axis. */
f32 align_x( f32 x, f32 w, f32 len, u32 a );
f32 align_y( f32 y, f32 h, f32 len, u32 a );

/* Place a natural nat_w x nat_h box inside `cell` per the alignment flags (gui_align_t).  The
   single seam for positioning sub-cell content -- thin compiled alias for gui_rect_align so
   widgets and callers share one rule. */
gui_rect_t rect_align( gui_rect_t cell, f32 nat_w, f32 nat_h, u32 align );

// clang-format on
/*============================================================================================*/
#endif    // GUI_RECT_H
