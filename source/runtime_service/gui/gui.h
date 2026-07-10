#ifndef GUI_H
#define GUI_H
/*==============================================================================================

    runtime_service/gui/gui.h -- gui module types (the public type header).

    In-house immediate-mode GUI for ORB.  No Dear ImGui, no GLFW/SDL: windowing/input come from
    the engine `app` layer (Win32), rendering goes through `rhi` (Vulkan).  The host drives a
    frame_begin -> ctx_begin/widgets/ctx_end -> frame_end -> render() lifecycle each frame.

    Read ARCHITECTURE.md (alongside this file) before chasing a bug across files -- it is the
    orientation map: the three state tiers (ambient-singular / per-context retained via g_ctx /
    frame-scratch), the two unity TUs (gui.c UI unit + gui_backend.c render unit), the
    EMIT -> BUILD -> RENDER pipeline, and the invariants.  Header split follows the house
    convention: this file (types) -> gui_api.h (DLL) -> gui_host.h (hosts/sandboxes).

    Two caches make an idle UI cheap (see ARCHITECTURE.md sec 6):
    1. CPU emit skip (s_frame_dirty, gui_frame.c): a single global bool.  When no input, animation,
       or render delta occurred, the whole emit phase is skipped and the previous frame's draw list
       is reused.
    2. GPU tessellation cache (gui_build_cache.c): granular per window.  A per-window hash mismatch
       re-tessellates only that window's slot; sibling windows reuse their geometry in place.

    Contents -- sections follow the source tree order (see gui.c's include list):

    foundation  -- ids, context config, geometry, style colors / config / themes / vars
    compose     -- rect algebra, layout template / alignment / modes, pack, split, field
    interact    -- item flags, drag and drop
    present     -- angle algebra, color packing, stroking, draw vertex, volatile cb,
                   semantic draw commands
    widgets     -- direction, color edit flags
    window      -- drag mode, apply condition, window flags
    dock        -- dockspace flags
    popup       -- combo flags
    table       -- table support
    frame       -- font config, capability flags, boot descriptor, limits, memory + render stats
    debug       -- overlay layers, render mode

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
   window_set_next_viewport.  GUI_VP_INVALID (UINT32_MAX) signals failure or no assignment. */

typedef u32 gui_vp_t;
#define GUI_VP_INVALID (~0u)

/* Opaque dock-node handle -- one region of a viewport's dock tree.  Returned by dockspace_over_viewport
   (the tree root) and dock_split (the new sibling), and passed to dock_split / dock_window to name a
   target region.  0 (GUI_DOCK_NONE) signals "no node" -- a failed call or an unassigned slot. */

typedef u32 gui_dock_id_t;
#define GUI_DOCK_NONE  0u

/* Opaque context handle -- integer index into the internal context pool.
   GUI_CTX_DEFAULT (0) is always valid after init().
   GUI_CTX_INVALID (-1) signals a failed ctx_create or an unset handle. */

typedef i32 gui_ctx_id_t;
#define GUI_CTX_DEFAULT  0
#define GUI_CTX_INVALID  (-1)

/*==============================================================================================
    GUI: Context Configuration

    Context configuration -- sizes the per-context resource pools at creation time.
    Pass to ctx_create(); NULL or zero fields default to the EDITOR preset (32 windows,
    512 state slots, 8 popup depth, 4 viewports, 48 dock nodes).
    max_dock_nodes == 0 is valid and disables docking for that context.
==============================================================================================*/

typedef struct
{
    u32  max_windows;    // persisted window pool (default 32)
    u32  state_slots;    // keyed state pool, must be power of two (default 512)
    u32  popup_depth;    // max popup nesting (default 8)
    u32  max_viewports;  // render surfaces (default 4)
    u32  max_dock_nodes; // dock-tree node pool; 0 = no docking (default 48)

} gui_ctx_config_t;

/* Pre-built configs -- scale context memory usage for how heavy the UI contents are */
#define GUI_CTX_CONFIG_EDITOR  \
    ( ( gui_ctx_config_t ){ 32, 512, 8, 4, 48 } )
#define GUI_CTX_CONFIG_GAME_UI \
    ( ( gui_ctx_config_t ){ 8, 64, 4, 1, 0 } )

/*==============================================================================================
    GUI: Geometry
==============================================================================================*/

typedef struct { f32 x, y; }        gui_vec2_t;
typedef struct { f32 x, y, w, h; }  gui_rect_t;

/* Edge insets, in pixels. Region padding -- the gap between a region's box and where its layout
   starts (see gui_pad).  Breathing room *inside* a widget's frame is a per-widget style concern
   (WIDGET_PAD), not a layout one; spacing *between* cells is gap_x / gap_y. */

typedef struct { f32 l, r, t, b; }  gui_pad_t;

/* Callback fired by input_text_ex after any frame that modifies the buffer.
   buf is the live caller-owned buffer (may be read or written); len is the current byte
   length (excluding NUL); bufsz is the total buffer capacity. */
typedef void ( *gui_text_cb_fn )( char* buf, u32 len, u32 bufsz, void* user );

/* Key hook consulted by the FOCUSED text field before its own key handling (see
   set_edit_key_hook).  Called once per key down this frame; key is an app_key_t value,
   repeat is true on OS auto-repeat ticks (false on the initial press).  Return true to
   consume: the key is cleared from the frame io, so neither the field nor any later widget
   acts on it -- the Quake-console passthrough (history, completion, scrollback keys). */
typedef bool ( *gui_edit_key_fn )( u32 key, bool ctrl, bool shift, bool repeat, void* user );

/* Monotonic wall-clock source (seconds), supplied by the host to the built-in perf overlay.
   gui has no timing service of its own (it is a leaf of rhi + app), so the host hands it a
   tick-seconds callback -- typically sys()->tick_seconds -- and gui uses it to measure the
   per-frame emit (build) and render (flush) cost the overlay reports.  See set_frame_hooks(). */
typedef f64 ( *gui_clock_fn )( void );

/* Host OS services for end-of-frame pacing (see set_frame_hooks / frame_pace).  gui links only
   app + rhi, so the sleep and the block-on-input wait are handed in as callbacks -- typically
   sys_sleep_milliseconds and sys_wait_for_os_events_ms.  A NULL member disables the feature that
   depends on it (no sleep -> frame_pace never sleeps; no wait -> idle skip unavailable). */
typedef void ( *gui_sleep_fn )( i32 milliseconds );
typedef void ( *gui_wait_events_fn )( i32 timeout_ms );

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

/* The scale ramp -- the named density steps the UI is authored in, instead of raw pixel sizes.
   Each step is a complete metric set (row height + pad + gap, authored in gui_style_t.scales),
   so "this region is a dense list" is one declaration and the pairing that makes a density look
   right lives in the theme.  scale_push/scale_pop scope a step over the style-var stack, which
   makes every metric read (WIDGET_H / pad / gap) and every counting helper (rows_h, calc_row)
   speak that step with no widget changes.  STD is authored identical to the base metrics, so
   an unpushed UI is unchanged. */
typedef enum
{
    GUI_SCALE_DENSE,   // text lists: outliners, entity browsers, tree views, table rows
    GUI_SCALE_STD,     // the everyday widget row: forms, buttons, sliders, inputs
    GUI_SCALE_ROOMY,   // menus, combo dropdown lists, title-height rows
    GUI_SCALE_BAR,     // tab bars, icon toolbars, panel headers
    GUI_SCALE_COUNT

} gui_scale_t;

/* One ramp step's metrics.  Authored in px at em=12 like every other theme metric; em-scaled,
   grid-quantized, and font-floored (a row always holds a text line) by gui_style_apply. */
typedef struct gui_scale_metrics_t
{
    u8 row;   // row height (the step's WIDGET_H)
    u8 pad;   // frame / content padding
    u8 gap;   // gap between consecutive widgets

} gui_scale_metrics_t;

/* ONE struct, TWO categories.  They stay together because the machinery treats them all
   identically -- themes snapshot them, the style stacks override them, style_var/style_col
   resolve them, gui_style_apply em-scales them -- and ONE test sorts every field between the
   two categories: can a read of this field move a rect?

     1. METRICS -- the spacing/size vocabulary.  One set of numbers consumed at two moments:
        composition (the composer divides space into cells -- row heights, gaps, region insets,
        scrollbar gutters, title bars) and widget self-measurement (a widget computes the
        natural size it REQUESTS through widget_next_rect_w, then seats its label / indicator
        inside the finished cell with the same pad).  Only the composer POSITIONS rects;
        widgets only measure and request -- that is the composition contract.
     2. SKIN -- paint-only: colors, corner roundings, mark shapes, caret geometry.  A read of
        these can only change pixels inside a rect composition already fixed; none ever sizes
        or moves a cell.

   Behavior (interact/) consumes neither category: it takes finished rects.  (Its one metric
   read is win_border, because the resize hit zone straddles the border -- border is geometry.) */

/* GUI_GRID_LATTICE -- compile-time master switch for grid_quantum snapping.  1 (default) keeps
   the feature; define 0 (e.g. -DGUI_GRID_LATTICE=0) to strip every snap to identity so the lattice
   arithmetic folds out and grid_quantum costs nothing.  Independent of a style's grid_quantum,
   which still disables snapping per-style at runtime when <= 1. */
#ifndef GUI_GRID_LATTICE
#define GUI_GRID_LATTICE 1
#endif

typedef struct gui_style_t
{
    u32 colors[ GUI_COL_COUNT ]; // SKIN: theme default palette (GUI_COLOR packs R,G,B,A bytes)

    /* 1. METRICS -- can move a rect: cell sizes, insets, gutters, and natural-size inputs */
    u8 line_size;          // widget row height
    u8 widget_gap;         // vertical gap between consecutive widgets
    u8 widget_pad;         // region inset (composer) AND label inset / natural-width pad (widgets)
    u8 min_cell_w;         // floor a flex/fraction track shrinks to before overflow
    u8 grid_quantum;       // px lattice row-level metrics snap to after font scaling (0/1 = off)
    u8 win_border;         // outline thickness -- consumes space: child heights, bar tracks, resize zones
    u8 win_title_h;        // window title bar height -- the body starts below it
    u8 checkbox_sz;        // checkbox indicator side -- feeds the checkbox's natural width
    u8 slider_knob_w;      // slider knob width AND the scrollbar gutter thickness regions reserve

    /* 2. SKIN -- paint-only: never sizes a cell */
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
    u8 checkmark_pad;      // inset of the check mark inside the checkbox
    u8 cursor_w;           // input text caret width
    u8 cursor_inset;       // input text caret top/bottom inset

    /* The scale ramp (see gui_scale_t) -- METRICS per density step.  STD mirrors
       line_size / widget_pad / widget_gap. */
    gui_scale_metrics_t scales[ GUI_SCALE_COUNT ];

} gui_style_t;

/* gui_style_get() -- returns a pointer to the mutable base style (s_style_base).  Edits take
   effect on the next gui_style_apply() / gui_theme_reset() call.  Mutating the struct directly
   without calling theme_reset marks the theme as anonymous (theme_get returns NULL). */

gui_style_t* gui_style_get( void );

/* gui_style_peek() -- read-only view of the base style.  Unlike gui_style_get() this does NOT
   mark the theme anonymous, so a style editor can populate its widgets (and keep labeling the
   active theme) and only call gui_style_get() on the frame it actually commits an edit. */

const gui_style_t* gui_style_peek( void );

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

    The tunable scalar metrics, the ImGuiStyleVar_ analogue.  Each names one scalar the layout
    or widgets read; push_style_var( var, value ) overrides it until the matching pop_style_var,
    next_style_var for just the next widget, and an unpushed var uses the font-derived default
    (recomputed when the font changes).  Values are f32 pixels.

    Grouped by the same two categories as gui_style_t (one mechanism, two audiences):
    METRICS slots can move rects (scale_push rides on the first three); SKIN slots only
    change how paint lands inside rects composition already fixed.

    Only metrics that flow through the shared accessor are listed, so every slot here is honored
    uniformly everywhere it is read; purely cosmetic internals (caret width, checkmark inset) are
    intentionally left off rather than exposed as half-working knobs.
==============================================================================================*/

typedef enum
{
    /* 1. METRICS -- can move a rect (scale_push/scale_pop override the first three) */

    GUI_VAR_LINE_SIZE,      // widget row height (the frame height)
    GUI_VAR_WIDGET_GAP,     // gap between consecutive widgets / cells
    GUI_VAR_WIDGET_PAD,     // content padding inside a frame (FramePadding)
    GUI_VAR_MIN_CELL_W,     // min width a flex cell shrinks to
    GUI_VAR_WIN_BORDER,     // outline thickness -- consumes space (child heights, bar tracks)
    GUI_VAR_WIN_TITLE_H,    // window title bar height -- the body starts below it
    GUI_VAR_CHECKBOX_SZ,    // checkbox / radio indicator side -- feeds the natural width
    GUI_VAR_SLIDER_KNOB_W,  // slider knob width + the scrollbar gutter thickness

    /* 2. SKIN -- paint-only */

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
    GUI: Rect Algebra

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
    GUI: Layout Template

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
        == 0.0        natural size -- the item's own content size.  Only pack mode resolves per
                      item with content in hand; a pre-divided column / grid track has none at
                      resolve time, so 0 there collapses to a zero-width track -- use fill /
                      fraction / px in columns + grid instead.
        <  0.0        GUI_END, the track-list terminator

    Gaps sit *between* cells and are subtracted before distribution, so a widget never sees or
    reasons about spacing -- it just fills the rect it is handed.

    Scope: this unit sizes tracks (row / cols / grid / next_item_fit / field_split) only.  Pure
    spacing calls (same_line, new_line) look similar but aren't -- a gap has no content, so its
    0 is a literal zero, not a content measure; see their own docs.

==============================================================================================*/

#define GUI_LAYOUT_COLS 8                     // max tracks on one axis (columns or rows)
#define GUI_END (-1.0f)                       // track-list terminator (any negative value)

/* carve markup sentinels -- nest a gui()->carve form (a single GUI_END-terminated f32 list, the same
   overloaded unit as cols).  A size FOLLOWED by a CUT is a container of that size, subdivided on the
   named axis until a matching GUI_END; a size followed by anything else is a leaf.  A form opens with
   a leading CUT that fills the whole area.  See gui()->carve. */

#define GUI_CUT_X (-2.0f)                     // open a nested column split (panels side by side)
#define GUI_CUT_Y (-3.0f)                     // open a nested row split (panels stacked)

/*==============================================================================================
    GUI: Content Alignment

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

/*----------------------------------------------------------------------------------------------
    Placement adapters -- position a self-sized box inside an existing rect, the free-placement
    companion to split / carve (which divide a rect into adjacent panels).  These never touch the
    layout pen: they take a parent rect and return a child rect, so they compose with content_rect,
    push_layout_overlay and each other, and an overlay is just several placements over one area in
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
    Item interaction state

    One frame of interaction for one item -- the result of the shared widget interaction state
    machine, whether run internally for a stock widget or over a caller rect via gui()->item().
    A user widget takes a rect (canvas cut, split/carve, own math), asks for behavior, and draws
    its own presentation from these flags, exactly as the stock widgets do internally.
    invisible_button( id, r ) is this reduced to its click bit.
----------------------------------------------------------------------------------------------*/

typedef struct gui_item_state_t
{
    bool hover;      // cursor is over the rect this frame
    bool active;     // primary button held with this item captured (dragging / holding)
    bool pressed;    // primary button went down on the item this frame
    bool clicked;    // press + release completed with the cursor still over ("fired")
    bool focused;    // item owns keyboard input (focusable items: text / value fields)
    bool nav;        // item is the keyboard-nav cursor while the keyboard is active (fill state)
    i32  nav_adjust; // keyboard value edit: -1 / +1 arrow step this frame (captured drag items)

} gui_item_state_t;

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

    /* selectable: do NOT close the enclosing popup when clicked.
       By default a selectable inside any popup calls popup_close_current()
       on click (Dear ImGui CloseCurrentPopup default).  Set this to opt out --
       e.g. a multi-select list inside a popup where the popup should stay open. */
    GUI_ITEM_NO_CLOSE_POPUP = 1 << 3,

    /* Room to grow without disturbing call sites or the vtable -- e.g. a future
    GUI_ITEM_READ_ONLY (editable widgets show but reject input), GUI_ITEM_NO_NAV, etc. */

} gui_item_flags_t;

/*==============================================================================================
    Drag and drop

    Item-to-item payload transfer (the ImGui BeginDragDropSource/Target analogue).  A widget
    becomes a drag SOURCE by calling drag_source_begin right after it emits; while the user
    drags from it, the source sets a typed payload (a small byte blob, copied) and emits preview
    widgets that follow the cursor.  Any other widget becomes a drop TARGET by calling
    drag_target_begin right after it emits; while a drag hovers it, drag_payload_accept matches
    the type tag, highlights the target, and returns the payload on the release frame.  One drag
    exists at a time (one mouse), so the payload store is a single ambient slot.  See gui_api.h
    for the usage contract.
==============================================================================================*/

#define GUI_DRAG_TYPE_CAP     16    // bytes of a payload type tag, including the NUL
#define GUI_DRAG_PAYLOAD_CAP  64    // payload bytes copied by drag_payload_set

typedef enum
{
    GUI_DRAG_NONE        = 0,

    /* drag_payload_accept: return the payload while the drag hovers the target (every frame),
       instead of only on the release (drop) frame -- for live preview effects at the target. */
    GUI_DRAG_ACCEPT_PEEK = 1 << 0,

    /* drag_source_begin: no cursor-following preview tooltip is opened (emit nothing between
       begin/end).  drag_payload_accept: skip the accept highlight around the target. */
    GUI_DRAG_NO_PREVIEW  = 1 << 1,

} gui_drag_flags_t;

/* The caller-facing payload view returned by drag_payload_accept / drag_payload_peek.  The bytes
   were copied at drag_payload_set time and stay valid until the drag ends (the release frame
   included) -- copy them out if they must outlive the drop. */
typedef struct
{
    const char* type;   // tag stamped by drag_payload_set
    const void* data;   // payload bytes (copied at set time)
    u32         size;   // payload byte count

} gui_drag_payload_t;

/*==============================================================================================
    GUI: Angle Algebra

    Angles -- the arc / pie / spinner / progress sweep parameters (draw_arc, draw_pie, ...) are
    radians in screen space (y down, so a positive angle turns clockwise; 0 points right / +x).
    Author in friendly degrees and convert at the call site:

    gui()->draw_arc( cx, cy, r, gui_radians( 0 ), gui_radians( 270 ), 3.0f, col );
    gui()->draw_pie( cx, cy, r, gui_radians( -90 ), gui_radians( 90 ), col );

    Stateless pure math, so these are inline here (no vtable entry) like the rect helpers above.
==============================================================================================*/

#define GUI_PI 3.14159265358979f

/* Degrees -> radians (the unit the arc / pie / sweep parameters take). */
static inline f32 gui_radians( f32 degrees ) { return degrees * ( GUI_PI / 180.0f ); }

/* Radians -> degrees (to read a stored sweep back in friendly units). */
static inline f32 gui_degrees( f32 radians ) { return radians * ( 180.0f / GUI_PI ); }

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
    Implementation in gui_emit_path.c.

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
    Volatile widget callback

    A "volatile" callback contains ordinary UI emit calls (text, colored rects, etc).  It runs
    inline during a real (dirty) frame via gui()->volatile_cb -- its widgets render exactly like
    any other code, no special behavior, except that the block's geometry is given its own
    RESERVED, PADDED region of the window's cached tessellation (vertex/index/draw-command
    headroom past what it actually produced).  On an idle frame (frame_dirty()==false), the same
    callback is invoked again standalone with is_replay=true; the framework reconstructs just
    enough context (window/clip/cursor position + the ambient draw state stamped at real emit),
    re-tessellates the output, and patches it into the reserved region.  The output does NOT have
    to match the original -- text may grow or shrink, shapes may change -- it only has to FIT the
    reservation; outgrowing it costs one automatic real frame, after which the block re-captures
    with a larger reservation (grow-only per widget).  The block's commands are also excluded from
    the window's retained-cache hash, so an animating block never forces its window to
    re-tessellate -- see gui_volatile_cb / gui_volatile_begin / gui_update_volatile.

    Interactive widgets (button, etc) are safe to call from a volatile callback but are inert
    during replay: hover/active/focus reflect whatever the last real frame established, but a
    replay can never newly acquire either state or fire a fresh click -- interaction is only ever
    resolved on real frames, which is guaranteed since input changes always force one.

    CONTRACT -- fixed layout footprint: the block's PIXELS may change every frame, but the space
    it occupies in the layout must not.  Surrounding widgets are retained and only re-lay-out on
    real frames; a block whose size varies (e.g. text gaining a digit) shoves its neighbours
    around on real frames while they sit frozen on idle ones -- visible jitter, plus the window
    re-tessellates every real frame because the neighbours' positions really did change.  Use
    fixed-size formatting ("%8.3f" with a mono font), a fixed canvas(), or padding to keep the
    footprint constant.
==============================================================================================*/

typedef void ( *gui_volatile_fn )( bool is_replay );

/*==============================================================================================
    Semantic draw commands

    The UI build pass emits one gui_cmd_t per visible shape into a list.  The render backend
    (gui_render.c) tessellates each command into vertices and indices at flush time.  This
    separates the UI logic from any graphics API knowledge.

    GPU draw commands (gui_gpu_cmd_t) are a backend-private type defined in gui_emit_draw.c;
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
    GUI_CMD_DASHED_LINE,     // patterned line: one textured quad, atlas dash row, tiled by U
    GUI_CMD_RECT_GRADIENT,   // filled rect, col_a->col_b blended by per-vertex color (one quad)

} gui_cmd_type_t;

/* Sentinel half-extent for an unclipped text command: any real glyph sits well inside this, so
   the tessellator's clip test never triggers and the whole-run fast path is taken. */
#define GUI_TEXT_NO_CLIP 1e30f

/* High bit of a rect command's tex_idx: sample the bindless texture as a full RGBA image (the
   texel is the color, vertex color tints) instead of the default R8-coverage model (the texel's
   R channel is alpha, vertex color supplies RGB).  Set by image_texture / draw_texture_in for
   scene-viewport style external textures; the render backend strips the bit into the rgba_tex
   push constant.  Batching is unaffected: commands split on the full 32-bit tex_idx value. */
#define GUI_TEX_RGBA_BIT 0x80000000u

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
                                      (akin to Dear ImGui's FirstUseEver) */

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

    /* Title bar & chrome -- strip default window decoration. */

    GUI_WIN_NOTITLEBAR        = 1 << 0,    /* no title bar: body fills the top; no collapse, no titlebar drag */
    GUI_WIN_NOCOLLAPSE        = 1 << 1,    /* no collapse arrow; the window stays expanded */

    /* Menu bar -- reserve a one-row strip below the title bar that menu_bar_begin fills (the
       ImGuiWindowFlags_MenuBar analogue).  The strip is carved from the top of the body before the
       body scroll region opens, so it never scrolls; menu_bar_begin returns false unless set. */

    GUI_WIN_MENUBAR           = 1 << 2,    /* reserve a non-scrolling menu-bar strip (menu_bar_begin) */

    /* Move & resize -- the window is user-movable and edge-resizable by default. */

    GUI_WIN_NOMOVE            = 1 << 3,    /* disable user drag moving the window from anywhere */
    GUI_WIN_NORESIZE          = 1 << 4,    /* disable user resizing from the border edges */

    /* Placement is managed externally (docking layout, animation, scripted snap).  Bypasses both
       the per-drag margin clamp (window_clamp) and the merge-back fit-inside clamp so the system
       can position and size the window freely without gui fighting the placement.  Without this
       flag both clamps apply unconditionally; with it neither does. */

    GUI_WIN_NO_BOUNDARY_CLAMP = 1 << 5,    /* placement is externally managed; skip both clamps */

    /* Auto-resize -- size the window to its content instead of a fixed w/h. */

    GUI_WIN_ALWAYS_AUTOSIZE   = 1 << 6,    /* hug content every frame: no user resize, no scrollbars */
    GUI_WIN_CAN_AUTOSIZE      = 1 << 7,    /* show a corner size-grip; double-click it to fit content */

    /* Scrolling -- a dynamic vertical bar and mouse-wheel input are on by default.  The NO* flags
       switch pieces off; the ALWAYS_* flags force a bar to stay shown regardless of content. */

    GUI_WIN_NOSCROLL          = 1 << 8,    /* disable all scroll bars (keep mouse input) */
    GUI_WIN_NOMOUSESCROLL     = 1 << 9,    /* disable mouse wheel scrolling */
    GUI_WIN_HSCROLL           = 1 << 10,   /* enable dynamic horizontal scroll bar (off by default) */
    GUI_WIN_ALWAYS_VSCROLL    = 1 << 11,   /* always show vertical scroll bar -- override */
    GUI_WIN_ALWAYS_HSCROLL    = 1 << 12,   /* always show horizontal scroll bar -- override */

    /* Native-borderless: this window IS its host OS window (window kind 3).  Its titlebar stands in
       for the Win32 caption and its border for the sizing frame, so titlebar drag / double-click /
       right-click and border drags are routed to native OS window actions (app()->window_start_move
       / window_title_event / window_system_menu / window_start_resize) instead of gui's in-client
       move, tear-off, collapse, and edge-resize.  The host OS window must have been opened with
       APP_WIN_BORDERLESS.  Geometry is owned by the OS window (size follows WM_SIZE).

       Hosts normally do not pass this flag directly: gui()->viewport_shell() is the front door --
       it emits the frame-only shell (and no-ops on an OS-chrome window).  Pass NATIVE yourself
       only to build a custom shell window. */

    GUI_WIN_NATIVE            = 1 << 13,

    /* Title-bar capability -- subtract a caption control a window should not offer.  All three
       buttons show by default (opt-out, like the NO* flags above); a window drops the ones it does
       not support and the title bar reflects that, reclaiming the freed space for the title text.

       NO_MINIMIZE / NO_MAXIMIZE gate the OS minimize / maximize caption buttons and so apply only to
       a native window (GUI_WIN_NATIVE or a detached floater) -- a non-native panel has no such OS
       state and never drew them.  The close (main) / pop-in (floater) primary button is never
       suppressed: close is essential and pop-in is a floater's only route back to the main surface.

       NO_DETACH removes the pop-out path for any window -- it hides the non-native detach button and
       blocks the drag tear-off -- independent of NOMOVE (a window may move yet refuse to pop out). */

    GUI_WIN_NO_MINIMIZE       = 1 << 14,   /* native: no minimize caption button */
    GUI_WIN_NO_MAXIMIZE       = 1 << 15,   /* native: no maximize / restore caption button */
    GUI_WIN_NO_DETACH         = 1 << 16,   /* no pop-out: hide detach button, block tear-off drag */

    /* Closeable -- add a close (X) button at the title bar's right edge.  Clicking it hides the
       window: window_begin returns false and emits nothing from then on, and the record persists
       so the window keeps its position / size while closed.  Re-opening is the caller's job --
       offer a button that calls window_set_open( title, true ).  A native window uses its OS close
       caption button instead, so this flag only adds the X to a regular (non-native) panel. */

    GUI_WIN_CLOSEABLE         = 1 << 17,   /* show a close (X) button; hidden until re-opened */

    /* Never hosts tabs: the window is not a tab-drop target (no center chip when another window is
       title-dragged over it) and window_tab refuses it as `onto_title`.  For control / instruction
       panels whose body the host gates on window_begin's return -- becoming an inactive tab would
       silently skip that body.  The window itself may still be dragged INTO groups or dockspaces. */

    GUI_WIN_NO_TAB_TARGET     = 1 << 18,   /* not a tab-drop target; window_tab refuses it as onto_title */

    /* Input passthrough -- the window is purely visual; the cursor passes through it as if it
       were not there.  hover_win is never set to this window, so no widget inside can receive
       mouse input and the window never steals clicks from content behind it.  Combine with
       GUI_WIN_OVERLAY for a completely inert window-based HUD.  region_begin (gui_region.c)
       honors this flag too -- a region is interactive by default, opt out with NO_INPUT for a
       fixed-rect HUD with no window identity at all (the perf overlay's case). */

    GUI_WIN_NO_INPUT          = 1 << 19,   /* click-through: never becomes hover_win */

    /* Child regions -- child_begin only.

       CHILD_RESIZE_X / _Y (the ImGuiChildFlags_ResizeX / _ResizeY analogue): a draggable grip on
       the child's right / bottom border; the size on that axis then becomes user-owned and persisted
       -- seeded once from the child_begin w/h, thereafter set by the drag -- overriding the passed
       value.  RESIZE_Y supersedes the h<=0 auto-size on that axis.  A real window ignores these (it
       owns its geometry already), as does a grid-cell child (the cell sizes it).  Vertical is the
       common case; both axes may be combined.

       NO_CLIP: skip pushing a draw clip rect for this child region.  Use when the caller knows
       content fits and wants to avoid the extra draw batch the scissor causes. */

    GUI_WIN_CHILD_RESIZE_X    = 1 << 20,   /* child: drag the right border to resize width   */
    GUI_WIN_CHILD_RESIZE_Y    = 1 << 21,   /* child: drag the bottom border to resize height  */
    GUI_WIN_NO_CLIP           = 1 << 22,   /* child: do not push a clip rect */

    /* Arena band: routes this window's (or region's) retained geometry into the debug band of
       the shared vertex/index arena.  Debug-band windows pack AFTER every main-band slot, are
       excluded from the render stats they may themselves display, and never raise any_changed /
       frame_dirty (so a live readout cannot silently defeat idle-skip for the whole app).  For
       self-measuring diagnostic UI -- the perf overlay and the pipeline dashboard; popups and
       tooltips opened from inside a debug-band window inherit the band automatically. */

    GUI_WIN_DEBUG_BAND        = 1 << 23,   /* diagnostic UI: debug arena band + stats/dirty exempt */

    /* Convenience composites -- common flag bundles named for intent (the ImGuiWindowFlags_NoXxx
       shorthands).  Plain ORs of the bits above, so they compose with extra flags as usual
       ( GUI_WIN_OVERLAY | GUI_WIN_NOMOUSESCROLL ) and a window's resolved behavior is identical
       to spelling the members out.

       NODECORATION -- strip all chrome: no title bar, no border resize, no scrollbars, no collapse.
                       A bare content panel you still position / move yourself.
       OVERLAY      -- a passive, non-interactive window-based HUD: undecorated, fixed in place,
                       hugging its content every frame, and non-detachable.  Pin it with
                       window_set_next_pos.  For a HUD with no window identity at all (no pool
                       record, no dock/native/z-order path), use region_begin instead. */

    GUI_WIN_NODECORATION = GUI_WIN_NOTITLEBAR | GUI_WIN_NORESIZE |
                           GUI_WIN_NOSCROLL   | GUI_WIN_NOCOLLAPSE,

    GUI_WIN_OVERLAY      = GUI_WIN_NODECORATION    | GUI_WIN_NOMOVE |
                           GUI_WIN_ALWAYS_AUTOSIZE | GUI_WIN_NO_DETACH | GUI_WIN_NO_INPUT,

} gui_win_flags_t;

/*----------------------------------------------------------------------------------------------
    Region z tier (region_begin) -- where a root region sits in the one z contest windows and
    popups compete in (whichever z wins, wins draw order and hover that frame).  A three-way
    choice, so it is a parameter rather than flag bits.

    MID -- the default: a fixed band above every ordinary window and below every popup, so a HUD
           element draws over normal windows but under a menu / combo / modal.
    BG  -- lowest tier (ties the docked/base window floor).  A background element that only
           receives hover when nothing else (no raised window) covers it -- e.g. a desk-level
           widget that must not steal clicks from anything on top of it.
    FG  -- highest tier (above every popup depth).  Always wins draw order and hover, even over
           an open menu/combo/modal -- e.g. an always-on-top HUD button that must remain
           clickable no matter what else is open.
----------------------------------------------------------------------------------------------*/

typedef enum
{
    GUI_REGION_MID = 0,     /* default band: over windows, under popups */
    GUI_REGION_BG,          /* background: loses to any raised window   */
    GUI_REGION_FG,          /* foreground: wins over every popup        */

} gui_region_tier_t;

/*==============================================================================================
    Dockspace flags

    Passed to dockspace_over_viewport.  0 (GUI_DOCKSPACE_NONE) is the default dockspace that fills
    the viewport behind the free-floating windows.  Bit values so future policy bits (e.g. hide the
    single-tab strip, no central-node auto-hide) can be ORed in without changing the call sites.
==============================================================================================*/

typedef enum
{
    GUI_DOCKSPACE_NONE     = 0,        /* default: fill the viewport, draw splitters + tab bars */

    /* Tab docking only: windows may tab into leaves (center drop) but never split them -- no side
       or edge drop chips are offered and the programmatic split verbs refuse, so no splitters can
       ever form.  The dockspace degenerates to one full-area tab group.  Sticky per viewport:
       re-published every dockspace_over_viewport call. */
    GUI_DOCKSPACE_NO_SPLIT = 1 << 0,

} gui_dockspace_flags_t;

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
    GUI_TABLE_COL_ALIGN_RIGHT  = 1 << 4,  // FUTURE: right-align cell content (flag reserved, unconsumed)
    GUI_TABLE_COL_ALIGN_CENTER = 1 << 5,  // FUTURE: center cell content (flag reserved, unconsumed)

} gui_table_col_flags_t;

/* Background color override target for table_set_bg_color. */
typedef enum
{
    GUI_TABLE_BG_NONE = 0,
    GUI_TABLE_BG_ROW,     // tint the current entire row    
    GUI_TABLE_BG_CELL,    // tint the current cell only     

} gui_table_bg_target_t;

/* Sort specification returned by table_get_sort_specs. */
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
/*==============================================================================================
    GUI: Font Configuration
==============================================================================================*/
/* Built-in font presets for init() -- pre-baked .orb_font assets (FreeType-rasterized offline by
   font_tool, not an stb runtime bake) shipped under assets/font/.  GUI_FONT_NONE loads nothing;
   the caller is then responsible for its own font_load() before the first frame renders. */

typedef enum
{
    GUI_FONT_NONE = 0,        // load nothing; caller loads its own font(s) via font_load()
    GUI_FONT_JETBRAINS_16,
    GUI_FONT_ROBOTO_16,
    GUI_FONT_CASCADIA_MONO_16

} gui_builtin_font_t;

/*==============================================================================================
    GUI: Capability Flags
==============================================================================================*/
/* Backend capability flags -- latched via gui_init_config_back() before init().  The render
   pipeline itself (fonts, EMIT draw list, tessellate, SUBMIT flush) is always on and has no flag;
   everything here is complexity layered on top that a caller can independently switch off.  A
   caller that never calls gui_init_config_back() gets GUI_CAPS_DEFAULT, which preserves the
   pipeline's full behavior -- these flags exist to let a minimal/embedded use of gui shed layers
   it doesn't need, not to change what a default caller sees. */

typedef struct
{
    bool icons;             // Runtime icon atlas (icon_register/find, draw_push_icon) -- owns its own
                            // 512x512 R8 texture + stb_rect_pack packer, stood up at init when on
    bool retained_cache;    // BUILD-phase diff + geometry reuse; off always re-tessellates every
                            // Window for every frame (also the backing for set_retained_skip)
    bool render_debug;      // Wireframe/batch-tint debug render mode; off skips compiling the 
                            // second (wireframe) pipeline at init
    bool stats_trace;       // Per-frame printf lines for cache diff / geometry / retained / draw-calls

} gui_backend_caps_t;

#define GUI_CAPS_DEFAULT \
    ( ( gui_backend_caps_t ){ .icons = true, .retained_cache = true, \
                              .render_debug = true, .stats_trace = false } )

/* Forward (UI-unit) capability flags -- latched via gui_init_config_front() before init().
   These do not exclude any code from the build -- the point is feature-boundary clarity. */

typedef struct
{
    bool tables;            // default on: optionally turn off tables.
    bool docking;           // default on: optionally turn off docking.
    bool keyboard_nav;      // default on: optionally turn off keyboard navigation.

} gui_forward_caps_t;

#define GUI_FORWARD_CAPS_DEFAULT \
    ( ( gui_forward_caps_t ){ .tables = true, .docking = true, .keyboard_nav = true } )

/*==============================================================================================
    GUI: Boot descriptor

    One-call host setup (gui()->boot): gui owns the main OS window and its render context end to
    end -- the same lifecycle its tear-off floaters already use -- instead of the host assembling
    window_open / context_open / init / viewport_open by hand.  Everything here is optional in the
    sense that a field left zero keeps today's default; the struct is designed to be built as a
    compound literal at the call site.  See boot() in gui_api.h for the full contract.
==============================================================================================*/

typedef struct
{
    const char*               title;      /* OS window title; doubles as the chrome shell caption  */
    i32                       x, y;       /* window position; 0,0 = OS centers                     */
    i32                       w, h;       /* client size; 0,0 = 50% of the desktop work area       */
    bool                      os_chrome;  /* true = stock OS frame; false (default) = borderless
                                             window with the gui chrome shell auto-emitted         */
    gui_builtin_font_t        font;       /* built-in preset; GUI_FONT_NONE = caller font_load()s  */
    const gui_forward_caps_t* caps;       /* UI-unit feature caps; NULL = GUI_FORWARD_CAPS_DEFAULT */
    gui_clock_fn              clock;      /* frame hooks (gui links no sys) -- see set_frame_hooks */
    gui_sleep_fn              sleep;
    gui_wait_events_fn        wait;
    f32                       clear[ 4 ]; /* present() clear color; alpha 0 = default dark        */
    bool                      debug;      /* arm the debug hotkey driver (debug_enable)            */

} gui_boot_desc_t;

/*==============================================================================================
    Limits
==============================================================================================*/

/* 16K verts covers the busiest measured frame (all sb_gui demo windows + the pipeline
   dashboard peak ~9K) with headroom, and keeps vertex indices well within u16 range
   (64K would sit right at the 65535 ceiling).  The 2x index ratio suits quad-dominated
   UI (6 idx per 4 verts = 1.5:1; AA paths/arcs stay under 2:1); the geometry that would
   exceed it overflows a frame's tessellation, not the buffer sizing.  The per-frame
   region sizes that fall out of these (VB 320 KB, IB 64 KB) are both 256-byte aligned,
   so each frame-in-flight region stays independently addressable -- note that this only
   matters if the VB/IB are ever moved off HOST_COHERENT memory, in which case regions
   would need rounding up to nonCoherentAtomSize to flush apart. */

#define GUI_MAX_VERTS      ( 32 * 1024 )
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
    Memory usage breakdown (bytes), reported by gui()->mem_stats().

    A full accounting of what the gui system holds, split by WHERE it lives:

      - GPU     : device memory -- per-viewport geometry buffers + font atlas textures.  Dynamic:
                  created at init / viewport_open, released at shutdown / viewport_close.
      - CPU .bss: fixed-size backend buffers baked into the image -- the draw list, the CPU
                  tessellation staging, the retained geometry cache, the font registry.  Present
                  for the whole run whether one window is open or fifty.
      - CPU heap: one malloc block per live context (header + state / popup / window / viewport /
                  dock pools).  Dynamic: grows only when a secondary context is created.

    Every bucket is exact (a sizeof of the backing array, a summed malloc size, or a live-count
    multiply of a fixed region), so the grand total is the true resident footprint -- not a
    high-water estimate.  print_mem_stats() dumps the same breakdown as a sectioned table.
==============================================================================================*/

typedef struct
{
    /* --- GPU device memory (dynamic). --- */
    u32 gpu_vertex_bytes;   // per-viewport VB regions, summed over live surfaces x frames-in-flight
    u32 gpu_index_bytes;    // per-viewport IB regions, summed over live surfaces x frames-in-flight
    u32 gpu_texture_bytes;  // font atlases (each already includes its white + dash rows)
    u32 gpu_total;          // sum of the three above
    u32 viewport_count;     // live GPU surfaces contributing to gpu_vertex/index_bytes

    /* --- CPU static memory (.bss; fixed backend buffers, resident the whole run). --- */
    u32 cpu_drawlist_bytes; // s_draw: command list + hashes + point/segment pools + text + clip tables
    u32 cpu_tess_bytes;     // s_tess: CPU vertex / index / GPU-command staging
    u32 cpu_cache_bytes;    // retained cache: ping-pong slot tables + cached GPU cmds + diff records
    u32 cpu_font_bytes;     // font registry slots (CPU glyph metrics), excluding the GPU atlas
    u32 cpu_static_total;   // sum of the four above

    /* --- CPU dynamic memory (heap; one malloc block per live context). --- */
    u32 cpu_context_bytes;  // sum over live contexts of the single ctx block (header + all pools)
    u32 context_count;      // live contexts contributing to cpu_context_bytes
    u32 cpu_dynamic_total;  // heap total (== cpu_context_bytes today; named for the section subtotal)

    /* --- Grand total: everything the gui system holds right now. --- */
    u32 total_bytes;        // gpu_total + cpu_static_total + cpu_dynamic_total

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

    u32 volatile_patched; // volatile_cb rows whose geometry was patched in place this frame
                           // (idle replay or a live real-frame reuse-patch) -- a separate signal
                           // from win_retained: a window with an animating volatile widget still
                           // counts as fully retained; this is what actually moved.
} gui_render_stats_t;

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

/*============================================================================================*/
#endif    // GUI_H
