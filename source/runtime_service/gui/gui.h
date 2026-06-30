#ifndef GUI_H
#define GUI_H
/*==============================================================================================

    runtime_service/gui/gui.h -- gui module types.

    Include in DLL modules that use gui through the vtable (gui()->...).
    Include gui_host.h instead for direct-call access (host, sandbox).

==============================================================================================*/

#include "orb.h"

// clang-format off
/*==============================================================================================
    GUI: ID
==============================================================================================*/

/* Widget id -- a hashed value creates a unique value to identify a widget */

typedef u32 gui_id_t;
#define GUI_ID_NONE 0u

/* Icon handle -- identifies one symbol packed into the runtime icon atlas (register_icon).
   The atlas is a second R8 coverage texture that lives beside the font atlas and batches in
   the same flush; icons draw as tinted quads via image / draw_icon_in.  0 means "no icon"
   (an unregistered name or a full atlas), and draw helpers no-op on it. */

typedef u32 gui_icon_id_t;
#define GUI_ICON_NONE 0u

/* Opaque viewport handle -- a render surface backed by an OS window.  Returned by
   viewport_open; passed to render, viewport_resize, viewport_close, and
   window_set_next_viewport.  GUI_VP_INVALID (-1) signals failure or no assignment. */

typedef i32  gui_vp_t;
#define GUI_VP_INVALID  (-1)

/* Opaque dock-node handle -- one region of a viewport's dock tree.  Returned by dockspace_over_viewport
   (the tree root) and dock_split (the new sibling), and passed to dock_split / dock_window to name a
   target region.  0 (GUI_DOCK_NONE) signals "no node" -- a failed call or an unassigned slot. */

typedef u32  gui_dock_id_t;
#define GUI_DOCK_NONE  0u

/* Context configuration -- sizes the per-context resource pools at creation time.
   Pass to ctx_create(); NULL or zero fields default to the EDITOR preset (32 windows,
   512 state slots, 8 popup depth, 4 viewports, 48 dock nodes).
   max_dock_nodes == 0 is valid and disables docking for that context. */

typedef struct
{
    u32  max_windows;    /* persisted window pool (default 32) */
    u32  state_slots;    /* keyed state pool, must be power of two (default 512) */
    u32  popup_depth;    /* max popup nesting (default 8) */
    u32  max_viewports;  /* render surfaces (default 4) */
    u32  max_dock_nodes; /* dock-tree node pool; 0 = no docking (default 48) */

} gui_ctx_config_t;

/* Pre-built configs. */
#define GUI_CTX_CONFIG_EDITOR  \
    ( ( gui_ctx_config_t ){ 32, 512, 8, 4, 48 } )
#define GUI_CTX_CONFIG_GAME_UI \
    ( ( gui_ctx_config_t ){ 8, 64, 4, 1, 0 } )

/* Opaque context handle -- integer index into the internal context pool.
   GUI_CTX_DEFAULT (0) is always valid after init().
   GUI_CTX_INVALID (-1) signals a failed ctx_create or an unset handle. */

typedef i32 gui_ctx_t;
#define GUI_CTX_DEFAULT  0
#define GUI_CTX_INVALID  (-1)

/*==============================================================================================
    GUI: Geometry
==============================================================================================*/

typedef struct { f32 x, y; }        gui_vec2_t;
typedef struct { f32 x, y, w, h; }  gui_rect_t;

/* Callback fired by input_text_ex after any frame that modifies the buffer.
   buf is the live caller-owned buffer (may be read or written); len is the current byte
   length (excluding NUL); bufsz is the total buffer capacity. */
typedef void ( *gui_text_cb_fn )( char* buf, u32 len, u32 bufsz, void* user );

/* Monotonic wall-clock source (seconds), supplied by the host to the built-in perf overlay.
   gui has no timing service of its own (it is a leaf of rhi + app), so the host hands it a
   tick-seconds callback -- typically sys()->tick_seconds -- and gui uses it to measure the
   per-frame emit (build) and render (flush) cost the overlay reports.  See perf_overlay(). */
typedef f64 ( *gui_clock_fn )( void );

/* Edge insets, in pixels.  Region padding -- the gap between a region's box and where its layout
   starts (see gui_pad).  Breathing room *inside* a widget's frame is a per-widget style concern
   (WIDGET_PAD), not a layout one; spacing *between* cells is gap_x / gap_y. */

typedef struct { f32 l, r, t, b; }  gui_pad_t;

/*----------------------------------------------------------------------------------------------
    Rect algebra -- pure helpers for custom-draw placement (canvas() regions).  Stateless, so they
    live inline with the geometry types they operate on.  The cut_* family is the "rectcut" idiom:
    each slices a strip off one edge of *r, shrinks *r to the remainder, and returns the slice --
    chain them to carve a canvas into label columns / content panes the way the row / column tracks
    carve a region, instead of hand-computing absolute offsets.

        gui_rect_t bar    = gui_rect_cut_top( &r, 24.0f );   // 24px strip off the top; r shrinks
        gui_rect_t labels = gui_rect_cut_left( &r, 80.0f );  // 80px label column; r is the rest
----------------------------------------------------------------------------------------------*/

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
    return ( gui_rect_t ){ r.x + a, r.y + a, r.w - 2.0f * a, r.h - 2.0f * a };
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

/*----------------------------------------------------------------------------------------------
    Angles -- the arc / pie / spinner / progress sweep parameters (draw_arc, draw_pie, ...) are
    radians in screen space (y down, so a positive angle turns clockwise; 0 points right / +x).
    Author in friendly degrees and convert at the call site:

        gui()->draw_arc( cx, cy, r, gui_radians( 0 ), gui_radians( 270 ), 3.0f, col );
        gui()->draw_pie( cx, cy, r, gui_radians( -90 ), gui_radians( 90 ), col );

    Stateless pure math, so these are inline here (no vtable entry) like the rect helpers above.
----------------------------------------------------------------------------------------------*/

#define GUI_PI 3.14159265358979f

/* Degrees -> radians (the unit the arc / pie / sweep parameters take). */
static inline f32 gui_radians( f32 degrees ) { return degrees * ( GUI_PI / 180.0f ); }

/* Radians -> degrees (to read a stored sweep back in friendly units). */
static inline f32 gui_degrees( f32 radians ) { return radians * ( 180.0f / GUI_PI ); }

/*==============================================================================================
    Layout template

    A region (a window body or a child_begin box) lays widgets out by carving its content area
    into cells.  gui_layout() installs a template that *persists and repeats*: every widget
    fills the next cell.  A region opens UNDECLARED (no template): the first layout header in its
    body names the mode -- stack() for the single flex column of auto height (the classic vertical
    stack), or columns / grid / form for the others.  See gui_layout_mode_t.

    Two modes, chosen by whether `rows` is set:

      Flow  (rows empty)  -- `cols` describe one row; it repeats *downward*, the pen accumulates,
                             and content grows + scrolls.  The everyday lists / forms / panels.

      Grid  (rows set)    -- `cols` x `rows` partition a *bounded* box (the region's content area
                             from the current pen to its bottom) into a fixed matrix, resolved up
                             front.  Widgets fill cells row-major; both axes are fixed, nothing
                             scrolls.  Titlebars, toolbars, split panes, dashboards, image grids.

    Column / row sizes use one overloaded f32 (the same rule on both axes):
        > 1.0         fixed pixels
        == 1.0        fill -- an equal share of the leftover (several fills split it evenly)
        (0.0, 1.0)    fraction of the gap-adjusted available extent
        == 0.0        natural -- the item's own content size.  This only has a measure where the
                      content is known at resolve time (pack mode, resolved per item); a pre-divided
                      column / grid track is resolved up front with no content, so a 0 there
                      collapses to a zero-width track -- use fill / fraction / px in columns + grid.
        <  0.0        GUI_END, the track-list terminator

    Gaps sit *between* cells and are subtracted before distribution, so a widget never sees or
    reasons about spacing -- it just fills the rect it is handed.
==============================================================================================*/

#define GUI_LAYOUT_COLS 8                     // max tracks on one axis (columns or rows)
#define GUI_END (-1.0f)                       // track-list terminator (any negative value)

/* carve markup sentinels -- nest a gui()->carve form (a single GUI_END-terminated f32 list, the same
   overloaded unit as cols).  A size FOLLOWED by a CUT is a container of that size, subdivided on the
   named axis until a matching GUI_END; a size followed by anything else is a leaf.  A form opens with
   a leading CUT that fills the whole area.  See gui()->carve. */
#define GUI_CUT_X (-2.0f)                     // open a nested column split (panels side by side)
#define GUI_CUT_Y (-3.0f)                     // open a nested row split (panels stacked)

/*----------------------------------------------------------------------------------------------
    Content alignment -- where a widget's natural-sized content sits inside the cell it is handed.

    Two independent axes, ORed together; 0 (LEFT | TOP) is the default and matches the original
    behavior.  A region carries one alignment (gui()->align, or the `align` field of a layout
    descriptor), persisting like the row template until changed.  It governs *content* placement
    (a text label, an image) -- a widget whose frame fills the cell (button, input) still fills it,
    and only its label/glyphs follow the alignment.  rect_align() is the single placement seam.
----------------------------------------------------------------------------------------------*/

typedef enum
{
    GUI_ALIGN_LEFT    = 0,            /* horizontal: against the left edge (default)  */
    GUI_ALIGN_HCENTER = 1 << 0,       /* horizontal: centered                         */
    GUI_ALIGN_RIGHT   = 1 << 1,       /* horizontal: against the right edge           */

    GUI_ALIGN_TOP     = 0,            /* vertical: against the top edge (default)      */
    GUI_ALIGN_VCENTER = 1 << 2,       /* vertical: centered                            */
    GUI_ALIGN_BOTTOM  = 1 << 3,       /* vertical: against the bottom edge             */

    GUI_ALIGN_CENTER  = GUI_ALIGN_HCENTER | GUI_ALIGN_VCENTER,   /* both axes centered */

} gui_align_t;

/*----------------------------------------------------------------------------------------------
    Placement adapters -- position a self-sized box inside an existing rect, the free-placement
    companion to split / carve (which divide a rect into adjacent panels).  These never touch the
    layout pen: they take a parent rect and return a child rect, so they compose with content_rect,
    push_layout_rect and each other, and an overlay is just several placements over one area in
    draw order.  Pure rect math, so inline here with the cut_* / inset helpers above.

        gui_rect_t hud = gui()->content_rect();
        draw_minimap( gui_anchor_box( hud, 160, 160, GUI_ALIGN_RIGHT | GUI_ALIGN_TOP,    pad8 ) );
        draw_health ( gui_anchor_box( hud, 220,  18, GUI_ALIGN_LEFT  | GUI_ALIGN_BOTTOM, pad8 ) );
----------------------------------------------------------------------------------------------*/

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

/*----------------------------------------------------------------------------------------------
    Anchor frame -- the general placement (UE4 Slate model): a normalized sub-rect of the parent
    (0..1 per axis) plus pixel offsets, resolved per axis by gui()->anchor.  On an axis where
    min == max the child is point-anchored: the anchor is a single line at that fraction, the child
    takes `size` and is hung off it by `pivot` (0 = near edge sits on the line, 0.5 = centered, 1 =
    far edge), shifted by the offset.  On an axis where min < max the child is stretch-anchored: its
    edges track those two parent fractions and the offsets become per-edge insets (size is ignored).
    This unifies "pin a badge 40% across" and "stretch a bar over the top with 8px margins".
----------------------------------------------------------------------------------------------*/

typedef struct
{
    gui_vec2_t  min;     // normalized 0..1: anchor's near edge as a fraction of the parent
    gui_vec2_t  max;     // normalized 0..1: anchor's far edge ( == min for a point anchor )
    gui_vec2_t  pivot;   // point-anchor only: which point of the child sits on the line ( 0.5 = center )
    gui_vec2_t  size;    // point-anchor only: child w / h in px
    gui_pad_t   off;     // point: l / t shift the pivot; stretch: l / t / r / b inset the tracked edges

} gui_anchor_t;

typedef struct
{
    f32             cols[ GUI_LAYOUT_COLS ];    // column tracks, GUI_END-terminated (see unit rule)
    f32             rows[ GUI_LAYOUT_COLS ];    // row tracks; empty/NULL => flow mode, else grid mode
    f32             row_h;                      // flow only -- row height: 0 = auto, >0 = pixels
    f32             gap_x, gap_y;               // inter-cell spacing; 0 = theme default
    gui_align_t     align;                      // content alignment within each cell (0 = LEFT | TOP)

} gui_layout_t;

/*==============================================================================================
    Layout mode -- the next-item methodology a region is laying out under.

    A region opens UNDECLARED (NONE): the first layout header names the mode (stack / columns /
    grid / ...), and a widget emitted before any header is a usage error (debug assert; a release
    build falls back to STACK rather than faulting).  This replaces the old silent single-column
    default -- the mode is now always explicit at the top of a region body.  The mode is the
    "next item methodology"; the per-cell sizing inside it is still the one overloaded unit rule.
==============================================================================================*/

typedef enum
{
    GUI_MODE_NONE = 0,    /* no header declared yet -- emitting a widget here is a usage error */
    GUI_MODE_STACK,       /* single flex column, rows accumulate + scroll (the vertical list)  */
    GUI_MODE_COLUMNS,     /* N pre-divided column tracks, rows accumulate + scroll             */
    GUI_MODE_GRID,        /* bounded cols x rows matrix, both axes fixed, nothing scrolls      */
    GUI_MODE_PACK,        /* natural-size print run, placed item-by-item along an axis (bar/strip) */

} gui_layout_mode_t;

/*==============================================================================================
    Pack direction -- the axis a pack() run places items along, item-by-item at natural size.
    bar() is the horizontal pack (a toolbar); strip() is the vertical pack.
==============================================================================================*/

typedef enum
{
    GUI_PACK_HORIZONTAL = 0,    /* bar:   items flow left to right, nextline wraps down    */
    GUI_PACK_VERTICAL   = 1,    /* strip: items flow top to bottom, nextline wraps across  */

} gui_pack_dir_t;

/*==============================================================================================
    Split axis -- the axis gui()->split carves a rect along.  X lays the panels left-to-right
    (a column split: a sidebar + content); Y lays them top-to-bottom (a row split: header /
    body / footer).  The panel sizes use the same overloaded unit as the column tracks.
==============================================================================================*/

typedef enum
{
    GUI_AXIS_X = 0,    /* carve into vertical panels side by side (columns)  */
    GUI_AXIS_Y = 1,    /* carve into horizontal panels stacked (rows)        */

} gui_axis_t;

/*==============================================================================================
    Field label side -- where a labeled value widget (input_text / slider_float / checkbox) puts
    its label when a field split is active (gui()->field_split / field_label_left).  The label and
    control are two tracks resolved across the widget's cell with the same overloaded unit as
    columns; `side` only decides which track sits on which edge.  NONE is the default: the label
    trails the control at its natural width on the right.
==============================================================================================*/

typedef enum
{
    GUI_LABEL_NONE  = 0,      /* off -- natural-width label trailing on the right (default) */
    GUI_LABEL_LEFT  = 1,      /* label track on the left, control fills the right */
    GUI_LABEL_RIGHT = 2,      /* label track on the right, control fills the left */

} gui_label_side_t;

/*==============================================================================================
    Window drag mode -- how a window may be repositioned by the mouse.
    Selected globally via gui()->window_set_drag(); default is TITLEBAR.
==============================================================================================*/

typedef enum
{
    GUI_WIN_DRAG_NONE     = 0,    /* windows are fixed in place                          */
    GUI_WIN_DRAG_TITLEBAR = 1,    /* drag only by the title bar (default)                */
    GUI_WIN_DRAG_BODY     = 2,    /* drag from anywhere in the window not over a widget  */

} gui_win_drag_t;

/*==============================================================================================
    Apply condition -- when a queued window_set_next_* value takes effect on its target window.

    Passed to window_set_next_pos / window_set_next_size.  The value and the condition are two
    separate axes: the same window can be seeded once, forced every frame, or re-applied whenever
    it re-appears, by changing only the condition -- the reason geometry is a side channel rather
    than fixed window_begin parameters.  Bit values so a window can mask the conditions it still
    permits.  A 0 (unset) condition is treated as ALWAYS.
==============================================================================================*/

typedef enum
{
    GUI_COND_ONCE      = 1 << 0,   /* apply once for this window, then never again -- seeds the
                                      initial position/size on first appearance and never again
                                      (akin to Dear ImGui's FirstUseEver until a saved-layout lands) */

    GUI_COND_ALWAYS    = 1 << 1,   /* apply every frame the value is queued -- forced geometry for
                                      layout managers, snapping, animation; pair with NOMOVE /
                                      NORESIZE so a user drag does not fight it */

    GUI_COND_APPEARING = 1 << 2,   /* apply each time the window appears -- on creation and again
                                      whenever it is shown after a frame of absence (e.g. re-center
                                      a reopened popup / modal) */
} gui_cond_t;

/*==============================================================================================
    Window flags

    Passed as the final argument to window_begin to customize a single window's behavior.
    They mostly switch off default behavior; pass 0 (GUI_WIN_NONE) for the defaults.
==============================================================================================*/

typedef enum
{
    GUI_WIN_NONE              = 0,         /* default behavior */
    GUI_WIN_NOTITLEBAR        = 1 << 0,    /* no title bar: body fills the top; no collapse, no titlebar drag */
    GUI_WIN_NOCOLLAPSE        = 1 << 1,    /* no collapse arrow; the window stays expanded */
    GUI_WIN_NORESIZE          = 1 << 2,    /* disable user resizing from the border edges */
    GUI_WIN_NOMOVE            = 1 << 3,    /* disable user drag moving the window from anywhere */

    /* scrollbars -- dynamic: vertical bar and mouse wheel input are enabled by default */

    GUI_WIN_NOSCROLL          = 1 << 4,    /* disable all scroll bars (keep mouse input) */
    GUI_WIN_HSCROLL           = 1 << 5,    /* enable dynamic horizontal scroll bar (off by default) */
    GUI_WIN_NOMOUSESCROLL     = 1 << 6,    /* disable mouse wheel scrolling */
    
    /* scrollbars - static: override dynamic bar flags */

    GUI_WIN_ALWAYS_VSCROLL    = 1 << 7,    /* always show vertical scroll bar -- override*/
    GUI_WIN_ALWAYS_HSCROLL    = 1 << 8,    /* always show horizontal scroll bar -- override */
    
    /* auto-resize -- size the window to its content instead of a fixed w/h */

    GUI_WIN_ALWAYS_AUTOSIZE   = 1 << 9,    /* hug content every frame: no user resize, no scrollbars */
    GUI_WIN_CAN_AUTOSIZE      = 1 << 10,   /* show a corner size-grip; double-click it to fit content */

    /* child resize -- child_begin only (the ImGuiChildFlags_ResizeX / _ResizeY analogue).  A
       draggable grip on the child's right / bottom border; the size on that axis then becomes
       user-owned and persisted -- seeded once from the child_begin w/h, thereafter set by the
       drag -- overriding the passed value.  RESIZE_Y supersedes the h<=0 auto-size on that axis.
       A real window ignores these (it owns its geometry already), as does a grid-cell child
       (the cell sizes it).  Vertical is the common case; both axes may be combined. */

    GUI_WIN_CHILD_RESIZE_X    = 1 << 11,   /* child: drag the right border to resize width   */
    GUI_WIN_CHILD_RESIZE_Y    = 1 << 12,   /* child: drag the bottom border to resize height  */

    /* Menu bar -- reserve a one-row strip below the title bar that menu_bar_begin fills (the
       ImGuiWindowFlags_MenuBar analogue).  The strip is carved from the top of the body before the
       body scroll region opens, so it never scrolls; menu_bar_begin returns false unless set. */

    GUI_WIN_MENUBAR           = 1 << 13,   /* reserve a non-scrolling menu-bar strip (menu_bar_begin) */

    /* Native-borderless: this window IS its host OS window (window kind 3).  Its titlebar stands in
       for the Win32 caption and its border for the sizing frame, so titlebar drag / double-click /
       right-click and border drags are routed to native OS window actions (app()->window_start_move
       / window_title_event / window_system_menu / window_start_resize) instead of gui's in-client
       move, tear-off, collapse, and edge-resize.  The host OS window must have been opened with
       APP_WIN_BORDERLESS.  Geometry is owned by the OS window (size follows WM_SIZE). */

    GUI_WIN_NATIVE            = 1 << 14,

    /* Title-bar capability -- subtract a caption control a window should not offer.  All three
       buttons show by default (opt-out, like the NO* flags above); a window drops the ones it does
       not support and the title bar reflects that, reclaiming the freed space for the title text.

       NO_MINIMIZE / NO_MAXIMIZE gate the OS minimize / maximize caption buttons and so apply only to
       a native window (GUI_WIN_NATIVE or a detached floater) -- a non-native panel has no such OS
       state and never drew them.  The close (main) / pop-in (floater) primary button is never
       suppressed: close is essential and pop-in is a floater's only route back to the main surface.

       NO_DETACH removes the pop-out path for any window -- it hides the non-native detach button and
       blocks the drag tear-off -- independent of NOMOVE (a window may move yet refuse to pop out). */

    GUI_WIN_NO_MINIMIZE       = 1 << 15,   /* native: no minimize caption button */
    GUI_WIN_NO_MAXIMIZE       = 1 << 16,   /* native: no maximize / restore caption button */
    GUI_WIN_NO_DETACH         = 1 << 17,   /* no pop-out: hide detach button, block tear-off drag */

    /* Placement is managed externally (docking layout, animation, scripted snap).  Bypasses both
       the per-drag margin clamp (window_clamp) and the merge-back fit-inside clamp so the system
       can position and size the window freely without gui fighting the placement.  Without this
       flag both clamps apply unconditionally; with it neither does. */

    GUI_WIN_NO_BOUNDARY_CLAMP = 1 << 18,

    /* Closeable -- add a close (X) button at the title bar's right edge.  Clicking it hides the
       window: window_begin returns false and emits nothing from then on, and the record persists
       so the window keeps its position / size while closed.  Re-opening is the caller's job --
       offer a button that calls window_set_open( title, true ).  A native window uses its OS close
       caption button instead, so this flag only adds the X to a regular (non-native) panel. */

    GUI_WIN_CLOSEABLE         = 1 << 19,   /* show a close (X) button; hidden until re-opened */

    /* Input passthrough -- the window is purely visual; the cursor passes through it as if it
       were not there.  hover_win is never set to this window, so no widget inside can receive
       mouse input and the window never steals clicks from content behind it.  Combine with
       GUI_WIN_OVERLAY for a completely inert HUD (the perf overlay uses this). */

    GUI_WIN_NO_INPUT          = 1 << 20,   /* click-through: never becomes hover_win */

    /* Convenience composites -- common flag bundles named for intent (the ImGuiWindowFlags_NoXxx
       shorthands).  Plain ORs of the bits above, so they compose with extra flags as usual
       ( GUI_WIN_OVERLAY | GUI_WIN_NOMOUSESCROLL ) and a window's resolved behavior is identical
       to spelling the members out.

       NODECORATION -- strip all chrome: no title bar, no border resize, no scrollbars, no collapse.
                       A bare content panel you still position / move yourself.
       OVERLAY      -- a passive, non-interactive HUD: undecorated, fixed in place, hugging its
                       content every frame, and non-detachable.  The "accepts no action" window --
                       what the built-in perf overlay uses.  Pin it with window_set_next_pos. */

    GUI_WIN_NODECORATION = GUI_WIN_NOTITLEBAR | GUI_WIN_NORESIZE |
                           GUI_WIN_NOSCROLL   | GUI_WIN_NOCOLLAPSE,

    GUI_WIN_OVERLAY      = GUI_WIN_NODECORATION    | GUI_WIN_NOMOVE |
                           GUI_WIN_ALWAYS_AUTOSIZE | GUI_WIN_NO_DETACH | GUI_WIN_NO_INPUT,

} gui_win_flags_t;

/*==============================================================================================
    Dockspace flags

    Passed to dockspace_over_viewport.  0 (GUI_DOCKSPACE_NONE) is the default dockspace that fills
    the viewport behind the free-floating windows.  Bit values so future policy bits (e.g. hide the
    single-tab strip, no central-node auto-hide) can be ORed in without changing the call sites.
==============================================================================================*/

typedef enum
{
    GUI_DOCKSPACE_NONE = 0,    /* default: fill the viewport, draw splitters + tab bars */

} gui_dockspace_flags_t;

/*==============================================================================================
    Item flags

    A push-model of per-item behavior tweaks, the ImGui ItemFlags analogue.  Instead of widening
    every widget signature with a new parameter, behavior is tuned through a flag set the widget
    reads at emit time, so a feature can be added without touching any call site.

    Two layers merge into the flags a widget sees:

      Stack    -- push_item_flag( flag, enable ) / pop_item_flag(): affects every widget until
                  popped (disable a run of buttons, mark a section read-only).  Nests; pop restores.
      Next     -- next_item_flag( flag, enable ): a one-shot override consumed by the very next
                  widget only, no pop needed.  Overrides the stack for that one item (it can force
                  a bit off even when the stack has it on).

    The merged value is resolved once per widget; a widget that does not care about a given flag
    simply ignores it, so unknown / future flags are inert by construction.  Bit values so several
    can be combined; 0 (GUI_ITEM_NONE) is the default no-op set.
==============================================================================================*/

typedef enum
{
    /* no tweaks -- the default behavior */
    GUI_ITEM_NONE          = 0,       

    /* inert + dimmed: no hover/active/focus/click, drawn at
       reduced opacity.  Honored uniformly by widget_behavior and
       the draw list, so it applies to every widget at once. */
    GUI_ITEM_DISABLED      = 1 << 0,  

    /* a held button fires repeatedly: once on press, then after
       an initial delay at a steady rate (spinner / scroll arrows),
       instead of once on release.  Honored by widget_behavior, so
       any button-kind widget under the flag auto-repeats. */
    GUI_ITEM_BUTTON_REPEAT = 1 << 1,  

    /* slider_float: suppress the value text drawn centered on the
       track.  The value is shown by default; set this (push or
       next_item_flag) to hide it for a bare / compact slider. */
    GUI_ITEM_NO_VALUE_TEXT = 1 << 2,  

    /* Room to grow without disturbing call sites or the vtable -- e.g. a future
    GUI_ITEM_READ_ONLY (editable widgets show but reject input), GUI_ITEM_NO_NAV, etc. */

} gui_item_flags_t;

/*==============================================================================================
    Direction -- a cardinal direction, the ImGuiDir analogue.  Passed to arrow_button (and any
    future directional widget) to pick which way the glyph points.
==============================================================================================*/

typedef enum
{
    GUI_DIR_LEFT,
    GUI_DIR_RIGHT,
    GUI_DIR_UP,
    GUI_DIR_DOWN,

} gui_dir_t;

/*==============================================================================================
    Color edit flags
==============================================================================================*/

typedef enum
{
    GUI_COLOR_EDIT_NONE        = 0,
    GUI_COLOR_EDIT_NO_ALPHA    = 1 << 0,  /* ColorEdit4 with alpha ignored/hidden */
    GUI_COLOR_EDIT_DISPLAY_HSV = 1 << 1,  /* Display inputs as HSV */
    GUI_COLOR_EDIT_FLOAT       = 1 << 2,  /* Display inputs as float 0..1 instead of 0..255 */

} gui_color_edit_flags_t;

/*==============================================================================================
    Combo flags

    Passed to combo_begin to tune the dropdown.  The HEIGHT_* group caps the dropdown to a fixed
    number of visible rows (then it scrolls) -- the ImGuiComboFlags_Height* analogue; they are
    mutually exclusive, so set exactly one (an unset height defaults to REGULAR / 8 rows).  0
    (GUI_COMBO_NONE) is the default no-tweak set.
==============================================================================================*/

typedef enum
{
    GUI_COMBO_NONE            = 0,         /* default behavior (REGULAR height) */

    GUI_COMBO_HEIGHT_SMALL    = 1 << 0,    /* cap the dropdown to ~4 rows, then scroll   */
    GUI_COMBO_HEIGHT_REGULAR  = 1 << 1,    /* cap to ~8 rows (the default), then scroll   */
    GUI_COMBO_HEIGHT_LARGE    = 1 << 2,    /* cap to ~20 rows, then scroll                */
    GUI_COMBO_HEIGHT_LARGEST  = 1 << 3,    /* no cap: as many rows as fit on screen       */

    /* Mask of the height bits, to clear the group before setting one (the demo idiom). */
    GUI_COMBO_HEIGHT_MASK     = GUI_COMBO_HEIGHT_SMALL | GUI_COMBO_HEIGHT_REGULAR
                                | GUI_COMBO_HEIGHT_LARGE | GUI_COMBO_HEIGHT_LARGEST,

} gui_combo_flags_t;

/*==============================================================================================
    Style colors

    The themeable color slots, the ImGuiCol_ analogue.  Each names one entry of the shared palette
    the widgets draw from; push_style_color( slot, abgr ) overrides it for every widget until the
    matching pop_style_color, next_style_color overrides it for just the next widget, and a slot
    left unpushed uses the theme default.  Colors are packed with GUI_COLOR (byte order R,G,B,A).

    The palette is shared rather than per-widget-type (one GUI_COL_WIDGET_BG, not Button +
    Checkbox + ...), matching the engine's single-palette theme: to recolor one button, bracket it
    with push/pop (only that button draws between them), or use next_style_color for a one-shot.
==============================================================================================*/

typedef enum
{
    GUI_COL_TEXT,           /* label / glyph text                          */
    GUI_COL_TEXT_DIM,       /* secondary text (trailing labels)            */
    GUI_COL_WINDOW_BG,      /* window body background                      */
    GUI_COL_CHILD_BG,       /* child region background                     */
    GUI_COL_TITLE_BG,       /* window title bar                            */
    GUI_COL_BORDER,         /* window / widget outlines                    */
    GUI_COL_WIDGET_BG,      /* idle widget body (button, checkbox, knob)   */
    GUI_COL_WIDGET_HOT,     /* hovered widget body                         */
    GUI_COL_WIDGET_ACT,     /* pressed / active widget body                */
    GUI_COL_WIDGET_FG,      /* widget foreground accent (slider fill)      */
    GUI_COL_CHECK_MARK,     /* checkbox tick / radio dot                   */
    GUI_COL_SLIDER_TRACK,   /* slider + scrollbar track                    */
    GUI_COL_RESIZE_HOT,     /* hot resize edge / size grip                 */
    GUI_COL_INPUT_BG,       /* text input field background                 */
    GUI_COL_INPUT_FOCUS,    /* focused text input field background         */
    GUI_COL_CURSOR,         /* text input caret                           */
    GUI_COL_NAV_HIGHLIGHT,  /* keyboard-nav focus ring around the nav item */

    GUI_COL_COUNT,          /* slot count -- not a color                   */

} gui_col_t;

/*==============================================================================================
    Global Style Configuration
==============================================================================================*/

typedef struct gui_style_t
{
    u32 colors[ GUI_COL_COUNT ]; /* Theme default palette (GUI_COLOR packs R,G,B,A bytes) */

    /* Layout metrics */
    u8 line_size;          // widget row height                                 
    u8 widget_gap;         // vertical gap between consecutive widgets
    u8 widget_pad;         // horizontal content area padding
    u8 win_title_h;        // window title bar height
    u8 win_border;         // window / widget outline thickness
    u8 checkbox_sz;        // checkbox indicator side
    u8 slider_knob_w;      // slider draggable knob width
    u8 min_cell_w;         // floor a flex/fraction track shrinks to before overflow
    u8 checkmark_pad;      // inset of filled square inside the checkbox
    u8 cursor_w;           // input text cursor width
    u8 cursor_inset;       // input text cursor top/bottom inset
    u8 win_rounding;       // corner radius: windows / children / popups
    u8 widget_rounding;    // corner radius: control frames
    u8 grab_rounding;      // corner radius: slider knobs / scrollbar grabs
    u8 check_style;        // checkbox/menu indicator: 0='v' tick, 1=disc, 2='X' (gui_check_style_t)
    u8 bullet_style;       // bullet glyph: 0=disc, 1=square (gui_bullet_style_t)
    u8 arrow_style;        // directional arrow: 0=triangle, 1=chevron (gui_arrow_style_t)
    u8 separator_style;    // separator rule: 0=solid, 1=dashed (gui_separator_style_t)
    u8 progress_style;     // progress fill: 0=solid, 1=gradient (gui_progress_style_t)
    u8 slider_knob;        // slider knob: 0=bar, 1=circle (gui_slider_knob_t)
    u8 menu_check;         // menu check gutter: 0=plain, 1=box (gui_menu_check_t)

} gui_style_t;

/* gui_style_get() -- returns a pointer to the mutable base style (s_style_base).  Edits take
   effect on the next gui_style_apply() / gui_theme_reset() call.  Mutating the struct directly
   without calling theme_reset marks the theme as anonymous (theme_get returns NULL). */

gui_style_t* gui_style_get( void );

/* gui_style_apply() -- recomputes the scaled active metrics from the current base style.
   Called automatically on font change; call manually after editing via gui_style_get(). */

void         gui_style_apply( void );

/*==============================================================================================
    Themes

    A theme is a named gui_style_t snapshot: a human-readable name paired with a complete set
    of colors and layout metrics.  The active theme is the root layer every push_style_color /
    push_style_var overrides relative to.  Switching or resetting a theme clears the push stacks
    immediately -- use this instead of managing deep push/pop sequences for large style changes.

        u32  n;
        const gui_theme_t* list = gui_theme_list( &n );  // enumerate built-ins
        for ( u32 i = 0; i < n; ++i ) puts( list[i].name );

        gui_theme_set( "light" );   // switch theme + clear style stacks
        gui_theme_reset();          // revert any style_get edits, clear stacks
==============================================================================================*/

typedef struct gui_theme_t
{
    const char* name;    /* human-readable key used by theme_set / theme_get */
    gui_style_t style;   /* complete color + metric snapshot                 */

} gui_theme_t;

const gui_theme_t* gui_theme_list ( u32* count_out );    /* enumerate built-in themes           */
bool               gui_theme_set  ( const char* name );  /* switch to named theme + reset stacks */
const char*        gui_theme_get  ( void );              /* active theme name, NULL if anonymous */
void               gui_theme_reset( void );              /* restore base + clear push stacks     */

/*==============================================================================================
    Style vars

    The tunable layout metrics, the ImGuiStyleVar_ analogue.  Each names one scalar pixel metric
    the layout + widgets read; push_style_var( var, value ) overrides it until the matching
    pop_style_var, next_style_var for just the next widget, and an unpushed var uses the
    font-derived default (recomputed when the font changes).  Values are f32 pixels.

    Only metrics that flow through the shared accessor are listed, so every slot here is honored
    uniformly everywhere it is read; purely cosmetic internals (caret width, checkmark inset) are
    intentionally left off rather than exposed as half-working knobs.
==============================================================================================*/

typedef enum
{
    GUI_VAR_LINE_SIZE,      // widget row height (the frame height)
    GUI_VAR_WIDGET_GAP,     // gap between consecutive widgets / cells
    GUI_VAR_WIDGET_PAD,     // content padding inside a frame (FramePadding)
    GUI_VAR_WIN_TITLE_H,    // window title bar height
    GUI_VAR_WIN_BORDER,     // window / widget outline thickness
    GUI_VAR_CHECKBOX_SZ,    // checkbox / radio indicator side
    GUI_VAR_SLIDER_KNOB_W,  // slider knob + scrollbar thickness
    GUI_VAR_MIN_CELL_W,     // min width a flex cell shrinks to
    GUI_VAR_WIN_ROUNDING,   // corner radius for windows / children / popups; 0 = square
    GUI_VAR_WIDGET_ROUNDING,// corner radius for control frames (button/checkbox/input/...)
    GUI_VAR_GRAB_ROUNDING,  // corner radius for slider knobs + scrollbar grabs
    GUI_VAR_CHECK_STYLE,    // checkbox/menu indicator: 0 = 'v' tick, 1 = filled disc, 2 = 'X' cross (gui_check_style_t)
    GUI_VAR_BULLET_STYLE,   // bullet glyph: 0 = filled disc, 1 = square (gui_bullet_style_t)
    GUI_VAR_ARROW_STYLE,    // directional arrow: 0 = filled triangle, 1 = stroked chevron (gui_arrow_style_t)
    GUI_VAR_SEPARATOR_STYLE,// separator rule: 0 = solid, 1 = dashed (gui_separator_style_t)
    GUI_VAR_PROGRESS_STYLE, // progress_bar fill: 0 = solid, 1 = vertical gradient (gui_progress_style_t)
    GUI_VAR_SLIDER_KNOB,    // slider knob shape: 0 = bar, 1 = circle (gui_slider_knob_t)
    GUI_VAR_MENU_CHECK,     // menu item check gutter: 0 = plain indicator, 1 = bordered box (gui_menu_check_t)

    GUI_VAR_COUNT,          // var count -- not a metric

} gui_style_var_t;

/* Checkbox / menu-item indicator shape (GUI_VAR_CHECK_STYLE).  Default is the tick. */
typedef enum
{
    GUI_CHECK_TICK  = 0,   // a two-stroke 'v' check mark
    GUI_CHECK_DISC  = 1,   // a filled disc inside the box
    GUI_CHECK_CROSS = 2,   // a two-diagonal 'X' cross

} gui_check_style_t;

/* Bullet glyph shape (GUI_VAR_BULLET_STYLE).  Default is the disc (Dear ImGui's RenderBullet). */
typedef enum
{
    GUI_BULLET_DISC   = 0,   // a small filled circle
    GUI_BULLET_SQUARE = 1,   // a small filled square

} gui_bullet_style_t;

/* Directional arrow shape (GUI_VAR_ARROW_STYLE).  Default is the solid triangle.  Threads through
   every arrow the chrome draws -- arrow_button, the collapse fold, the combo / submenu arrow, the
   dock overlay -- since they all route through draw_arrow, exactly as check / bullet do. */
typedef enum
{
    GUI_ARROW_FILLED  = 0,   // a filled triangle pointing the direction
    GUI_ARROW_CHEVRON = 1,   // a stroked '>' chevron (two strokes to an apex)

} gui_arrow_style_t;

/* Separator rule shape (GUI_VAR_SEPARATOR_STYLE).  Default is the solid rule.  Honored by
   separator() and the leading / trailing rules of separator_text(). */
typedef enum
{
    GUI_SEPARATOR_SOLID  = 0,   // a continuous filled rule
    GUI_SEPARATOR_DASHED = 1,   // a dashed rule           

} gui_separator_style_t;

/* progress_bar fill style (GUI_VAR_PROGRESS_STYLE).  Default is the solid fill; the gradient
   variant glosses the fill from the foreground accent to a brighter tint (top to bottom). */
typedef enum
{
    GUI_PROGRESS_SOLID    = 0,   // a flat foreground-accent fill
    GUI_PROGRESS_GRADIENT = 1,   // a top-to-bottom gradient gloss

} gui_progress_style_t;

/* Slider / drag knob shape (GUI_VAR_SLIDER_KNOB).  Default is the bar grab; the circle variant
   draws a round handle (raise GUI_VAR_GRAB_ROUNDING instead for a pill bar). */
typedef enum
{
    GUI_SLIDER_KNOB_BAR    = 0,   // a rectangular grab (grab-rounded)
    GUI_SLIDER_KNOB_CIRCLE = 1,   // a circular handle                

} gui_slider_knob_t;

/* Menu item check gutter style (GUI_VAR_MENU_CHECK).  Default is the bordered box, which draws
   an idle checkbox frame in the gutter whether or not the item is selected; the plain variant
   renders no box and only the indicator symbol when selected. */
typedef enum
{
    GUI_MENU_CHECK_PLAIN = 0,   // indicator only when selected; no idle box
    GUI_MENU_CHECK_BOX   = 1,   // bordered box always; indicator when selected

} gui_menu_check_t;

/*==============================================================================================
    Debug overlay layers

    Bitmask passed to gui()->debug_set_layers().  Each bit enables one bolt-on debug
    visualization, emitted into a separate draw list and painted last, on top of the UI.
    The overlay is compiled in for Debug builds only (GUI_DEBUG_OVERLAY); in a Release
    build set_layers is a no-op and get_layers returns 0.  These constants stay defined in
    every build so call sites compile unchanged.
==============================================================================================*/

typedef enum
{
    GUI_DBG_NONE     = 0,         // overlay off                                          }
    GUI_DBG_WINDOW   = 1 << 0,    // window outer frames; the hover window stands out     }
    GUI_DBG_INTERACT = 1 << 1,    // per-widget interaction rects (hover/active tinted)   }
    GUI_DBG_RESIZE   = 1 << 2,    // window edge-resize grab bands; hot when armed        }
    GUI_DBG_CLIP     = 1 << 3,    // clip (scissor) rectangle stack, colored by depth     }
    GUI_DBG_LAYOUT   = 1 << 4,    // layout allocated space per widget                    }

    GUI_DBG_ALL      = GUI_DBG_WINDOW | GUI_DBG_INTERACT | GUI_DBG_RESIZE | GUI_DBG_CLIP | GUI_DBG_LAYOUT,

} gui_dbg_layer_t;

/*==============================================================================================
    Debug render mode

    How the main UI draw list is rasterized, selected via gui()->debug_set_render_mode().  Unlike
    the debug overlay layers (a separate draw list painted on top), this changes the rasterization
    of the UI itself, so the two are independent and compose.  Available in every build (it is just
    a pipeline + push-constant switch, cheap enough to leave in Release).

      NORMAL    -- textured, blended UI (the default).
      WIREFRAME -- the geometry's triangle edges (VK_POLYGON_MODE_LINE), each window keeping its own
                   color: a direct read on how many triangles a shape costs.
      BATCH     -- every GPU draw call (one indexed draw == one batch) is tinted a distinct color, so
                   a color change marks a batch split -- count the colors to count the batches.
==============================================================================================*/

typedef enum
{
    GUI_RENDER_NORMAL    = 0,   // normal textured / blended UI                       */
    GUI_RENDER_WIREFRAME = 1,   // triangle edges only (wireframe)                    */
    GUI_RENDER_BATCH     = 2,   // per-draw-call color tint (batch boundary view)     */

    GUI_RENDER_MODE_COUNT,      // mode count -- not a mode                           */

} gui_render_mode_t;

/*==============================================================================================
    Color packing

    GUI_COLOR(r,g,b,a) packs 0-255 byte values into a u32 such that memory byte order
    is [R, G, B, A], matching VK_FORMAT_R8G8B8A8_UNORM vertex attribute layout.
==============================================================================================*/

#define GUI_COLOR( r, g, b, a ) \
    ( ( ( u32 )( a ) << 24 ) | ( ( u32 )( b ) << 16 ) | ( ( u32 )( g ) << 8 ) | ( u32 )( r ) )

/*==============================================================================================
    Line / path stroking

    Thickness, pixel-snapping, and where a stroke sits relative to the ideal path it is drawn from.
    Implementation in gui_draw_path.c.

    Pixel model: integer coordinates fall on the lines *between* pixels, so a crisp axis-aligned
    stroke is one whose two edges both land on integers.  draw_line strokes a single segment: a
    horizontal / vertical one snaps to the pixel grid and renders perfectly crisp (like a
    separator); any other angle is stroked with a 1px antialiased edge so diagonals stay smooth.
    draw_polyline / path_stroke connect several points with miter-limited corners (always
    antialiased) -- use them for multi-segment outlines, arrows, and diagonal runs.
==============================================================================================*/

/* Where the stroke sits across the ideal path (the line the coordinates describe).  CENTER runs
   the path down the middle of the stroke; INSIDE / OUTSIDE push the whole width onto one side (the
   left-hand normal of travel is the "inside").  CENTER_BIASED is CENTER plus a parity-aware snap so
   an odd-thickness axis-aligned line lands on whole pixels instead of straddling two -- the crisp
   default for UI rules and borders.  (The snap only bites on axis-aligned single segments; a
   diagonal or a multi-segment polyline treats CENTER_BIASED as CENTER and relies on antialiasing.) */
typedef enum
{
    GUI_STROKE_CENTER_BIASED = 0,   // centered + snapped to the pixel grid (default) 
    GUI_STROKE_CENTER,              // centered on the path, no snap                  
    GUI_STROKE_INSIDE,              // whole width on the interior side of a CW-screen ring 
    GUI_STROKE_OUTSIDE,             // whole width on the exterior side of a CW-screen ring

} gui_stroke_align_t;

#define GUI_PATH_MAX 256            /* max points path_line_to accumulates before a stroke */

/*==============================================================================================
    Draw vertex  (20 bytes, single interleaved binding)

    Vertex attribute layout (matches the gui pipeline):
        location 0 : float2  (x, y)       offset  0   -- pixel-space position
        location 1 : float2  (u, v)       offset  8   -- texture UV [0..1]
        location 2 : UNORM4  (abgr u32)   offset 16   -- packed color, R8G8B8A8_UNORM
==============================================================================================*/

typedef struct
{
    f32 x, y; // pixel position */
    f32 u, v; // texture UV     */
    u32 abgr; // packed color   */

} gui_draw_vert_t;

/*==============================================================================================
    Semantic draw commands

    The UI build pass emits one gui_cmd_t per visible shape into a list.  The render backend
    (gui_render.c) tessellates each command into vertices and indices at flush time.  This
    separates the UI logic from any graphics API knowledge.

    GPU draw commands (gui_gpu_cmd_t) are a backend-private type defined in gui_draw.c;
    they carry index ranges and bind state for one GPU draw call.
==============================================================================================*/

typedef enum
{
    GUI_CMD_RECT_FILLED,     // filled rectangle or textured quad (glyph)
    GUI_CMD_RECT_OUTLINE,    // hollow rectangle: four edge quads          
    GUI_CMD_TRIANGLE,        // solid triangle                             
    GUI_CMD_TEXT,            // glyph run from the font atlas              
    GUI_CMD_CIRCLE_FILLED,   // filled disc (triangle fan)                 
    GUI_CMD_LINE,            // single stroke segment                      
    GUI_CMD_POLYLINE,        // multi-segment antialiased polyline         
    GUI_CMD_DASHED_LINE,     // patterned line: one textured quad, atlas dash row, tiled by U */
    GUI_CMD_RECT_GRADIENT,   // filled rect, col_a->col_b blended by per-vertex color (one quad) */

} gui_cmd_type_t;

/* Sentinel half-extent for an unclipped text command: any real glyph sits well inside this, so
   the tessellator's clip test never triggers and the whole-run fast path is taken. */
#define GUI_TEXT_NO_CLIP 1e30f

/* One semantic draw command.  The 4-byte header carries the command type, the index of the active
   scissor rect in the per-frame clip table (assigned at clip-push time -- no per-emit search), and
   the target viewport.  z lives in gui_cmd_seg_t (per-segment, constant within a window) and is not
   repeated here.  Reducing the header from 28 bytes to 4 bytes brings the struct from 72 -> 48 bytes.
   tex_idx == 0 in rect means solid color (white texel).
   rounding (rect / rect_outline) is the corner radius baked from the ambient draw rounding at emit
   time, already clamped to the rect; 0 tessellates as a plain square shape.
   text.off is a byte offset into the frame's text pool (s_draw.text_pool), not a pointer: the
   string lives in the pool until the next frame_begin, so the command is valid through flush.
   Storing an offset instead of a const char* keeps the union at 4-byte alignment. */
typedef struct
{
    u8 type;       // gui_cmd_type_t, fits u8 (9 values)
    u8 clip_idx;   // index into per-frame s_draw.clip_table (set at push time)
    u8 vp;         // target viewport (GUI_MAX_VIEWPORTS = 4, fits u8)
    u8 _pad;
    union
    {
        struct { f32 x, y, w, h, u0, v0, u1, v1; f32 rounding; u32 tex_idx; u32 abgr; } rect;
        struct { f32 x, y, w, h, t;              f32 rounding;              u32 abgr; } rect_outline;
        struct { f32 ax, ay, bx, by, cx, cy;                     u32 abgr; } tri;
        /* clip_x0/clip_x1 are the horizontal pixel window for glyph-level clipping: the first and
           last straddling glyphs are cut and their U remapped; interior glyphs emit whole.  The
           sentinel (clip_x0 = -GUI_TEXT_NO_CLIP, clip_x1 = +GUI_TEXT_NO_CLIP) means unclipped
           and takes the original whole-run fast path. */
        struct { f32 x, y;  u32 off; u32 len;  f32 clip_x0, clip_x1;  u32 abgr; } text;
        struct { f32 cx, cy, r; u32 segs;                        u32 abgr; } circle;
        struct { f32 x0, y0, x1, y1, thickness;                  u32 abgr; } line;
        struct { u32 pt_offset; u32 pt_count; f32 thickness;
                 gui_stroke_align_t align; bool closed;         u32 abgr; } polyline;
        /* Dashed line tessellates to one oriented textured quad: U spans 0..len/period so the
           atlas dash row tiles along the line; duty (on-fraction) picks the nearest baked row. */
        struct { f32 x0, y0, x1, y1, thickness, period, duty;     u32 abgr; } dash;
        /* Gradient rect: one quad with col_a/col_b on opposite edges; the GPU interpolates the
           per-vertex color across it.  horizontal = left->right, else top->bottom.  Always square. */
        struct { f32 x, y, w, h; u32 col_a, col_b; bool horizontal; } gradient;
    };
} gui_cmd_t;

/*==============================================================================================
    Limits
==============================================================================================*/

/* 16K verts is far above what a debug UI emits per frame, and keeps vertex indices
   well within u16 range (64K would sit right at the 65535 ceiling).  The per-frame
   region sizes that fall out of these (VB 320 KB, IB 96 KB) are both 256-byte
   aligned, so each frame-in-flight region stays independently addressable -- note
   that this only matters if the VB/IB are ever moved off HOST_COHERENT memory, in
   which case regions would need rounding up to nonCoherentAtomSize to flush apart. */

#define GUI_MAX_VERTS      ( 16 * 1024 )
#define GUI_MAX_IDX        ( GUI_MAX_VERTS * 3 )
#define GUI_MAX_CMDS       1024

/* Command segments: one contiguous span of the command list per (z, vp) the emit path stamps, cut
   wherever draw_set_sort_key / draw_set_viewport change the tag.  The render backend orders these
   spans instead of re-scanning the whole command buffer.  Worst case each command sits in its own
   segment, plus the open one, so the cap is the command cap + 1. */

#define GUI_MAX_SEGS       ( GUI_MAX_CMDS + 1 )
#define GUI_MAX_PATH_PTS   8192                 /* per-frame total polyline/path point pool */
#define GUI_MAX_TEXT_POOL  ( 16 * 1024 )        /* per-frame flat string copy pool for text cmds */
#define GUI_CLIP_DEPTH     32
#define GUI_MAX_CLIP_RECTS 64                   /* per-frame clip table entries; u8 index so max is 256 */

/*==============================================================================================
    GPU resource memory usage (bytes), reported by gui()->mem_stats().
==============================================================================================*/

typedef struct
{
    u32 vertex_bytes;   // vertex buffer -- all frames-in-flight regions */
    u32 index_bytes;    // index buffer  -- all frames-in-flight regions */
    u32 texture_bytes;  // font atlases + 1x1 white pixel                */
    u32 total_bytes;    // sum of the above                              */

} gui_mem_stats_t;

/*==============================================================================================
    Per-frame render statistics, reported by gui()->render_stats().

    A direct read on render density: the geometry the last completed frame tessellated and how
    many GPU indexed draw calls (batches) it cost to paint it across every surface.  Published at
    frame_begin, so a read during the build returns the PREVIOUS frame's totals -- the standard
    one-frame-lag metric (the build that reads it is also the one being measured).
==============================================================================================*/

typedef struct
{
    u32 cmd_count;      // semantic draw commands the UI emitted                        
    u32 vert_count;     // tessellated vertices (total, including retained)             
    u32 tri_count;      // tessellated triangles (total, including retained)            
    u32 draw_calls;     // GPU indexed draw calls (batches), summed over surfaces       

    u32 win_total;      // windows tracked this frame                                  
    u32 win_retained;   // windows whose geometry was reused (no re-tessellation)      
    u32 vert_retained;  // vertices that came from prev-frame copy, not re-tessellated 
    u32 tri_retained;   // triangles retained from prev-frame copy                     

    u32 upload_batches; // number of buffer write calls per frame                      
    u32 upload_bytes;   // total bytes uploaded to GPU vertex and index buffers

} gui_render_stats_t;

/*==============================================================================================
    Font selection

    gui_font_t selects which built-in bitmap atlas to use.
    The TrueType path is activated separately via gui()->font_load(path).

    The number of the pixel height (not font size in .ttf)
==============================================================================================*/

typedef enum
{
    GUI_FONT_BITMAP_8 = 0,        // 8x8   pixel glyphs -- compact, pixel-perfect at native size
    GUI_FONT_BITMAP_16,           // 16x16 pixel glyphs -- 2x larger version of 8x8
    GUI_FONT_BITMAP_16_JETBOLD,   // 10x16 pixel glyphs
    GUI_FONT_BITMAP_20_JETBOLD,   // 12x20 pixel glyphs
    GUI_FONT_BITMAP_24_JETBOLD,   // 14x33 pixel glyphs
    GUI_FONT_BITMAP_24_CONSOLA,   // 13x25 pixel glyphs    
    GUI_FONT_BITMAP_MAX,          // number of built-in bitmap fonts

} gui_font_t;

/*==============================================================================================
    Table support

    begin_table / end_table open a multi-column layout with independent cell clipping.
    Use table_setup_column before any row to name and size columns, then iterate with
    table_next_row + table_next_column.  See gui_api.h for the full ergonomic contract.

    Column count limit is GUI_TABLE_COLS_MAX.  Column sizes use the same overloaded f32 as
    the layout engine: >1 = fixed pixels, 1 = stretch / fill, (0,1) = fraction.
==============================================================================================*/

#define GUI_TABLE_COLS_MAX 16

typedef enum
{
    GUI_TABLE_NONE            = 0,
    GUI_TABLE_BORDERS_H       = 1 << 0,   // horizontal row dividers (between rows)         
    GUI_TABLE_BORDERS_V       = 1 << 1,   // vertical column dividers (between columns)     
    GUI_TABLE_BORDERS_OUTER   = 1 << 2,   // outer frame border around the whole table      
    GUI_TABLE_BORDERS         = GUI_TABLE_BORDERS_H | GUI_TABLE_BORDERS_V | GUI_TABLE_BORDERS_OUTER,
    GUI_TABLE_SCROLL_Y        = 1 << 3,   // table body scrolls vertically                  
    GUI_TABLE_SCROLL_X        = 1 << 4,   // table body scrolls horizontally                
    GUI_TABLE_SORTABLE        = 1 << 5,   // clicking a header column header sorts          
    GUI_TABLE_ROW_STRIPES     = 1 << 6,   // alternating even/odd row background tint       
    GUI_TABLE_RESIZABLE       = 1 << 7,   // drag column borders to resize                  
    GUI_TABLE_NO_HEADER       = 1 << 8,   // skip table_headers_row entirely                

} gui_table_flags_t;

typedef enum
{
    GUI_TABLE_COL_NONE         = 0,
    GUI_TABLE_COL_FIXED        = 1 << 0,  // fixed pixel width -- does not stretch          
    GUI_TABLE_COL_STRETCH      = 1 << 1,  // fill remaining space (default when width==0)   
    GUI_TABLE_COL_NO_RESIZE    = 1 << 2,  // pins this column's right boundary (no drag)    
    GUI_TABLE_COL_NO_SORT      = 1 << 3,  // not clickable for sort                         
    GUI_TABLE_COL_ALIGN_RIGHT  = 1 << 4,  // right-align cell content (future phase)        
    GUI_TABLE_COL_ALIGN_CENTER = 1 << 5,  // center cell content (future phase)             

} gui_table_col_flags_t;

/* Background color override target for table_set_bg_color (future phase). */
typedef enum
{
    GUI_TABLE_BG_NONE = 0,
    GUI_TABLE_BG_ROW,     // tint the current entire row    
    GUI_TABLE_BG_CELL,    // tint the current cell only     

} gui_table_bg_target_t;

/* Sort specification returned by table_get_sort_specs (future phase). */
typedef struct
{
    i32  col;          // sorted column index; -1 = unsorted
    bool descending;   // false = ascending                  

} gui_table_sort_specs_t;

/* Sort key for one cell, filled by the value callback below.  Set num + is_num for a numeric
   compare; otherwise set str for an alphabetical (strcmp) compare.  A row that leaves both unset
   sorts as an empty string / zero. */
typedef struct
{
    const char* str;      // alphabetical key (used when is_num is false)
    f64         num;      // numeric key (used when is_num is true)       
    bool        is_num;   // true = compare num; false = compare str      

} gui_table_sort_value_t;

/* Built-in sort: supply the sort key for one cell.  row is the user data index, col the column
   being sorted.  Let the table handle alphabetical / numeric ordering and the sort direction. */
typedef void ( *gui_table_sort_value_fn )( i32 row, i32 col, gui_table_sort_value_t* out,
                                             void* user );

/* Custom sort: full-control comparator -- return <0 / 0 / >0 like strcmp.  a and b are user data
   indices, col the sorted column, descending the requested direction (apply or ignore it as you
   wish; the table does NOT negate the result for you). */
typedef i32 ( *gui_table_sort_cmp_fn )( i32 a, i32 b, i32 col, bool descending, void* user );

// clang-format on
/*============================================================================================*/
#endif    // GUI_H
