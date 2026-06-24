#ifndef IMGUI_INTERNAL_H
#define IMGUI_INTERNAL_H
/*==============================================================================================

    runtime_service/imgui/imgui_internal.h -- Shared internal types for the imgui unity build.

    imgui is built as a unity translation unit (imgui.c #includes every imgui_*.c).  Historically
    each constituent file defined its record types inline and later-included files relied on the
    include ORDER to see them -- a window record defined in imgui.c, a layout frame in imgui_ctx.c,
    a viewport in imgui_render.c, all folded by value into imgui_context_t further down.  This
    header lifts that cross-file TYPE layer into one place so the dependency is explicit and
    order-independent: every constituent file includes this once, up front, and sees the full set.

    It holds ONLY types (and the capacities they embed) shared across more than one constituent
    file.  File-private record types (the GPU command, the font atlas, the style save-pairs, the
    text-edit scratch) stay in their owning .c file.  The per-file STATE instances (s_io, s_build,
    g_ctx, the stacks) also stay in their owning files -- this header declares their types, not the
    storage; the unity TU still resolves the statics.

    Include chain: imgui_internal.h -> imgui_host.h -> imgui_api.h -> imgui.h
    Also pulls rhi_api.h (imgui_viewport_t holds GPU buffers/targets) and app_api.h (imgui_io_t
    indexes app keys; the OS-event forwarders take an app_event_t).

==============================================================================================*/

#include "runtime_service/imgui/imgui_host.h"   /* public imgui types: imgui_rect_t, imgui_id_t, flags, enums */
#include "runtime_service/rhi/rhi_api.h"          /* rhi_buffer_t / rhi_texture_t for imgui_viewport_t           */
#include "engine/app/app_api.h"                   /* app_key_t / app_event_t for imgui_io_t + event forwarders   */

// clang-format off

/*==============================================================================================
    Shared capacities

    Fixed-array bounds embedded in the shared record types below.  (IMGUI_LAYOUT_COLS lives in the
    public imgui.h; the per-file stack depths that are NOT embedded in a shared type -- the id stack,
    the item-flag stack -- stay private to their owning .c file.)
==============================================================================================*/

/* Per-context default pool sizes -- used to wire the static default context (slot 0).
   Secondary contexts may use different sizes passed via imgui_ctx_config_t. */
#define IMGUI_DEFAULT_MAX_WINDOWS    32   /* default persisted window pool */
#define IMGUI_DEFAULT_POPUP_DEPTH     8   /* default max nested popups */
#define IMGUI_DEFAULT_STATE_SLOTS   512   /* default keyed state pool capacity (power of two) */
#define IMGUI_DEFAULT_MAX_VIEWPORTS   4   /* default render surfaces */
#define IMGUI_DEFAULT_DOCK_NODES     48   /* default dock-tree nodes */

/* Non-per-context capacities -- these size non-context structs and stay as fixed constants. */
#define IMGUI_LAYOUT_DEPTH  8       // max nested scroll regions (windows or children)
#define IMGUI_KEY_COUNT     128     // imgui_io_t key arrays; must cover the full app_key_t range

#define IMGUI_DOCK_TABS_MAX  8      // windows co-docked (tabbed) in one leaf node
#define IMGUI_DOCK_NAME_CAP  28     // bytes of a tab's display name, copied at dock time

#define IMGUI_STATE_CAP   20        // payload bytes per slot (max state struct: imgui_region_t)

/* GPU buffer region sizing uses a fixed viewport count (allocated once at init before any config).
   This is NOT the per-context runtime limit -- that is g_ctx->max_viewports.
   Must match APP_WIN_MAX / RHI_CTX_MAX. */
#define IMGUI_MAX_VIEWPORTS 4       // GPU buffer region count; matches APP_WIN_MAX / RHI_CTX_MAX

/*==============================================================================================
    Input snapshot (imgui_input.c)

    The frame's distilled IO state -- not exposed in the public header.  IMGUI_KEY_COUNT must cover
    the full app_key_t range; imgui_input.c carries the static assert that verifies this.
==============================================================================================*/

typedef struct
{
    f32   mouse_x, mouse_y;
    f32   mouse_wheel;
    bool  mouse_down    [ 3 ];
    bool  mouse_pressed [ 3 ];
    bool  mouse_released[ 3 ];
    bool  mouse_double  [ 3 ];
    bool  keys_down          [ IMGUI_KEY_COUNT ];
    bool  keys_pressed       [ IMGUI_KEY_COUNT ];   /* initial press only                  */
    bool  keys_pressed_repeat[ IMGUI_KEY_COUNT ];   /* initial press + OS auto-repeat ticks */
    bool  keys_released      [ IMGUI_KEY_COUNT ];
    char  text[ 32 ];
    char  paste[ 256 ];   /* clipboard text delivered this frame (APP_EV_CLIPBOARD), else empty */
    f32   dt;
    f64   time;           /* seconds since the first frame -- dt accumulated; backs get_time() */
    i32   display_w, display_h;
    u32   mouse_viewport; /* surface the cursor is in (resolved from mouse-event win_id); persists */

} imgui_io_t;

/*==============================================================================================
    Layout metrics (imgui.c)

    Integer pixel dimensions derived from the active font's type size (em) by layout_compute.
    The instance (s_layout) lives in imgui.c; other files read it for control / padding sizes.
==============================================================================================*/

typedef struct
{
    u32 line_size;      /* widget row height                                 */
    u32 widget_gap;     /* vertical gap between consecutive widgets          */
    u32 widget_pad;     /* horizontal content area padding                   */
    u32 win_title_h;    /* window title bar height                           */
    u32 win_border;     /* window / widget outline thickness                 */
    u32 checkbox_sz;    /* checkbox indicator side                           */
    u32 slider_knob_w;  /* slider draggable knob width                       */
    u32 min_cell_w;     /* floor a flex/fraction track shrinks to before overflow */
    u32 checkmark_pad;  /* inset of filled square inside the checkbox        */
    u32 cursor_w;       /* input text cursor width                           */
    u32 cursor_inset;   /* input text cursor top/bottom inset                */
    u32 win_rounding;   /* corner radius: windows / children / popups        */
    u32 widget_rounding;/* corner radius: control frames                     */
    u32 grab_rounding;  /* corner radius: slider knobs / scrollbar grabs     */
    u32 check_style;    /* checkbox/menu indicator: 0='v' tick, 1=disc, 2='X' (imgui_check_style_t) */
    u32 bullet_style;   /* bullet glyph: 0=disc, 1=square (imgui_bullet_style_t)              */
    u32 arrow_style;    /* directional arrow: 0=triangle, 1=chevron (imgui_arrow_style_t)     */
    u32 separator_style;/* separator rule: 0=solid, 1=dashed (imgui_separator_style_t)        */
    u32 progress_style; /* progress fill: 0=solid, 1=gradient (imgui_progress_style_t)        */
    u32 slider_knob;    /* slider knob: 0=bar, 1=circle (imgui_slider_knob_t)                 */

} imgui_metrics_t;

/*==============================================================================================
    Widget interaction (imgui_widget_core.c)

    The interaction class picked at the call site and the resolved per-frame result every widget
    drives its visuals from.  widget_behavior is the producer; every widget file consumes both.
==============================================================================================*/

/* Interaction class for a widget, selected at the call site.  Only the press-time
   behavior differs between widgets; everything else (hover/active/click) is uniform. */
typedef enum
{
    WIDGET_KIND_BUTTON    = 0,   /* press captures active; reports clicked   */
    WIDGET_KIND_DRAG      = 1,   /* press captures active; held for dragging */
    WIDGET_KIND_FOCUSABLE = 2,   /* press also claims keyboard focus         */

} widget_kind_t;

/* Result of one frame of interaction for a widget.  Every widget drives its
   visuals and value changes from these flags instead of touching s_interaction directly. */
typedef struct
{
    bool hover;    /* cursor is over the widget this frame                 */
    bool active;   /* primary button held with this widget as the target   */
    bool pressed;  /* primary button went down on the widget this frame    */
    bool clicked;  /* press + release completed with the cursor still over */
    bool focused;  /* widget owns keyboard input (focusable widgets)       */
    bool nav;      /* widget is the keyboard-nav cursor, highlighted       */

} widget_state_t;

/*==============================================================================================
    Persisted window record (behavior in imgui_window.c)

    One persisted window.  Geometry is owned here after the first appearance; the window pool that
    holds these lives in the bound context (imgui_context_t).  Behavior is in imgui_window.c.
==============================================================================================*/

/* Reserved high z-band for popup / tooltip overlays: they paint above every normal window (whose z
   comes from z_counter and never climbs this high), and a deeper popup above a shallower one.  A
   window stamped into this band is an overlay, never the OS-window frame -- window_is_native keys
   off it so a popup opened from an owned floater stays an anchored overlay, not a surface-filling
   native window.  imgui_popup.c stamps it (IMGUI_POPUP_Z_BASE + depth) before window_begin_ex. */
#define IMGUI_POPUP_Z_BASE   0x80000000u

typedef struct imgui_window_t
{
    imgui_id_t id;              /* id_hash(title); 0 = free slot                  */
    f32        x, y;            /* persisted top-left (updated by dragging)       */
    f32        w, h;            /* persisted dimensions                           */
    u32        z;               /* paint order: higher = more recently raised = in front */
    u32        viewport;        /* target surface index (0 = main swapchain); set via set_next_window_viewport */

    f32        scroll_y;        /* vertical scroll offset; 0 = top                */
    f32        scroll_x;        /* horizontal scroll offset; 0 = left             */
    f32        content_h;       /* total content height measured last frame       */
    f32        content_w;       /* total content width measured last frame        */

    bool       collapsed;       /* title-bar-only when set; toggled by the arrow  */
    bool       closed;          /* CLOSEABLE: hidden by the X until re-opened      */

    /* Re-open of a CLOSEABLE floater: closing it lets the abandoned-teardown free its OS window,
       reverting this record to viewport 0.  reopen_floater remembers it was a floater so the next
       begin re-spawns it.  The geometry is the floater's RESTORE (normal) state, sampled every
       frame it is not maximized -- so a floater closed while maximized re-opens maximized yet still
       restores to its previous normal size.  home_x/home_y are the restore client-corner screen
       position; restore_w/restore_h the restore size; reopen_maximized re-maximizes on re-spawn. */
    bool       reopen_floater;     /* re-spawn as a floater on the next begin             */
    bool       reopen_maximized;   /* floater was maximized -- re-maximize after re-spawn */
    i32        home_x, home_y;     /* saved restore (normal) client-corner screen pos     */
    f32        restore_w, restore_h; /* saved restore (normal) size                       */

    imgui_win_flags_t flags;    /* behavior flags supplied to begin_window        */

    /* Next-window channel bookkeeping (see set_next_window_pos / _size).  last_frame drives the
       "appearing" test; the allow masks track which conditions a queued value may still fire. */

    u32        last_frame;      /* frame index last begun; 0 = never begun        */
    u8         set_pos_allow;   /* conds still permitted to set position (imgui_cond_t bits) */
    u8         set_size_allow;  /* conds still permitted to set size              */

} imgui_window_t;

/*==============================================================================================
    Keyboard navigation state (driver in imgui_nav.c)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather
    than the mouse -- plus the menu-bar state machine layered on top of it.  The instance is a
    member of the bound context (imgui_context_t), reached through the s_nav alias.
==============================================================================================*/

typedef struct
{
    imgui_id_t   id;            // the highlighted item (keyboard cursor); persists across frames
    imgui_id_t   win;           // window/popup nav is scoped to (the hover_win analogue)
    imgui_rect_t ref_rect;      // id's rect last frame -- the directional scoring origin

    /* Two visual states, the Dear ImGui NavDisableHighlight split.  active means a nav cursor
       position exists -> the outline ring is drawn at id (and follows clicks), persisting even in
       mouse mode so it keeps its location.  highlight means the keyboard is the *active* instrument
       right now -> the nav item also takes the fill (like a hovered button), mouse hover is
       suppressed (so the two never double-fill), and the keyboard is captured.  A nav key sets both;
       a mouse move or click drops highlight (back to ring-only), leaving active. */

    bool         active;        // a nav cursor exists -> draw the ring (cleared rarely)
    bool         highlight;     // keyboard is the active instrument -> fill + hover-suppress

    i32          move_dir;      // directional request this frame (imgui_dir_t, or -1 for none)
    i32          tab;           // Tab linear move: +1 forward, -1 back, 0 none
    bool         activate;      // Enter/Space -> fire id like a click this frame

    bool         id_seen;       // id was emitted in win this frame (else it went stale)
    imgui_id_t   move_best;     // best-scored directional candidate this frame
    f32          move_score;    // its score (lower is better; reset to a large value each frame)
    imgui_rect_t move_rect;     // its rect -> next frame's ref_rect
    imgui_rect_t self_rect;     // id's own rect captured this frame (keeps ref_rect fresh)
    imgui_id_t   tab_first;     // first eligible item this frame (Tab wrap + first-focus)
    imgui_id_t   tab_prev;      // item emitted just before id (Shift+Tab target)
    imgui_id_t   tab_next;      // item emitted just after id (Tab target)
    bool         tab_take;      // the item just registered was id -> grab the next as tab_next

    /* Menu-bar navigation -- a small state machine layered on the nav cursor + popup stack, entered
       by Alt (toggle) or an Alt+letter mnemonic.  While active, nav lives either on the bar entries
       (in_menus false: win is the bar window, a highlighted entry drops its menu) or inside the open
       menu popups (in_menus true: win is the top popup).  Down/Enter descend, Up at a top item and
       Left/Esc ascend -- always landing back on menu_owner so closing a menu returns to the bar
       entry that opened it (not the first entry).  See imgui_nav.c + begin_menu. */

    imgui_id_t   bar_win;       // menu-bar window nav is driving; 0 = not in menu-bar mode
    bool         in_menus;      // menu mode: false = on the bar entries, true = inside the popups
    imgui_id_t   menu_owner;    // bar entry whose menu is open -- the ascend / close return target
    imgui_id_t   prev_win;      // nav target to restore when Alt toggles out of the menu bar
    imgui_id_t   prev_id;       // nav cursor to restore on Alt toggle-out (the last focus location)
    u8           mnemonic;      // pending Alt+letter mnemonic (uppercase ASCII); 0 = none

} nav_state_t;

/*==============================================================================================
    Layout-frame (stack in imgui_ctx.c)

    Every scrollable region (a window body or a begin_child box) pushes one frame.  The top frame
    owns the layout pen and the content column the leaf widgets emit into; the rest is the resolve
    context layout_pop_region needs to measure content and draw the region's scrollbars.
==============================================================================================*/

typedef struct
{
    f32 cursor_x,  cursor_y;    // layout pen, top-left of the next widget (scroll-biased)
    f32 content_x, content_w;   // widget-row left edge + available width
    f32 content_max_x;          // rightmost edge reached this frame -- drives hscroll

    /* Active row template (imgui_layout / row sugar).  Persists and repeats: each widget fills
       the next cell, wrapping to a fresh row of the same shape when the columns run out.  A
       region opens with the default -- one flex column, auto height -- so a plain vertical
       stack needs no layout call.  See imgui_layout_t in imgui.h for the unit rule. */

    imgui_layout_mode_t mode;                    // declared next-item methodology; NONE until a header

    u32         lay_ncols;                       // column count
    f32         lay_row_h;                       // flow row height: 0 = auto, >0 = pixels
    f32         lay_gap_x, lay_gap_y;            // inter-cell spacing (resolved to a number)
    u32         lay_nrows;                       // row count; 0 => flow mode, else grid
    f32         lay_cols[ IMGUI_LAYOUT_COLS ];   // source column units, kept so indent can re-resolve

    /* Field split (field_split / field_label_left): a labeled value widget splits its cell into a
       label track + a control track, resolved with the column unit rule.  side 0 = off (the
       label trails the control); 1 = label-left; 2 = label-right. */

    u8          lay_field_side;                  // imgui_label_side_t: 0 off, 1 left, 2 right
    f32         lay_field_label;                 // label track size   (overloaded unit)
    f32         lay_field_control;               // control track size (overloaded unit)

    /* Content alignment (align / layout.align): where a widget's natural-sized content sits in
       its cell.  Persists like the row template; 0 = LEFT | TOP (the original top-left). */

    u8          lay_align;                       // imgui_align_t flags

    /* Resolved cell geometry, computed once when a template is installed (the source track lists
       are not kept -- they are only needed to produce these).  Flow uses cellx/cellw for every
       row; grid uses cellx/cellw x rowy/rowh as the fixed matrix.  cols indexes [0,lay_ncols),
       rows [0,lay_nrows). */

    f32 cellx[ IMGUI_LAYOUT_COLS ];         // resolved cell left edges
    f32 cellw[ IMGUI_LAYOUT_COLS ];         // resolved cell widths
    f32 rowy [ IMGUI_LAYOUT_COLS ];         // resolved cell tops    (grid only)
    f32 rowh [ IMGUI_LAYOUT_COLS ];         // resolved cell heights (grid only)

    /* Iteration cursor.  Flow: (col) walks one row; a wrap advances cursor_y past row_h_cur and
       row_y/row_h_cur describe the live row.  Grid: (col,row) walk the pre-resolved matrix. */

    u32 col;                                // next column to emit (0 = at a row start)
    u32 row;                                // current grid row (with col, walks row-major)
    f32 row_y;                              // top of the current flow row
    f32 row_h_cur;                          // resolved height of the current flow row
    f32 content_y_max;                      // bottom of the content area -- grid band end

    /* same_line: pin the next widget to the previous item's line instead of breaking to a new row.
       prev_item is the last cell handed out; same_line() arms cont_line and sets cont_x to the
       continuation x (just past prev_item).  See widget_next_rect_w. */

    imgui_rect_t prev_item;                 // last cell emitted this region (same_line anchor)
    bool         cont_line;                 // next widget continues on prev_item's line
    f32          cont_x;                    // x at which the continued widget is placed

    /* Pack mode (bar / strip): a print run placing items along pack_dir at natural size -- or a
       pack_size override resolved against the space remaining on the current line -- with
       pack_nextline breaking to a fresh line.  pack_main is the running pen along the axis,
       pack_cross the current line's origin on the other axis, pack_line its max cross extent. */

    u8  pack_dir;                           // imgui_pack_dir_t: 0 horizontal (bar), 1 vertical (strip)
    f32 pack_main;                          // running main-axis pen (absolute)
    f32 pack_cross;                         // current line's cross-axis origin (absolute)
    f32 pack_line;                          // current line's max cross extent
    f32 pack_origin_main;                   // main-axis start, for the nextline reset
    f32 pack_size_next;                     // pending main-axis size unit; < 0 = unset (natural)

    /* Resolve context, set at push and read at pop. */

    imgui_id_t          region_id;          // base id for the region's scrollbar widget ids
    imgui_win_flags_t   flags;              // scroll policy bits (IMGUI_WIN_*SCROLL), reused
    imgui_rect_t        outer;              // the region box in screen space
    f32                 origin_x;           // unscrolled content origin -- measures content extent
    f32                 origin_y;
    f32                 view_w, view_h;     // gutter-adjusted visible extents (must match the bars)
    f32                 sb_w, sb_h;         // reserved gutter sizes (0 = no bar this frame)
    bool                show_v, show_h;     // a bar is shown this axis
    bool                pushed_clip;        // a draw clip was pushed (balance at pop)

    /* Persistent scroll state, owned by the caller (window record or region pool entry). */

    f32*                scroll_x;
    f32*                scroll_y;
    f32*                pcontent_w;         // write-back: measured content extent for next frame
    f32*                pcontent_h;

    imgui_rect_t        parent_clip;        // s_build.clip_rect to restore at pop
    u32                 id_restore;         // id-scope depth to restore at pop (see id stack below)

    /* Child edge-resize (begin_child CHILD_RESIZE_*): the armed/hot edges of this child's border
       and the s_build.win_resize_hot to restore at end_child.  begin_child sets both (0 for a
       non-resizeable child); end_child bolds child_resize_edge and restores the saved hot, so a
       hot edge suppresses body widgets only while inside this child, never its siblings. */

    u8                  child_resize_edge;       // hot/armed resize edges for this child (0 = none)
    u8                  child_resize_saved_hot;  // s_build.win_resize_hot to restore at end_child

} layout_frame_t;

/*==============================================================================================
    Persistent region state (imgui_layout_region.c)

    A begin_child region's scroll offset and last-measured content size, kept across frames in the
    keyed state pool (imgui_ctx.c), keyed by region id.  Windows keep these inline in imgui_window_t.
==============================================================================================*/

typedef struct
{
    f32 scroll_x, scroll_y;   /* persisted scroll offset (fractional: scrollbar drag is t * max_scroll) */
    f32 content_w, content_h; /* content extent measured last frame (f32* passed to layout_push_region)  */
    i16 user_w, user_h;       /* user-resized size in pixels; 0 = none, use the passed w/h              */

} imgui_region_t;

/*==============================================================================================
    Popup stack (imgui_ctx.c; driver in imgui_popup.c)

    A popup is a top-level overlay begun while a parent window is still open but laid out, clipped,
    and painted independent of it.  imgui_overlay_save_t snapshots exactly the cross-cutting state
    begin/end_window mutate so end_popup can restore the parent verbatim.  The open set is a stack
    ordered parent -> child, held in imgui_context_t.
==============================================================================================*/

typedef struct
{
    /* Flat window context (s_build) the popup's begin_window clobbers. */
    imgui_id_t             win_id;
    const char*            win_title;
    bool                   win_collapsed;
    imgui_win_flags_t      win_flags;
    f32                    win_title_h;
    u8                     win_resize_hot;
    bool                   win_grip_hot;
    struct imgui_window_t* cur_win;
    f32                    win_x, win_y, win_w, win_h;
    imgui_rect_t           clip_rect;     // s_build.clip_rect (interaction clip)

    /* Draw cursor + the parent's top layout frame. */

    u32                    sort_key;      // s_draw.cur_z
    u32                    viewport;      // s_draw.cur_vp (target surface routing)
    bool                   had_parent;    // a layout region was open (parent frame valid)
    layout_frame_t         parent_frame;  // the parent's top frame, restored after the popup

} imgui_overlay_save_t;

typedef struct
{
    imgui_id_t   id;                // popup window id (salted; matches s_build.win_id / hover_win)
    bool         modal;             // blocks input behind it + dims the background
    f32          anchor_x;          // open point -- where a non-modal popup is placed
    f32          anchor_y;
    u32          open_frame;        // frame open_popup ran -- "appearing" detection
    u32          begun_frame;       // last frame begin_popup ran -- drives stale-close
    imgui_rect_t rect;              // on-screen rect last frame -- drives click-outside
    imgui_overlay_save_t saved;     // parent context to restore at end_popup

} imgui_popup_t;

/*==============================================================================================
    Keyed state pool + per-context retained state (imgui_ctx.c)

    The single store a widget uses to keep a few bytes alive across frames, keyed by its id (a
    region's scroll, a tree node's open flag, a combo's popup state), plus the per-context id-salt
    and frame clock that stamp and age it.  An open-addressing hash table; see imgui_ctx.c for the
    reclamation contract.
==============================================================================================*/

typedef struct
{
    imgui_id_t  id;                         // 0 = empty slot
    u32         seen_frame;                 // frame last touched -- drives stale reclamation
    u8          data[ IMGUI_STATE_CAP ];    // payload; naturally 4-byte aligned (follows two u32 fields)

} imgui_state_slot_t;

typedef struct
{
    /* Per-context id namespace seed.  XOR'd into id_hash's FNV basis, so the same string hashes to a
       distinct id in each context and every id_combine built on it inherits the offset.  Keeps the
       ambient hover / active / focus ids -- compared globally across contexts -- from confusing a
       widget in one viewport with an identically-named widget in another.  0 is the default
       context's namespace and leaves id_hash byte-identical to the unsalted hash. */
    u32 id_salt;

    u32  frame;           /* monotonic frame index, bumped each ctx_begin this context is built */
    bool wants_redraw;    /* set by imgui_anim_f32 while mid-transition; cleared at ctx_begin */

    imgui_state_slot_t* state;       /* open-addressed keyed per-widget state; points into context alloc */
    u32                 state_count; /* capacity, power of two */
    u32                 state_mask;  /* state_count - 1, for bucket masking */

} imgui_retained_t;

/*==============================================================================================
    Render viewport (behavior in imgui_render.c)

    One render surface a context drives: GPU buffers + a color target, the OS window hosting it, and
    the routing/ownership bookkeeping for host-provided vs imgui-owned (torn-off floater) surfaces.
    [0] is the main swapchain; the rest are floaters.  Held by value in imgui_context_t.viewports.
==============================================================================================*/

struct imgui_dock_node_t;   /* the dock tree node -- defined in full after imgui_viewport_t below */

typedef struct
{
    rhi_buffer_t  vb;           // CPU_TO_GPU vertex buffer, one region per frame-in-flight
    rhi_buffer_t  ib;           // CPU_TO_GPU index buffer (u16), one region per frame-in-flight

    /* Color target flush paints into: RHI_SWAPCHAIN_COLOR for the main viewport, a floater's own
       swapchain image otherwise.  Held per viewport so flush is target-agnostic. */
    rhi_texture_t target;

    /* OS window this surface is hosted by (app win_id_t), or -1 (APP_WIN_INVALID) if unassociated.
       Input routing maps a mouse event's win_id to this surface so the cursor's host viewport is
       known -- a window only hover-tests when the cursor is in the OS window hosting its viewport. */
    i32 win_id;

    /* rhi context driving this surface's swapchain (RHI_CTX_INVALID if none).  Only set for an
       imgui-OWNED surface (a torn-off floater imgui spawned): flush of a host-provided surface
       resolves RHI_SWAPCHAIN_COLOR from the host's cmd, so the host viewport leaves this invalid.
       An owned surface has no host driving it -- imgui runs frame_begin/end on this ctx itself. */
    i32 rhi_ctx;

    /* true when imgui created this surface's OS window + rhi context (tear-off floater) and must
       therefore destroy them.  false for the host-provided main surface (index 0) and any surface
       the host opened via viewport_open -- imgui frees only the GPU buffers for those, never the
       window/context it does not own. */
    bool owned;

    /* Set when the user closes an owned floater's OS window (APP_EV_WIN_CLOSE): the surface is torn
       down at the next update_platform_windows, a safe point between the build and the present, so
       no in-flight draw list references a surface being freed.  Ignored for non-owned surfaces. */
    bool pending_close;

    /* Drawable size of this surface in pixels.  Set by the host (viewport 0 from frame_begin, floaters
       via viewport_resize) BEFORE the build so begin_window clips its windows against THIS surface's
       extent, not the main window's.  0 = unset -> begin_window falls back to the main display size
       (single-window behavior).  Distinct from the win_w/win_h passed to flush, which only sets the
       GPU viewport/scissor clamp at submit time; the clip baked into each draw command is built here. */
    i32 disp_w, disp_h;

    /* Top band (pixels) drawn by this surface's native host caption (the IMGUI_WIN_NATIVE shell
       window's title bar height), published each frame by that shell.  window_clamp keeps non-native
       windows' top edge at or below this inset so their title bars stay grabbable above the drawn
       chrome band.  0 until first published (no native shell or default OS-chrome main window).
       Sticky: NOT cleared each frame -- persists from the last frame the native shell was active so
       update_platform_windows always has a valid top bound regardless of build ordering. */
    f32 caption_inset;

    /* Docking seam.  NULL = free-float placement (today's behavior, including the main viewport's
       overlapping windows); non-NULL = a dock tree tiling/tabbing the windows on this surface.
       Inert until docking lands -- a documented placement hook, no machinery yet. */
    struct imgui_dock_node_t* dock_root;

} imgui_viewport_t;

/*==============================================================================================
    Dock node (behavior in imgui_dock.c)

    One node of a viewport's dock tree -- the machinery behind the dock_root seam above.  A node is
    either a LEAF (split == DOCK_SPLIT_NONE), which tabs one or more windows into a single region, or
    an INTERNAL split (DOCK_SPLIT_X / _Y), which divides its rect between two children at `ratio` with
    a draggable splitter between them.  Nodes live in a fixed per-context pool (imgui_context_t.
    dock_nodes) so child / parent pointers stay valid across frames; a freed slot has id == 0.

    rect / content are resolved every frame by dock_node_layout from the viewport extent down: rect is
    the node's whole box, content is the leaf's body below its tab strip (where the active window draws).
==============================================================================================*/

typedef enum
{
    DOCK_SPLIT_NONE = 0,    /* leaf -- tabs windows; child[] unused                 */
    DOCK_SPLIT_X,           /* internal -- vertical split, children side by side    */
    DOCK_SPLIT_Y,           /* internal -- horizontal split, children top / bottom  */

} imgui_dock_split_t;

typedef struct imgui_dock_node_t
{
    imgui_id_t id;                          /* stable node handle; 0 = free pool slot            */
    u32        viewport;                    /* surface (viewport index) this tree belongs to     */
    u8         split;                       /* imgui_dock_split_t: NONE = leaf, else internal     */
    f32        ratio;                       /* child[0]'s fraction of the split axis (0.5 default) */

    struct imgui_dock_node_t* parent;       /* owning split, or NULL for the tree root           */
    struct imgui_dock_node_t* child[ 2 ];   /* internal only (NULL on a leaf)                    */

    /* Leaf payload: the windows tabbed into this node.  Names are copied at dock time so the tab
       bar is self-sufficient (no dependence on a window emitting this frame or its title lifetime). */

    imgui_id_t tabs [ IMGUI_DOCK_TABS_MAX ];
    char       names[ IMGUI_DOCK_TABS_MAX ][ IMGUI_DOCK_NAME_CAP ];
    u32        tab_count;
    u32        active_tab;                   /* index of the visible tab                          */

    imgui_rect_t rect;                       /* whole node box, resolved this frame               */
    imgui_rect_t content;                    /* leaf body below the tab strip (active window's rect) */

} imgui_dock_node_t;

/*==============================================================================================
    imgui_context_t -- the bound per-context retained state ("bind and use").

    A context is the emission session the code binds once and emits ALL its windows into; it owns
    the state that must persist between frames for that UI.  Every retained access resolves through
    g_ctx via the aliases in imgui_ctx.c -- s_retained, s_nav, the popup open-set -- so switching
    contexts is a single pointer assignment (ctx_bind): no copy, no backup/restore.

    Ambient state (s_interaction) and frame scratch (s_build, the stacks, s_draw) stay global for now;
    s_io (hardware input snapshot) is always shared.  The `listening` flag gates whether a bound context
    receives hover / click / nav updates -- a deaf context renders but returns inert widget state.
==============================================================================================*/

typedef struct imgui_context_t
{
    imgui_retained_t    retained;          // id salt, frame clock, keyed state pool (ptr into alloc)
    nav_state_t         nav;               // nav cursor location + menu-bar mode
                                           
    imgui_popup_t*      popups_open;       // open popup set, ordered parent -> child; ptr into alloc
    u32                 popup_open_count;  // live open count
    u32                 popup_depth;       // capacity (max nesting depth) 
                                            
    imgui_window_t*     windows;           // persisted window records; ptr into alloc
    u32                 window_count;      // live records in the pool
    u32                 max_windows;       // capacity
    imgui_window_t      window_scratch;    // transient fallback when the pool is full; stays embedded
    u32                 z_counter;         // monotonic paint-order dispenser
                                           
    imgui_viewport_t*   viewports;         // render surfaces: [0]=main swapchain; ptr into alloc
    u32                 viewport_count;    // high-water slot count (compacted on close; iterate [0, count))
    u32                 max_viewports;     // capacity
                                            
    imgui_dock_node_t*  dock_nodes;        // dock-tree node pool; NULL when max_dock_nodes == 0
    u32                 dock_node_count;   // high-water slot count in the pool
    u32                 dock_id_seq;       // monotonic node-id dispenser (0 = none)
    u32                 max_dock_nodes;    // capacity; 0 = docking disabled
                                            
    bool                listening;         // true: context receives hover/click/nav input this frame
    void*               _alloc;            // heap block; NULL for the static default context (slot 0)

} imgui_context_t;

/*==============================================================================================
    Table state types (imgui_table.c)

    Phase 1 only: per-frame context + a small persistent pool for column widths / sort state.
    The pool is module-static (not per-context) and LRU-reclaimed by seen_frame, the same
    contract as the keyed widget state pool.  The table context is a single active slot for now;
    nested tables are a future addition.
==============================================================================================*/

/* Per-column setup data filled by table_setup_column before the first row. */
typedef struct
{
    char                    label[ 32 ];   /* display name for headers_row (future)       */
    imgui_table_col_flags_t flags;         /* FIXED / STRETCH / NO_RESIZE / etc.          */
    f32                     init_w;        /* 0 = stretch (==1 fill); >1 = fixed pixels   */

} imgui_table_col_t;

#define IMGUI_TABLE_POOL_CAP 32   /* max concurrent distinct tables tracked across frames */

/* Per-table persistent state: column widths, sort choice, and scroll position survive frames. */
typedef struct
{
    imgui_id_t id;
    u32        seen_frame;
    f32        col_w[ IMGUI_TABLE_COLS_MAX ];   /* 0 = use column's init_w / default */
    i8         sort_col;                        /* -1 = unsorted                     */
    i8         sort_dir;                        /* 0 = ascending, 1 = descending     */

    /* Scroll state + measured content extent for a scrolling body (IMGUI_TABLE_SCROLL_*).
       The layout region reads scroll_* as the pen bias and writes content_* back at pop; both
       must persist across frames for the two-pass gutter / clamp logic to settle. */
    f32        scroll_x, scroll_y;
    f32        content_w, content_h;

} imgui_table_persist_t;

/* Per-frame active table context.  One table open at a time (no nesting yet). */
typedef struct
{
    imgui_id_t              id;
    imgui_table_flags_t     flags;
    i32                     ncols;
    imgui_table_col_t       cols[ IMGUI_TABLE_COLS_MAX ];
    i32                     col_setup_n;   /* number of table_setup_column calls so far */

    /* Resolved column geometry (screen space), set once in table_begin. */
    f32                     col_x[ IMGUI_TABLE_COLS_MAX ];
    f32                     col_w[ IMGUI_TABLE_COLS_MAX ];

    /* Iteration state. */
    i32                     cur_col;       /* -1 before first table_next_column this row */
    i32                     cur_row;       /* -1 before first table_next_row             */
    f32                     row_top;       /* screen-space top of the current row        */
    f32                     row_h;         /* current row height in pixels               */

    imgui_rect_t            outer_rect;    /* full table box in screen space             */
    imgui_rect_t            body_rect;     /* content area inside the opened region      */
    f32                     header_h;      /* header strip height; 0 if no header        */

    /* Set true once the body region has been pushed (either by table_headers_row or
       the first table_next_row).  Guards layout_pop_region in table_end. */
    bool                    header_done;

    /* The header is drawn last (as chrome, like a window title bar) so it overpaints rows that
       scrolled under it.  table_headers_row only does the sort interaction up front and records
       what the deferred draw needs: whether a header exists and which column is hot / active. */
    bool                    want_header;   /* table_headers_row was called this frame      */
    i8                      hdr_hot;       /* column under the cursor (-1 none)            */
    i8                      hdr_act;       /* column being pressed     (-1 none)           */

    /* Column-resize feedback: index of the interior boundary (between col i and i+1) that is hot
       or being dragged, drawn as a highlight line in table_end.  -1 = none.  See IMGUI_TABLE_RESIZABLE. */
    i8                      resize_hot;

    /* Set true in table_headers_row when the user clicks a sort-active column header.
       Cleared by table_get_sort_specs.  Automatically false each new frame (s_tab memset). */
    bool                    sort_dirty;

    /* s_build.clip_rect on entry, restored when the one table clip is popped in table_end. */
    imgui_rect_t            saved_clip;

    imgui_table_persist_t*  persist;

} imgui_table_t;

/*==============================================================================================
    Cross-file forward declarations

    A handful of helpers are called from a file included BEFORE the file that defines them (the
    unity TU resolves the static at link of the single object).  Declaring them here removes the
    hand-placed forward declarations that used to sit in imgui.c.
==============================================================================================*/

/* The mouse-input path (imgui_input.c) resolves an event's app win_id to the viewport hosting it,
   but the viewport pool lives on g_ctx (imgui_ctx.c) included later.  Defined after g_ctx. */
static u32 viewport_index_for_window( i32 win_id );

/* OS resize / close events for an imgui-OWNED floater are serviced against the viewport pool, so
   imgui_event (imgui_input.c) delegates them here.  Defined in imgui_frame.c after g_ctx; returns
   true when win_id is an owned viewport (event consumed). */
static bool imgui_owned_window_event( const app_event_t* ev );

/* The window chrome (imgui_widget_window.c) is included BEFORE the dock machinery (imgui_dock.c),
   but begin_window / end_window must route a docked window into its node.  These two are defined in
   imgui_dock.c and forward-declared here so the earlier file can call them: the lookup that decides
   whether a window is docked, and the tab-strip + border chrome a docked window draws in place of a
   title bar.  dock_window_chrome reads the current window rect from s_build. */
static imgui_dock_node_t* dock_find_window_node( imgui_id_t win );
static void               dock_window_chrome( imgui_dock_node_t* node );

/* Phase-2 mouse gestures, also defined in imgui_dock.c and called from imgui_widget_window.c: detect
   + preview a drop target while a free window is dragged over a dockspace, and commit it on release. */
static void dock_drag_detect( imgui_id_t win_id, imgui_window_t* win );
static void dock_drag_commit( imgui_id_t win_id, const char* title );

/*==============================================================================================
    Shared stateless helpers

    Small pure scalar/geometry helpers used across both translation units (the UI unit and the
    render backend unit -- imgui_draw.c needs rect_intersect for clip nesting).  static inline so
    each TU gets its own copy with no linkage; they touch nothing but their arguments.
==============================================================================================*/

/* Clamp t to [0,1] -- the saturate used by slider + scrollbar drag mapping. */
static inline f32
saturate( f32 t ) { return t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t ); }

/* Clamp v to [lo,hi]. */
static inline f32
clampf( f32 v, f32 lo, f32 hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

/* Overlap of two rects (zero-size when they do not overlap).  Nested regions intersect their
   clip with the parent so a child never scissors or hit-tests past it. */
static inline imgui_rect_t
rect_intersect( imgui_rect_t a, imgui_rect_t b )
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = ( a.x + a.w < b.x + b.w ) ? a.x + a.w : b.x + b.w;
    f32 y1 = ( a.y + a.h < b.y + b.h ) ? a.y + a.h : b.y + b.h;
    f32 w  = x1 - x0 > 0.0f ? x1 - x0 : 0.0f;
    f32 h  = y1 - y0 > 0.0f ? y1 - y0 : 0.0f;
    return ( imgui_rect_t ){ x0, y0, w, h };
}

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_INTERNAL_H
