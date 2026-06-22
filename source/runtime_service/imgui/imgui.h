#ifndef IMGUI_H
#define IMGUI_H
/*==============================================================================================

    runtime_service/imgui/imgui.h -- imgui module types.

    Include in DLL modules that use imgui through the vtable (imgui()->...).
    Include imgui_host.h instead for direct-call access (host, sandbox).

==============================================================================================*/

#include "orb.h"

// clang-format off
/*==============================================================================================
    IMGUI: ID
==============================================================================================*/

/* Widget id -- a hashed value creates a unique value to identify a widget */

typedef u32 imgui_id_t;
#define IMGUI_ID_NONE 0u

/* Opaque viewport handle -- a render surface backed by an OS window.  Returned by
   viewport_open; passed to render, viewport_resize, viewport_close, and
   set_next_window_viewport.  IMGUI_VP_INVALID (-1) signals failure or no assignment. */

typedef i32  imgui_vp_t;
#define IMGUI_VP_INVALID  (-1)

/* Opaque dock-node handle -- one region of a viewport's dock tree.  Returned by dockspace_over_viewport
   (the tree root) and dock_split (the new sibling), and passed to dock_split / dock_window to name a
   target region.  0 (IMGUI_DOCK_NONE) signals "no node" -- a failed call or an unassigned slot. */

typedef u32  imgui_dock_id_t;
#define IMGUI_DOCK_NONE  0u

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

} imgui_ctx_config_t;

/* Pre-built configs. */
#define IMGUI_CTX_CONFIG_EDITOR  \
    ( ( imgui_ctx_config_t ){ 32, 512, 8, 4, 48 } )
#define IMGUI_CTX_CONFIG_GAME_UI \
    ( ( imgui_ctx_config_t ){ 8, 64, 4, 1, 0 } )

/* Opaque context handle -- integer index into the internal context pool.
   IMGUI_CTX_DEFAULT (0) is always valid after init().
   IMGUI_CTX_INVALID (-1) signals a failed ctx_create or an unset handle. */

typedef i32 imgui_ctx_t;
#define IMGUI_CTX_DEFAULT  0
#define IMGUI_CTX_INVALID  (-1)

/*==============================================================================================
    IMGUI: Geometry
==============================================================================================*/

typedef struct { f32 x, y; }        imgui_vec2_t;
typedef struct { f32 x, y, w, h; }  imgui_rect_t;

/* Callback fired by input_text_ex after any frame that modifies the buffer.
   buf is the live caller-owned buffer (may be read or written); len is the current byte
   length (excluding NUL); bufsz is the total buffer capacity. */
typedef void ( *imgui_text_cb_fn )( char* buf, u32 len, u32 bufsz, void* user );

/* Edge insets, in pixels.  Region padding -- the gap between a region's box and where its layout
   starts (see imgui_pad).  Breathing room *inside* a widget's frame is a per-widget style concern
   (WIDGET_PAD), not a layout one; spacing *between* cells is gap_x / gap_y. */

typedef struct { f32 l, r, t, b; }  imgui_pad_t;

/*----------------------------------------------------------------------------------------------
    Rect algebra -- pure helpers for custom-draw placement (canvas() regions).  Stateless, so they
    live inline with the geometry types they operate on.  The cut_* family is the "rectcut" idiom:
    each slices a strip off one edge of *r, shrinks *r to the remainder, and returns the slice --
    chain them to carve a canvas into label columns / content panes the way the row / column tracks
    carve a region, instead of hand-computing absolute offsets.

        imgui_rect_t bar    = imgui_rect_cut_top( &r, 24.0f );   // 24px strip off the top; r shrinks
        imgui_rect_t labels = imgui_rect_cut_left( &r, 80.0f );  // 80px label column; r is the rest
----------------------------------------------------------------------------------------------*/

/* Shrink r inward by per-edge insets. */
static inline imgui_rect_t
imgui_rect_inset( imgui_rect_t r, imgui_pad_t p )
{
    return ( imgui_rect_t ){ r.x + p.l, r.y + p.t, r.w - p.l - p.r, r.h - p.t - p.b };
}

/* Shrink r inward by the same margin on every edge (the common uniform-inset case). */
static inline imgui_rect_t
imgui_rect_pad( imgui_rect_t r, f32 a )
{
    return ( imgui_rect_t ){ r.x + a, r.y + a, r.w - 2.0f * a, r.h - 2.0f * a };
}

/* Center point of r. */
static inline imgui_vec2_t
imgui_rect_center( imgui_rect_t r )
{
    return ( imgui_vec2_t ){ r.x + r.w * 0.5f, r.y + r.h * 0.5f };
}

/* True when (x,y) lies in r -- left / top inclusive, right / bottom exclusive, so abutting rects
   partition the plane with no overlap (the pixel-coverage convention). */
static inline bool
imgui_rect_contains( imgui_rect_t r, f32 x, f32 y )
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* rectcut: slice `a` px off the left of *r, return the slice, leave *r as the remainder. */
static inline imgui_rect_t
imgui_rect_cut_left( imgui_rect_t* r, f32 a )
{
    if ( a > r->w ) a = r->w;
    imgui_rect_t cut = ( imgui_rect_t ){ r->x, r->y, a, r->h };
    r->x += a;
    r->w -= a;
    return cut;
}

/* rectcut: slice `a` px off the right of *r. */
static inline imgui_rect_t
imgui_rect_cut_right( imgui_rect_t* r, f32 a )
{
    if ( a > r->w ) a = r->w;
    r->w -= a;
    return ( imgui_rect_t ){ r->x + r->w, r->y, a, r->h };
}

/* rectcut: slice `a` px off the top of *r. */
static inline imgui_rect_t
imgui_rect_cut_top( imgui_rect_t* r, f32 a )
{
    if ( a > r->h ) a = r->h;
    imgui_rect_t cut = ( imgui_rect_t ){ r->x, r->y, r->w, a };
    r->y += a;
    r->h -= a;
    return cut;
}

/* rectcut: slice `a` px off the bottom of *r. */
static inline imgui_rect_t
imgui_rect_cut_bottom( imgui_rect_t* r, f32 a )
{
    if ( a > r->h ) a = r->h;
    r->h -= a;
    return ( imgui_rect_t ){ r->x, r->y + r->h, r->w, a };
}

/*==============================================================================================
    Layout template

    A region (a window body or a begin_child box) lays widgets out by carving its content area
    into cells.  imgui_layout() installs a template that *persists and repeats*: every widget
    fills the next cell.  A region opens UNDECLARED (no template): the first layout header in its
    body names the mode -- stack() for the single flex column of auto height (the classic vertical
    stack), or columns / grid / form for the others.  See imgui_layout_mode_t.

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
        <  0.0        IMGUI_END, the track-list terminator

    Gaps sit *between* cells and are subtracted before distribution, so a widget never sees or
    reasons about spacing -- it just fills the rect it is handed.
==============================================================================================*/

#define IMGUI_LAYOUT_COLS 8                     // max tracks on one axis (columns or rows)
#define IMGUI_END (-1.0f)                       // track-list terminator (any negative value)

/*----------------------------------------------------------------------------------------------
    Content alignment -- where a widget's natural-sized content sits inside the cell it is handed.

    Two independent axes, ORed together; 0 (LEFT | TOP) is the default and matches the original
    behavior.  A region carries one alignment (imgui()->align, or the `align` field of a layout
    descriptor), persisting like the row template until changed.  It governs *content* placement
    (a text label, an image) -- a widget whose frame fills the cell (button, input) still fills it,
    and only its label/glyphs follow the alignment.  rect_align() is the single placement seam.
----------------------------------------------------------------------------------------------*/

typedef enum
{
    IMGUI_ALIGN_LEFT    = 0,            /* horizontal: against the left edge (default)  */
    IMGUI_ALIGN_HCENTER = 1 << 0,       /* horizontal: centered                         */
    IMGUI_ALIGN_RIGHT   = 1 << 1,       /* horizontal: against the right edge           */

    IMGUI_ALIGN_TOP     = 0,            /* vertical: against the top edge (default)      */
    IMGUI_ALIGN_VCENTER = 1 << 2,       /* vertical: centered                            */
    IMGUI_ALIGN_BOTTOM  = 1 << 3,       /* vertical: against the bottom edge             */

    IMGUI_ALIGN_CENTER  = IMGUI_ALIGN_HCENTER | IMGUI_ALIGN_VCENTER,   /* both axes centered */

} imgui_align_t;

typedef struct
{
    f32             cols[ IMGUI_LAYOUT_COLS ];  // column tracks, IMGUI_END-terminated (see unit rule)
    f32             rows[ IMGUI_LAYOUT_COLS ];  // row tracks; empty/NULL => flow mode, else grid mode
    f32             row_h;                      // flow only -- row height: 0 = auto, >0 = pixels
    f32             gap_x, gap_y;               // inter-cell spacing; 0 = theme default
    imgui_align_t   align;                      // content alignment within each cell (0 = LEFT | TOP)

} imgui_layout_t;

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
    IMGUI_MODE_NONE = 0,    /* no header declared yet -- emitting a widget here is a usage error */
    IMGUI_MODE_STACK,       /* single flex column, rows accumulate + scroll (the vertical list)  */
    IMGUI_MODE_COLUMNS,     /* N pre-divided column tracks, rows accumulate + scroll             */
    IMGUI_MODE_GRID,        /* bounded cols x rows matrix, both axes fixed, nothing scrolls      */
    IMGUI_MODE_PACK,        /* natural-size print run, placed item-by-item along an axis (bar/strip) */

} imgui_layout_mode_t;

/*==============================================================================================
    Pack direction -- the axis a pack() run places items along, item-by-item at natural size.
    bar() is the horizontal pack (a toolbar); strip() is the vertical pack.
==============================================================================================*/

typedef enum
{
    IMGUI_PACK_HORIZONTAL = 0,    /* bar:   items flow left to right, nextline wraps down    */
    IMGUI_PACK_VERTICAL   = 1,    /* strip: items flow top to bottom, nextline wraps across  */

} imgui_pack_dir_t;

/*==============================================================================================
    Field label side -- where a labeled value widget (input_text / slider_float / checkbox) puts
    its label when a field split is active (imgui()->field_split / field_label_left).  The label and
    control are two tracks resolved across the widget's cell with the same overloaded unit as
    columns; `side` only decides which track sits on which edge.  NONE is the default: the label
    trails the control at its natural width on the right.
==============================================================================================*/

typedef enum
{
    IMGUI_LABEL_NONE  = 0,      /* off -- natural-width label trailing on the right (default) */
    IMGUI_LABEL_LEFT  = 1,      /* label track on the left, control fills the right */
    IMGUI_LABEL_RIGHT = 2,      /* label track on the right, control fills the left */

} imgui_label_side_t;

/*==============================================================================================
    Window drag mode -- how a window may be repositioned by the mouse.
    Selected globally via imgui()->set_window_drag(); default is TITLEBAR.
==============================================================================================*/

typedef enum
{
    IMGUI_WIN_DRAG_NONE     = 0,    /* windows are fixed in place                          */
    IMGUI_WIN_DRAG_TITLEBAR = 1,    /* drag only by the title bar (default)                */
    IMGUI_WIN_DRAG_BODY     = 2,    /* drag from anywhere in the window not over a widget  */

} imgui_win_drag_t;

/*==============================================================================================
    Apply condition -- when a queued set_next_window_* value takes effect on its target window.

    Passed to set_next_window_pos / set_next_window_size.  The value and the condition are two
    separate axes: the same window can be seeded once, forced every frame, or re-applied whenever
    it re-appears, by changing only the condition -- the reason geometry is a side channel rather
    than fixed begin_window parameters.  Bit values so a window can mask the conditions it still
    permits.  A 0 (unset) condition is treated as ALWAYS.
==============================================================================================*/

typedef enum
{
    IMGUI_COND_ONCE      = 1 << 0,   /* apply once for this window, then never again -- seeds the
                                        initial position/size on first appearance and never again
                                        (akin to Dear ImGui's FirstUseEver until a saved-layout lands) */
    IMGUI_COND_ALWAYS    = 1 << 1,   /* apply every frame the value is queued -- forced geometry for
                                        layout managers, snapping, animation; pair with NOMOVE /
                                        NORESIZE so a user drag does not fight it */
    IMGUI_COND_APPEARING = 1 << 2,   /* apply each time the window appears -- on creation and again
                                        whenever it is shown after a frame of absence (e.g. re-center
                                        a reopened popup / modal) */

} imgui_cond_t;

/*==============================================================================================
    Window flags

    Passed as the final argument to begin_window to customize a single window's behavior.
    They mostly switch off default behavior; pass 0 (IMGUI_WIN_NONE) for the defaults.
==============================================================================================*/

typedef enum
{
    IMGUI_WIN_NONE              = 0,         /* default behavior */
    IMGUI_WIN_NOTITLEBAR        = 1 << 0,    /* no title bar: body fills the top; no collapse, no titlebar drag */
    IMGUI_WIN_NOCOLLAPSE        = 1 << 1,    /* no collapse arrow; the window stays expanded */
    IMGUI_WIN_NORESIZE          = 1 << 2,    /* disable user resizing from the border edges */
    IMGUI_WIN_NOMOVE            = 1 << 3,    /* disable user drag moving the window from anywhere */

    /* scrollbars -- dynamic: vertical bar and mouse wheel input are enabled by default */

    IMGUI_WIN_NOSCROLL          = 1 << 4,    /* disable all scroll bars (keep mouse input) */
    IMGUI_WIN_HSCROLL           = 1 << 5,    /* enable dynamic horizontal scroll bar (off by default) */
    IMGUI_WIN_NOMOUSESCROLL     = 1 << 6,    /* disable mouse wheel scrolling */
    
    /* scrollbars - static: override dynamic bar flags */

    IMGUI_WIN_ALWAYS_VSCROLL    = 1 << 7,    /* always show vertical scroll bar -- override*/
    IMGUI_WIN_ALWAYS_HSCROLL    = 1 << 8,    /* always show horizontal scroll bar -- override */
    
    /* auto-resize -- size the window to its content instead of a fixed w/h */

    IMGUI_WIN_ALWAYS_AUTOSIZE   = 1 << 9,    /* hug content every frame: no user resize, no scrollbars */
    IMGUI_WIN_CAN_AUTOSIZE      = 1 << 10,   /* show a corner size-grip; double-click it to fit content */

    /* child resize -- begin_child only (the ImGuiChildFlags_ResizeX / _ResizeY analogue).  A
       draggable grip on the child's right / bottom border; the size on that axis then becomes
       user-owned and persisted -- seeded once from the begin_child w/h, thereafter set by the
       drag -- overriding the passed value.  RESIZE_Y supersedes the h<=0 auto-size on that axis.
       A real window ignores these (it owns its geometry already), as does a grid-cell child
       (the cell sizes it).  Vertical is the common case; both axes may be combined. */

    IMGUI_WIN_CHILD_RESIZE_X    = 1 << 11,   /* child: drag the right border to resize width   */
    IMGUI_WIN_CHILD_RESIZE_Y    = 1 << 12,   /* child: drag the bottom border to resize height  */

    /* Menu bar -- reserve a one-row strip below the title bar that begin_menu_bar fills (the
       ImGuiWindowFlags_MenuBar analogue).  The strip is carved from the top of the body before the
       body scroll region opens, so it never scrolls; begin_menu_bar returns false unless set. */

    IMGUI_WIN_MENUBAR           = 1 << 13,   /* reserve a non-scrolling menu-bar strip (begin_menu_bar) */

    /* Native-borderless: this window IS its host OS window (window kind 3).  Its titlebar stands in
       for the Win32 caption and its border for the sizing frame, so titlebar drag / double-click /
       right-click and border drags are routed to native OS window actions (app()->window_start_move
       / window_title_event / window_system_menu / window_start_resize) instead of imgui's in-client
       move, tear-off, collapse, and edge-resize.  The host OS window must have been opened with
       APP_WIN_BORDERLESS.  Geometry is owned by the OS window (size follows WM_SIZE). */

    IMGUI_WIN_NATIVE            = 1 << 14,

    /* Title-bar capability -- subtract a caption control a window should not offer.  All three
       buttons show by default (opt-out, like the NO* flags above); a window drops the ones it does
       not support and the title bar reflects that, reclaiming the freed space for the title text.

       NO_MINIMIZE / NO_MAXIMIZE gate the OS minimize / maximize caption buttons and so apply only to
       a native window (IMGUI_WIN_NATIVE or a detached floater) -- a non-native panel has no such OS
       state and never drew them.  The close (main) / pop-in (floater) primary button is never
       suppressed: close is essential and pop-in is a floater's only route back to the main surface.

       NO_DETACH removes the pop-out path for any window -- it hides the non-native detach button and
       blocks the drag tear-off -- independent of NOMOVE (a window may move yet refuse to pop out). */

    IMGUI_WIN_NO_MINIMIZE       = 1 << 15,   /* native: no minimize caption button */
    IMGUI_WIN_NO_MAXIMIZE       = 1 << 16,   /* native: no maximize / restore caption button */
    IMGUI_WIN_NO_DETACH         = 1 << 17,   /* no pop-out: hide detach button, block tear-off drag */

    /* Placement is managed externally (docking layout, animation, scripted snap).  Bypasses both
       the per-drag margin clamp (window_clamp) and the merge-back fit-inside clamp so the system
       can position and size the window freely without imgui fighting the placement.  Without this
       flag both clamps apply unconditionally; with it neither does. */

    IMGUI_WIN_NO_BOUNDARY_CLAMP = 1 << 18,

} imgui_win_flags_t;

/*==============================================================================================
    Dockspace flags

    Passed to dockspace_over_viewport.  0 (IMGUI_DOCKSPACE_NONE) is the default dockspace that fills
    the viewport behind the free-floating windows.  Bit values so future policy bits (e.g. hide the
    single-tab strip, no central-node auto-hide) can be ORed in without changing the call sites.
==============================================================================================*/

typedef enum
{
    IMGUI_DOCKSPACE_NONE = 0,    /* default: fill the viewport, draw splitters + tab bars */

} imgui_dockspace_flags_t;

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
    can be combined; 0 (IMGUI_ITEM_NONE) is the default no-op set.
==============================================================================================*/

typedef enum
{
    /* no tweaks -- the default behavior */
    IMGUI_ITEM_NONE          = 0,       

    /* inert + dimmed: no hover/active/focus/click, drawn at
       reduced opacity.  Honored uniformly by widget_behavior and
       the draw list, so it applies to every widget at once. */
    IMGUI_ITEM_DISABLED      = 1 << 0,  

    /* a held button fires repeatedly: once on press, then after
       an initial delay at a steady rate (spinner / scroll arrows),
       instead of once on release.  Honored by widget_behavior, so
       any button-kind widget under the flag auto-repeats. */
    IMGUI_ITEM_BUTTON_REPEAT = 1 << 1,  

    /* slider_float: suppress the value text drawn centered on the
       track.  The value is shown by default; set this (push or
       next_item_flag) to hide it for a bare / compact slider. */
    IMGUI_ITEM_NO_VALUE_TEXT = 1 << 2,  

    /* Room to grow without disturbing call sites or the vtable -- e.g. a future
    IMGUI_ITEM_READ_ONLY (editable widgets show but reject input), IMGUI_ITEM_NO_NAV, etc. */

} imgui_item_flags_t;

/*==============================================================================================
    Direction -- a cardinal direction, the ImGuiDir analogue.  Passed to arrow_button (and any
    future directional widget) to pick which way the glyph points.
==============================================================================================*/

typedef enum
{
    IMGUI_DIR_LEFT,
    IMGUI_DIR_RIGHT,
    IMGUI_DIR_UP,
    IMGUI_DIR_DOWN,

} imgui_dir_t;

/*==============================================================================================
    Combo flags

    Passed to begin_combo to tune the dropdown.  The HEIGHT_* group caps the dropdown to a fixed
    number of visible rows (then it scrolls) -- the ImGuiComboFlags_Height* analogue; they are
    mutually exclusive, so set exactly one (an unset height defaults to REGULAR / 8 rows).  0
    (IMGUI_COMBO_NONE) is the default no-tweak set.
==============================================================================================*/

typedef enum
{
    IMGUI_COMBO_NONE            = 0,         /* default behavior (REGULAR height) */

    IMGUI_COMBO_HEIGHT_SMALL    = 1 << 0,    /* cap the dropdown to ~4 rows, then scroll   */
    IMGUI_COMBO_HEIGHT_REGULAR  = 1 << 1,    /* cap to ~8 rows (the default), then scroll   */
    IMGUI_COMBO_HEIGHT_LARGE    = 1 << 2,    /* cap to ~20 rows, then scroll                */
    IMGUI_COMBO_HEIGHT_LARGEST  = 1 << 3,    /* no cap: as many rows as fit on screen       */

    /* Mask of the height bits, to clear the group before setting one (the demo idiom). */
    IMGUI_COMBO_HEIGHT_MASK     = IMGUI_COMBO_HEIGHT_SMALL | IMGUI_COMBO_HEIGHT_REGULAR
                                | IMGUI_COMBO_HEIGHT_LARGE | IMGUI_COMBO_HEIGHT_LARGEST,

} imgui_combo_flags_t;

/*==============================================================================================
    Style colors

    The themeable color slots, the ImGuiCol_ analogue.  Each names one entry of the shared palette
    the widgets draw from; push_style_color( slot, abgr ) overrides it for every widget until the
    matching pop_style_color, next_style_color overrides it for just the next widget, and a slot
    left unpushed uses the theme default.  Colors are packed with IMGUI_COLOR (byte order R,G,B,A).

    The palette is shared rather than per-widget-type (one IMGUI_COL_WIDGET_BG, not Button +
    Checkbox + ...), matching the engine's single-palette theme: to recolor one button, bracket it
    with push/pop (only that button draws between them), or use next_style_color for a one-shot.
==============================================================================================*/

typedef enum
{
    IMGUI_COL_TEXT,           /* label / glyph text                          */
    IMGUI_COL_TEXT_DIM,       /* secondary text (trailing labels)            */
    IMGUI_COL_WINDOW_BG,      /* window body background                      */
    IMGUI_COL_CHILD_BG,       /* child region background                     */
    IMGUI_COL_TITLE_BG,       /* window title bar                            */
    IMGUI_COL_BORDER,         /* window / widget outlines                    */
    IMGUI_COL_WIDGET_BG,      /* idle widget body (button, checkbox, knob)   */
    IMGUI_COL_WIDGET_HOT,     /* hovered widget body                         */
    IMGUI_COL_WIDGET_ACT,     /* pressed / active widget body                */
    IMGUI_COL_WIDGET_FG,      /* widget foreground accent (slider fill)      */
    IMGUI_COL_CHECK_MARK,     /* checkbox tick / radio dot                   */
    IMGUI_COL_SLIDER_TRACK,   /* slider + scrollbar track                    */
    IMGUI_COL_RESIZE_HOT,     /* hot resize edge / size grip                 */
    IMGUI_COL_INPUT_BG,       /* text input field background                 */
    IMGUI_COL_INPUT_FOCUS,    /* focused text input field background         */
    IMGUI_COL_CURSOR,         /* text input caret                           */
    IMGUI_COL_NAV_HIGHLIGHT,  /* keyboard-nav focus ring around the nav item */

    IMGUI_COL_COUNT,          /* slot count -- not a color                   */

} imgui_col_t;

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
    IMGUI_VAR_LINE_SIZE,      /* widget row height (the frame height)        */
    IMGUI_VAR_WIDGET_GAP,     /* gap between consecutive widgets / cells     */
    IMGUI_VAR_WIDGET_PAD,     /* content padding inside a frame (FramePadding)*/
    IMGUI_VAR_WIN_TITLE_H,    /* window title bar height                     */
    IMGUI_VAR_WIN_BORDER,     /* window / widget outline thickness           */
    IMGUI_VAR_CHECKBOX_SZ,    /* checkbox / radio indicator side             */
    IMGUI_VAR_SLIDER_KNOB_W,  /* slider knob + scrollbar thickness           */
    IMGUI_VAR_MIN_CELL_W,     /* min width a flex cell shrinks to            */
    IMGUI_VAR_WIN_ROUNDING,   /* corner radius for windows / children / popups; 0 = square */
    IMGUI_VAR_WIDGET_ROUNDING,/* corner radius for control frames (button/checkbox/input/...) */
    IMGUI_VAR_GRAB_ROUNDING,  /* corner radius for slider knobs + scrollbar grabs */

    IMGUI_VAR_COUNT,          /* var count -- not a metric                   */

} imgui_style_var_t;

/*==============================================================================================
    Debug overlay layers

    Bitmask passed to imgui()->debug_set_layers().  Each bit enables one bolt-on debug
    visualization, emitted into a separate draw list and painted last, on top of the UI.
    The overlay is compiled in for Debug builds only (IMGUI_DEBUG_OVERLAY); in a Release
    build set_layers is a no-op and get_layers returns 0.  These constants stay defined in
    every build so call sites compile unchanged.
==============================================================================================*/

typedef enum
{
    IMGUI_DBG_NONE     = 0,         /* overlay off                                          */
    IMGUI_DBG_WINDOW   = 1 << 0,    /* window outer frames; the hover window stands out     */
    IMGUI_DBG_INTERACT = 1 << 1,    /* per-widget interaction rects (hover/active tinted)   */
    IMGUI_DBG_RESIZE   = 1 << 2,    /* window edge-resize grab bands; hot when armed        */
    IMGUI_DBG_CLIP     = 1 << 3,    /* clip (scissor) rectangle stack, colored by depth     */

    IMGUI_DBG_ALL      = IMGUI_DBG_WINDOW | IMGUI_DBG_INTERACT | IMGUI_DBG_RESIZE | IMGUI_DBG_CLIP,

} imgui_dbg_layer_t;

/*==============================================================================================
    Color packing

    IMGUI_COLOR(r,g,b,a) packs 0-255 byte values into a u32 such that memory byte order
    is [R, G, B, A], matching VK_FORMAT_R8G8B8A8_UNORM vertex attribute layout.
==============================================================================================*/

#define IMGUI_COLOR( r, g, b, a ) \
    ( ( ( u32 )( a ) << 24 ) | ( ( u32 )( b ) << 16 ) | ( ( u32 )( g ) << 8 ) | ( u32 )( r ) )

/*==============================================================================================
    Line / path stroking

    Thickness, pixel-snapping, and where a stroke sits relative to the ideal path it is drawn from.
    Implementation in imgui_draw_path.c.

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
    IMGUI_STROKE_CENTER_BIASED = 0,   /* centered + snapped to the pixel grid (default) */
    IMGUI_STROKE_CENTER,              /* centered on the path, no snap                  */
    IMGUI_STROKE_INSIDE,              /* whole width on the interior side of a CW-screen ring */
    IMGUI_STROKE_OUTSIDE,             /* whole width on the exterior side of a CW-screen ring */

} imgui_stroke_align_t;

#define IMGUI_PATH_MAX 256            /* max points path_line_to accumulates before a stroke */

/*==============================================================================================
    Draw vertex  (20 bytes, single interleaved binding)

    Vertex attribute layout (matches the imgui pipeline):
        location 0 : float2  (x, y)       offset  0   -- pixel-space position
        location 1 : float2  (u, v)       offset  8   -- texture UV [0..1]
        location 2 : UNORM4  (abgr u32)   offset 16   -- packed color, R8G8B8A8_UNORM
==============================================================================================*/

typedef struct
{
    f32 x, y; /* pixel position */
    f32 u, v; /* texture UV     */
    u32 abgr; /* packed color   */

} imgui_draw_vert_t;

/*==============================================================================================
    Semantic draw commands

    The UI build pass emits one imgui_cmd_t per visible shape into a list.  The render backend
    (imgui_render.c) tessellates each command into vertices and indices at flush time.  This
    separates the UI logic from any graphics API knowledge.

    GPU draw commands (imgui_gpu_cmd_t) are a backend-private type defined in imgui_draw.c;
    they carry index ranges and bind state for one GPU draw call.
==============================================================================================*/

typedef enum
{
    IMGUI_CMD_RECT_FILLED,     /* filled rectangle or textured quad (glyph) */
    IMGUI_CMD_RECT_OUTLINE,    /* hollow rectangle: four edge quads          */
    IMGUI_CMD_TRIANGLE,        /* solid triangle                             */
    IMGUI_CMD_TEXT,            /* glyph run from the font atlas              */
    IMGUI_CMD_CIRCLE_FILLED,   /* filled disc (triangle fan)                 */
    IMGUI_CMD_LINE,            /* single stroke segment                      */
    IMGUI_CMD_POLYLINE,        /* multi-segment antialiased polyline         */

} imgui_cmd_type_t;

/* One semantic draw command.  clip, z, and vp are baked from the draw state at emit time.
   The union carries the shape parameters; tex_idx == 0 in rect means solid color (white texel).
   rounding (rect / rect_outline) is the corner radius baked from the ambient draw rounding at emit
   time, already clamped to the rect; 0 tessellates as a plain square shape.
   text.str must remain valid until imgui_render_flush is called (same-frame lifetime). */
typedef struct
{
    imgui_cmd_type_t type;      /* which shape to tessellate            */
    imgui_rect_t     clip;      /* scissor rect, baked at emit time     */
    u32              z;         /* sort key, baked at emit time         */
    u32              vp;        /* target viewport index, baked at emit */
    union
    {
        struct { f32 x, y, w, h, u0, v0, u1, v1; f32 rounding; u32 tex_idx; u32 abgr; } rect;
        struct { f32 x, y, w, h, t;              f32 rounding;             u32 abgr; } rect_outline;
        struct { f32 ax, ay, bx, by, cx, cy;                     u32 abgr; } tri;
        struct { f32 x, y;  const char* str; u32 len;            u32 abgr; } text;
        struct { f32 cx, cy, r; u32 segs;                        u32 abgr; } circle;
        struct { f32 x0, y0, x1, y1, thickness;                  u32 abgr; } line;
        struct { u32 pt_offset; u32 pt_count; f32 thickness;
                 imgui_stroke_align_t align; bool closed;         u32 abgr; } polyline;
    };
} imgui_cmd_t;

/*==============================================================================================
    Limits
==============================================================================================*/

/* 16K verts is far above what a debug UI emits per frame, and keeps vertex indices
   well within u16 range (64K would sit right at the 65535 ceiling).  The per-frame
   region sizes that fall out of these (VB 320 KB, IB 96 KB) are both 256-byte
   aligned, so each frame-in-flight region stays independently addressable -- note
   that this only matters if the VB/IB are ever moved off HOST_COHERENT memory, in
   which case regions would need rounding up to nonCoherentAtomSize to flush apart. */

#define IMGUI_MAX_VERTS      ( 16 * 1024 )
#define IMGUI_MAX_IDX        ( IMGUI_MAX_VERTS * 3 )
#define IMGUI_MAX_CMDS       1024
#define IMGUI_MAX_PATH_PTS   8192        /* per-frame total polyline/path point pool */
#define IMGUI_MAX_TEXT_POOL  ( 16 * 1024 ) /* per-frame flat string copy pool for text cmds */
#define IMGUI_CLIP_DEPTH     32

/*==============================================================================================
    GPU resource memory usage (bytes), reported by imgui()->mem_stats().
==============================================================================================*/

typedef struct
{
    u32 vertex_bytes;   /* vertex buffer -- all frames-in-flight regions */
    u32 index_bytes;    /* index buffer  -- all frames-in-flight regions */
    u32 texture_bytes;  /* font atlases + 1x1 white pixel                */
    u32 total_bytes;    /* sum of the above                              */

} imgui_mem_stats_t;

/*==============================================================================================
    Font selection

    imgui_font_t selects which built-in bitmap atlas to use.
    The TrueType path is activated separately via imgui()->load_font(path).

    The number of the pixel height (not font size in .ttf)
==============================================================================================*/

typedef enum
{
    IMGUI_FONT_BITMAP_8 = 0,        /* 8x8   pixel glyphs -- compact, pixel-perfect at native size */
    IMGUI_FONT_BITMAP_16,           /* 16x16 pixel glyphs -- 2x larger version of 8x8 */
    IMGUI_FONT_BITMAP_12,           /* 8x12  pixel glyphs -- default, pixel-perfect at native size */
    IMGUI_FONT_BITMAP_16_PROGGY,    /* 9x16  tiny font */
    IMGUI_FONT_BITMAP_20_PROGGY,    /* 12x20 tiny font */
    IMGUI_FONT_BITMAP_16_JETBOLD,   /* 10x16 pixel glyphs */
    IMGUI_FONT_BITMAP_20_JETBOLD,   /* 12x20 pixel glyphs */
    IMGUI_FONT_BITMAP_24_JETBOLD,   /* 14x33 pixel glyphs */
    IMGUI_FONT_BITMAP_24_CONSOLA,   /* 13x25 pixel glyphs */
    
    IMGUI_FONT_BITMAP_MAX,          /* number of built-in bitmap fonts; */

} imgui_font_t;

/*==============================================================================================
    Table support

    begin_table / end_table open a multi-column layout with independent cell clipping.
    Use table_setup_column before any row to name and size columns, then iterate with
    table_next_row + table_next_column.  See imgui_api.h for the full ergonomic contract.

    Column count limit is IMGUI_TABLE_COLS_MAX.  Column sizes use the same overloaded f32 as
    the layout engine: >1 = fixed pixels, 1 = stretch / fill, (0,1) = fraction.
==============================================================================================*/

#define IMGUI_TABLE_COLS_MAX 16

typedef u32 imgui_table_flags_t;
typedef enum
{
    IMGUI_TABLE_NONE            = 0,
    IMGUI_TABLE_BORDERS_H       = 1 << 0,   /* horizontal row dividers (between rows)          */
    IMGUI_TABLE_BORDERS_V       = 1 << 1,   /* vertical column dividers (between columns)      */
    IMGUI_TABLE_BORDERS_OUTER   = 1 << 2,   /* outer frame border around the whole table       */
    IMGUI_TABLE_BORDERS         = IMGUI_TABLE_BORDERS_H | IMGUI_TABLE_BORDERS_V | IMGUI_TABLE_BORDERS_OUTER,
    IMGUI_TABLE_SCROLL_Y        = 1 << 3,   /* table body scrolls vertically                  */
    IMGUI_TABLE_SCROLL_X        = 1 << 4,   /* table body scrolls horizontally                */
    IMGUI_TABLE_SORTABLE        = 1 << 5,   /* clicking a header column header sorts          */
    IMGUI_TABLE_ROW_STRIPES     = 1 << 6,   /* alternating even/odd row background tint       */
    IMGUI_TABLE_RESIZABLE       = 1 << 7,   /* drag column borders to resize                  */
    IMGUI_TABLE_NO_HEADER       = 1 << 8,   /* skip table_headers_row entirely                */

} imgui_table_flags_e;

typedef u32 imgui_table_col_flags_t;
typedef enum
{
    IMGUI_TABLE_COL_NONE         = 0,
    IMGUI_TABLE_COL_FIXED        = 1 << 0,  /* fixed pixel width -- does not stretch          */
    IMGUI_TABLE_COL_STRETCH      = 1 << 1,  /* fill remaining space (default when width==0)   */
    IMGUI_TABLE_COL_NO_RESIZE    = 1 << 2,  /* pins this column's right boundary (no drag)    */
    IMGUI_TABLE_COL_NO_SORT      = 1 << 3,  /* not clickable for sort                         */
    IMGUI_TABLE_COL_ALIGN_RIGHT  = 1 << 4,  /* right-align cell content (future phase)        */
    IMGUI_TABLE_COL_ALIGN_CENTER = 1 << 5,  /* center cell content (future phase)             */

} imgui_table_col_flags_e;

/* Background color override target for table_set_bg_color (future phase). */
typedef enum
{
    IMGUI_TABLE_BG_NONE = 0,
    IMGUI_TABLE_BG_ROW,     /* tint the current entire row    */
    IMGUI_TABLE_BG_CELL,    /* tint the current cell only     */

} imgui_table_bg_target_t;

/* Sort specification returned by table_get_sort_specs (future phase). */
typedef struct
{
    i32  col;          /* sorted column index; -1 = unsorted */
    bool descending;   /* false = ascending                  */

} imgui_table_sort_specs_t;

/* Sort key for one cell, filled by the value callback below.  Set num + is_num for a numeric
   compare; otherwise set str for an alphabetical (strcmp) compare.  A row that leaves both unset
   sorts as an empty string / zero. */
typedef struct
{
    const char* str;      /* alphabetical key (used when is_num is false) */
    f64         num;      /* numeric key (used when is_num is true)       */
    bool        is_num;   /* true = compare num; false = compare str      */

} imgui_table_sort_value_t;

/* Built-in sort: supply the sort key for one cell.  row is the user data index, col the column
   being sorted.  Let the table handle alphabetical / numeric ordering and the sort direction. */
typedef void ( *imgui_table_sort_value_fn )( i32 row, i32 col, imgui_table_sort_value_t* out,
                                             void* user );

/* Custom sort: full-control comparator -- return <0 / 0 / >0 like strcmp.  a and b are user data
   indices, col the sorted column, descending the requested direction (apply or ignore it as you
   wish; the table does NOT negate the result for you). */
typedef i32 ( *imgui_table_sort_cmp_fn )( i32 a, i32 b, i32 col, bool descending, void* user );

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_H
