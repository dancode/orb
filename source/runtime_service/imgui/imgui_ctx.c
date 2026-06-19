/*==============================================================================================

    runtime_service/imgui/imgui_ctx.c -- Immediate-mode context state and per-frame drivers.

    Declares all persistent ambient and frame-scratch state (s_interaction, s_build, nav_state_t,
    layout_frame_t, imgui_context_t) and drives the per-frame lifecycle via ctx_new_frame.

    ID hashing and the keyed state pool (id_hash, id_combine, id_seed/push/pop, imgui_state_get,
    IMGUI_STATE) are in imgui_ctx_id.c, included just after this file.

    Public IO accessors (imgui_want_capture_*, imgui_is_key_*, imgui_is_mouse_*, etc.) are in
    imgui_ctx_io.c, also included after this file.

    Included by imgui.c after imgui_input.c so s_io is in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* imgui_window_t is defined early in imgui.c (it is embedded by value in the context below);
   s_build.cur_win points at a pool record so end_window can write scroll_y / content_h back. */

/* Ambient interaction state -- the one user's live hover / active / focus, persisting across
   frames.  There is one pointer, one keyboard, one mouse, so none of this is ever duplicated per
   viewport: when the multi-context model lands this stays a single global the focused context is
   bound to (an adapter), never per-context state.  Tier: ambient singular.  See the three-tier
   note in imgui.c. */

static struct
{
    imgui_id_t  hover_id;       // widget under the cursor this frame (rebuilt each frame)
    imgui_id_t  active_id;      // widget with the mouse button held (drag / hold)
    u8          active_button;  // which button holds active_id (0=left); reset to 0 on release
    imgui_id_t  focused_id;     // widget that owns keyboard input

    /* Auto-repeat timing for the held button (IMGUI_ITEM_BUTTON_REPEAT).  Only one widget is active
       at a time, so a single timer suffices: repeat_t accumulates held time since the last fire, and
       repeat_on flips true once the initial delay has elapsed (switching to the faster rate).  Both
       are reset on the press frame, so a new button starts its own cadence. */
    f32         repeat_t;
    bool        repeat_on;

    /* Window occlusion is resolved one frame deferred: the single window the cursor is
       over (front-most by z) is only known after every window has been submitted.
       Each begin_window nominates itself into next_hover_win; ctx_new_frame promotes it to
       hover_win.  Next frame a window compares its id against hover_win at entry -- if it
       isn't the hover window it cannot be hit, so it (and all its widgets) skip hit-testing
       entirely.  Only the hover window does widget hit-testing, and within one window
       widgets don't overlap, so widget hover can be resolved immediately (no deferral). */

    imgui_id_t  hover_win;      // the window the cursor is over (resolved last frame)
    imgui_id_t  next_hover_win; // front-most window nominee gathered this frame
    u32         next_hover_win_z;

} s_interaction;

/* Frame-build scratch -- the "where am I emitting right now" context, rebuilt every frame as the
   widget tree is walked.  Nothing here survives begin_frame: it is set and repopulated by the
   begin_window / begin_child / widget calls, never read across a frame boundary.  Because contexts
   build sequentially on one thread, this stays a single global builder reused by each context in
   turn rather than per-context state.  Tier: frame scratch. */

static struct
{
    imgui_id_t  last_item_id;   // id of the most recent widget emitted -- context-menu / tooltip anchor

    u32         cur_viewport;       // ambient viewport for new-window inheritance (updated per window emitted)

    imgui_id_t  win_id;             // id of the window currently between begin/end_window
    const char* win_title;          // title string, cached for end_window's deferred chrome
    bool        win_collapsed;      // current window is collapsed (title bar only this frame)
    imgui_win_flags_t win_flags;    // current window's behavior flags (begin_window arg)
    f32         win_title_h;        // current window's title bar height (0 if NOTITLEBAR)
    u8          win_resize_hot;     // resize edges hot this frame -- suppresses widget hover
    bool        win_grip_hot;       // cursor over the CAN_AUTOSIZE grip -- suppresses widget hover
    struct imgui_window_t* cur_win; // persisted window record; scroll write-back target

    f32  win_x, win_y;        // current window top-left (outer frame)
    f32  win_w, win_h;        // current window dimensions

    imgui_rect_t menubar_rect; // reserved menu-bar strip for the current window (WIN_MENUBAR); begin_menu_bar fills it

    /* Layout pen + scroll region state now live on the layout-frame stack below; the
       window is just the root frame.  s_build keeps only the cross-cutting per-frame
       context the chrome and widgets read regardless of which region is active. */

    imgui_rect_t clip_rect;   // active interaction clip -- widget hover is gated by it
    bool         wheel_used;  // a region consumed the wheel this frame (innermost wins)

    /* Item flags -- the push-model behavior set a widget reads at emit time (see imgui_item_flags_t).
       item_flags is the merged top of the push/pop stack; next_set / next_val are the one-shot
       override for the next widget (which bits it controls + their values); cur_item_flags is the
       value resolved for the widget currently being emitted, latched by item_flags_resolve and read
       by widget_behavior and the widgets.  All reset to 0 each frame. */

    imgui_item_flags_t item_flags;       // merged top-of-stack item flags
    imgui_item_flags_t next_set;         // bits the next-item override controls
    imgui_item_flags_t next_val;         // their values
    imgui_item_flags_t cur_item_flags;   // flags resolved for the item being emitted

    /* Combo dropdown coordination (imgui_widget_combo.c).  begin_combo sets combo_open true while
       emitting its popup body; a selectable clicked in that body sets combo_item_clicked, and
       end_combo closes the dropdown when it sees it -- so picking an item dismisses the combo with
       no caller code, exactly as a selection should.  Both are scoped to the combo body and reset
       each frame as a safety net. */

    bool               combo_open;          // a combo dropdown body is currently being emitted
    bool               combo_item_clicked;  // a selectable in that body was clicked this frame

    /* Keyboard navigation state lives in its own subsystem struct (nav_state_t s_nav, below) so
       the per-frame build scratch does not bloat it.  See imgui_nav.c for the driver and
       nav_item_register (imgui_widget_core.c) for the per-item seam. */

} s_build;

/*----------------------------------------------------------------------------------------------
    Keyboard navigation state (s_nav)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather
    than the mouse -- plus the menu-bar state machine layered on top of it.  The type is defined
    here; the instance is a member of the bound context (imgui_context_t, below) reached via the
    s_nav alias, apart from the ambient s_interaction / frame-scratch s_build.  imgui_nav.c drives
    it and nav_item_register (imgui_widget_core.c) is the per-item seam.

    `win` is the window or popup nav is scoped to, chosen each frame the way hover_win is (a popup
    captures it while open).  Movement is resolved one frame deferred against `ref_rect`, exactly
    like hover_win lags the cursor: the request is set at nav_new_frame, every item in `win`
    registers itself through widget_behavior during emission, and the winner is committed at the
    next nav_new_frame.
----------------------------------------------------------------------------------------------*/

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

/* s_nav lives in the bound context (imgui_context_t, below) and is reached through the g_ctx alias,
   so each context keeps its own nav cursor location. */

/*----------------------------------------------------------------------------------------------
    Item-flag stack

    push_item_flag saves the current merged value here and pop_item_flag restores it, so a push
    nests cleanly regardless of which bits it touched.  An over-deep push aliases the top slot and
    is still counted truthfully, mirroring the id / layout stacks, so push/pop stay paired.
----------------------------------------------------------------------------------------------*/

#define IMGUI_ITEM_FLAG_DEPTH 16

static imgui_item_flags_t s_item_flag_stack[ IMGUI_ITEM_FLAG_DEPTH ];
static u32                s_item_flag_sp;

/* Disabled items draw at this opacity (the rest of the dim is in the draw list's global alpha). */
#define IMGUI_DISABLED_ALPHA 0.5f

/* Push: save the current merged flags, then set or clear `flag` in the live set. */
static void
item_flag_push( imgui_item_flags_t flag, bool enable )
{
    if ( s_item_flag_sp < IMGUI_ITEM_FLAG_DEPTH )
        s_item_flag_stack[ s_item_flag_sp ] = s_build.item_flags;
    ++s_item_flag_sp;    /* count truthfully so push/pop stay paired even past the cap */

    if ( enable ) s_build.item_flags |=  flag;
    else          s_build.item_flags &= ~flag;
}

/* Pop: restore the merged flags saved by the matching push. */
static void
item_flag_pop( void )
{
    if ( s_item_flag_sp == 0 ) return;
    --s_item_flag_sp;
    u32 i = s_item_flag_sp < IMGUI_ITEM_FLAG_DEPTH ? s_item_flag_sp : IMGUI_ITEM_FLAG_DEPTH - 1;
    s_build.item_flags = s_item_flag_stack[ i ];
}

/* Next-item override: mark `flag` as controlled for the next widget, with its on/off value.
   Consumed (and cleared) by item_flags_resolve when that widget emits -- no pop needed. */
static void
item_flag_next( imgui_item_flags_t flag, bool enable )
{
    s_build.next_set |= flag;
    if ( enable ) s_build.next_val |=  flag;
    else          s_build.next_val &= ~flag;
}

/* Resolve the flags for the item now emitting: the stack value with the one-shot next-item override
   applied over it (the override wins on the bits it controls), then clear the override.  Latches the
   result for widget_behavior / widgets to read, and sets the draw alpha so a disabled item dims with
   no per-widget code.  Called once per item from widget_next_rect_w (the universal emit seam). */
static imgui_item_flags_t
item_flags_resolve( void )
{
    imgui_item_flags_t f = ( s_build.item_flags & ~s_build.next_set ) | ( s_build.next_val & s_build.next_set );
    s_build.next_set = 0;
    s_build.next_val = 0;
    s_build.cur_item_flags = f;

    /* Same seam for the style stacks: promote any next_style_* override into the active per-item
       layer so it applies for this widget's whole draw, then clears for the following one. */
    style_item_commit();

    draw_set_alpha( ( f & IMGUI_ITEM_DISABLED ) ? IMGUI_DISABLED_ALPHA : 1.0f );
    return f;
}

/* Clear the per-item state before chrome runs.  Window/child borders, scrollbars, titlebars, and
   the collapse arrow are not items -- they never go through widget_next_rect_w, so without this they
   would inherit whatever the last body widget latched (a disabled trailing widget would dim the
   border and deaden the scrollbar).  Called at the chrome seams (begin/end_window, begin_child,
   layout_pop_region) so chrome always interacts undisabled and paints opaque. */
static void
item_flags_chrome_reset( void )
{
    s_build.cur_item_flags = IMGUI_ITEM_NONE;
    draw_set_alpha( 1.0f );
    style_chrome_reset();   /* drop lingering next_style_* overrides; keep the push/pop stack */
}

/*----------------------------------------------------------------------------------------------
    Layout-frame stack

    - Every scrollable region (a window body or a begin_child box) pushes one frame.  
    - The top frame owns the layout pen and the content column the leaf widgets emit into.
    - The rest of the struct is the resolve context layout_pop_region needs to measure 
      content and draw the region's scrollbars.
    - The pen fields used to live flat in the window context; nesting moved them here.

    - Memory is just the fixed array -- a frame carries only what is needed to emit widget 
      rects and resolve scroll at pop, so a deep nesting costs nothing beyond these slots.
----------------------------------------------------------------------------------------------*/

#define IMGUI_LAYOUT_DEPTH 8    // max nested scroll regions (windows or children)

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

static layout_frame_t s_layout_stack[ IMGUI_LAYOUT_DEPTH ];
static u32            s_layout_sp;   // active frame count; top = s_layout_sp - 1

/* Top layout frame.  Valid between a begin_window/begin_child and its matching end.  When the
   stack is empty (a caller emitted a widget into a collapsed window despite the false return)
   slot 0 is returned instead of indexing out of bounds -- the stray widget draws into whatever
   the last frame's root region left there rather than crashing.  The read index is also clamped
   to the top slot so an over-deep nesting (capped in layout_push_region) never reads past the
   array. */

static layout_frame_t*
lf( void )
{
    u32 i = s_layout_sp ? s_layout_sp - 1 : 0;
    if ( i >= IMGUI_LAYOUT_DEPTH ) i = IMGUI_LAYOUT_DEPTH - 1;
    return &s_layout_stack[ i ];
}

/*----------------------------------------------------------------------------------------------
    Popup stack

    The set of currently-open popups *is* a stack, ordered parent -> child: index 0 is the
    top-level popup, each deeper index a popup opened while inside the one above.  This single
    array is the source of truth for open / close, nesting, and the click-outside policy; the
    popups themselves are rendered as ordinary windows on a reserved high z-band (see imgui_popup.c).

      s_popup_open_count  -- persists across frames; the live open set is [0, count).
      s_popup_begin_count -- rebuilt each frame; the current popup nesting depth while emitting
                             (0 at top level, 1 inside one begin_popup, ...).  open_popup writes
                             a request at this depth; begin_popup matches its id against it.

    A popup is a *top-level overlay*: it is begun while a parent window is still open, but it must
    lay out, clip, and paint independent of that parent (a context menu escapes the window's
    bounds, paints above it, and must not disturb its layout pen).  imgui_overlay_save_t snapshots
    exactly the cross-cutting state begin/end_window mutate -- the flat window context, the
    interaction clip, the draw sort key, and the parent's top layout frame -- so end_popup can
    restore the parent verbatim.  The stack *counters* (layout / id / clip depth) are left intact
    and balance naturally through the normal push/pop, so no slot is ever reused or lost. */

#define IMGUI_POPUP_DEPTH 8     // max nested popups (menus + submenus)

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
    imgui_id_t   id;            // popup window id (salted; matches s_build.win_id / hover_win)
    bool         modal;         // blocks input behind it + dims the background
    f32          anchor_x;      // open point -- where a non-modal popup is placed
    f32          anchor_y;
    u32          open_frame;    // frame open_popup ran -- "appearing" detection
    u32          begun_frame;   // last frame begin_popup ran -- drives stale-close
    imgui_rect_t rect;          // on-screen rect last frame -- drives click-outside
    imgui_overlay_save_t saved; // parent context to restore at end_popup

} imgui_popup_t;

/* The open set (s_popups_open) and its live count (s_popup_open_count) are members of the bound
   context (imgui_context_t, below), reached through the g_ctx alias -- popups persist per context.
   s_popup_begin_count is per-frame scratch (rebuilt as begin/end_popup run) and stays a plain global. */
static u32           s_popup_begin_count;                 // current nesting depth (per frame)

/*----------------------------------------------------------------------------------------------
    Keyed state pool -- persistent per-id widget state.

    The single store a widget uses to keep a few bytes alive across frames, keyed by its id: a
    region's scroll offset, a tree node's open flag, a combo's popup state.  imgui_state_get hands
    back a stable, zero-on-create pointer to `size` bytes for `id`; the IMGUI_STATE( T, id ) sugar
    casts it to a typed struct.  The contract is the immediate-mode norm -- fetch your state every
    frame you are live and the pointer is stable; an id left unfetched goes cold and its slot is
    recycled, so there is nothing to free and nothing leaks.

    Storage is an open-addressing hash table keyed by id (ids already avalanche, so id & mask is a
    good bucket).  A lookup probes linearly from the home bucket to the first empty slot -- O(1) at
    the low load factor a UI runs at.  A slot untouched for more than one frame is a tombstone the
    next insert on its chain reclaims; reclamation only ever overwrites one occupied slot with
    another, never empties one, so no probe chain is broken and no sweep or free list is needed.
    The "more than one frame" gate (not "one or more") is deliberate: a slot seen last frame may
    still be revisited later this frame, so only entries two+ frames cold are fair game.
----------------------------------------------------------------------------------------------*/

#define IMGUI_STATE_SLOTS 512                       // open-addressing capacity (power of two)
#define IMGUI_STATE_MASK  ( IMGUI_STATE_SLOTS - 1 ) // bucket = id & mask
#define IMGUI_STATE_CAP   32                        // payload bytes per slot (max state struct)

typedef struct
{
    imgui_id_t id;          // 0 = empty slot
    u32        seen_frame;  // frame last touched -- drives stale reclamation
    union                   // force max alignment so any payload struct is correctly aligned
    {
        void* _p;
        f64   _d;
        u8    bytes[ IMGUI_STATE_CAP ];
    } data;

} imgui_state_slot_t;

/* ---- First slice of per-context retained state ----
   The keyed state pool and the frame clock that ages it are the first members of what will become
   imgui_context_t.  They move together because the clock only has meaning relative to the pool it
   stamps -- and, more pointedly, the clock must advance per context, not per app-frame: a context
   not rebuilt on a given frame must not tick, or its live entries would read as cold and be
   reclaimed (losing scroll / open state) while it is merely hidden.  Window / popup / combo
   "appearing" detection keys off the same per-context clock for the same reason.  Bundling them now
   makes the eventual lift into imgui_context_t a regrouping, not a rewrite.  Tier: per-context
   retained. */

typedef struct
{
    /* Per-context id namespace seed.  XOR'd into id_hash's FNV basis (below), so the same string
       hashes to a distinct id in each context and every id_combine built on it inherits the offset.
       This is what keeps the ambient hover / active / focus ids -- which are compared globally, across
       every context -- from confusing a widget in one viewport with an identically-named widget in
       another.  0 is the default context's namespace and leaves id_hash byte-identical to the
       unsalted hash, so a single-context build is unchanged. */
    u32 id_salt;

    u32 frame;                                       // monotonic frame index, bumped each new_frame this
                                                     //   context is built; stamps + ages the pool below
    imgui_state_slot_t state[ IMGUI_STATE_SLOTS ];   // open-addressed keyed per-widget state

} imgui_retained_t;

/*----------------------------------------------------------------------------------------------
    imgui_context_t -- the bound per-context retained state ("bind and use").

    A context is the emission session the code binds once and emits ALL its windows into; it owns the
    state that must persist between frames for that UI.  Every retained access in the module resolves
    through g_ctx via the aliases below -- s_retained (id salt + frame clock + keyed state pool),
    s_nav (the nav cursor location + menu mode), and the popup open-set -- so switching contexts is a
    single pointer assignment (ctx_bind): no copy, no backup/restore.

    The window pool (imgui_window.c) and the per-viewport surfaces (imgui_render.c) join here once
    their record types -- defined in later-included files -- can be folded in; until then they remain
    separate retained globals.

    Ambient state (one user: s_interaction, s_io) and frame scratch (s_build, the stacks, s_draw) are
    NOT per context -- they stay global and target whichever context is bound.
----------------------------------------------------------------------------------------------*/

typedef struct imgui_context_t
{
    imgui_retained_t retained;                          // id salt, frame clock, keyed state pool
    nav_state_t      nav;                               // nav cursor location + menu-bar mode
    imgui_popup_t    popups_open[ IMGUI_POPUP_DEPTH ];  // open popup set, ordered parent -> child
    u32              popup_open_count;                  // live open count

    imgui_window_t   windows[ IMGUI_MAX_WINDOWS ];      // persisted window records (imgui_window.c behavior)
    u32              window_count;                      // live records in the pool
    imgui_window_t   window_scratch;                    // transient fallback when the pool is full
    u32              z_counter;                         // monotonic paint-order dispenser

    imgui_viewport_t viewports[ IMGUI_MAX_VIEWPORTS ];  // render surfaces: [0]=main swapchain, rest floaters
    u32              viewport_count;                    // live viewports (imgui_render.c behavior)

} imgui_context_t;

/* The default context, bound at startup.  g_ctx is the one indirection every retained access goes
   through; the host points it at another context before emitting that context's windows. */
static imgui_context_t  s_default_context;
static imgui_context_t* g_ctx = &s_default_context;

#define s_retained         ( g_ctx->retained )
#define s_nav              ( g_ctx->nav )
#define s_popups_open      ( g_ctx->popups_open )
#define s_popup_open_count ( g_ctx->popup_open_count )
#define s_windows          ( g_ctx->windows )
#define s_window_count     ( g_ctx->window_count )
#define s_window_scratch   ( g_ctx->window_scratch )
#define s_z_counter        ( g_ctx->z_counter )

/* Bind the active context; every alias above resolves into it from here on.  NULL rebinds the
   default.  This is the whole multi-context seam -- no state is copied.  One context today. */
static void
ctx_bind( imgui_context_t* ctx )
{
    g_ctx = ctx ? ctx : &s_default_context;
}

/* Resolve an app win_id to the viewport index.  Slot index == win_id by construction;
   out-of-range ids fall back to the main surface (0).
   Forward-declared in imgui.c; called by the mouse-input path in imgui_input.c. */
static u32
viewport_index_for_window( i32 win_id )
{
    if ( win_id >= 0 && win_id < (i32)IMGUI_MAX_VIEWPORTS )
        return (u32)win_id;
    return 0;   /* invalid -> main swapchain surface */
}

/*----------------------------------------------------------------------------------------------
    Id-scope stack

    The top of this stack is the seed every widget id combines against, so identical labels in
    different scopes never collide.  Regions seed it automatically (layout_push_region pushes the
    region id, layout_pop_region restores), and push_id / pop_id add temporary levels for repeated
    widgets inside one region (e.g. list rows keyed by index).  Reset to empty each frame.

    Over-deep pushes alias the top slot rather than writing past the array, and id_seed clamps its
    read the same way -- mirroring the layout stack, so deep nesting degrades instead of crashing.
----------------------------------------------------------------------------------------------*/

#define IMGUI_ID_STACK_DEPTH 32

static imgui_id_t s_id_stack[ IMGUI_ID_STACK_DEPTH ];
static u32        s_id_sp;

/* id_seed, id_push, id_pop, id_hash, id_combine, imgui_state_get, and IMGUI_STATE are defined
   in imgui_ctx_id.c (included immediately after this file).  They reference s_id_stack/s_id_sp
   and s_retained (via g_ctx) which are defined here and visible in the unity build. */

/*----------------------------------------------------------------------------------------------
    rect_hit -- true when the mouse cursor (from s_io) is inside the given rect
----------------------------------------------------------------------------------------------*/

static bool
rect_hit( imgui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/* rect_intersect (rect overlap) is a shared geometry helper defined in imgui.c, ahead of the
   unity includes, so imgui_draw.c can use it for clip intersection too. */

/*----------------------------------------------------------------------------------------------
    nav_score_dir -- directional-move cost from the current nav item (cur) to a candidate (cand)
    along `dir`.  The Dear ImGui nav scorer in its simplest center-to-center form: project the
    displacement onto the move axis (primary) and the perpendicular axis (secondary); a candidate
    not on the correct side is rejected, and the rest are ranked by primary distance plus a weighted
    perpendicular penalty so the item most directly ahead wins.  Reads only rects, so it is agnostic
    to the layout mode that produced them.  Lower is better; a large value means "rejected".
----------------------------------------------------------------------------------------------*/

#define NAV_SCORE_REJECT 3.0e38f    /* effectively +inf -- candidate is not in the move direction */

static f32
nav_score_dir( imgui_rect_t cur, imgui_rect_t cand, imgui_dir_t dir )
{
    f32 ccx = cur.x  + cur.w  * 0.5f, ccy = cur.y  + cur.h  * 0.5f;
    f32 ncx = cand.x + cand.w * 0.5f, ncy = cand.y + cand.h * 0.5f;
    f32 dx  = ncx - ccx, dy = ncy - ccy;

    f32 prim, secd;   /* primary = distance along the axis; secondary = perpendicular offset */
    switch ( dir )
    {
        case IMGUI_DIR_UP:    prim = -dy; secd = dx < 0 ? -dx : dx; break;
        case IMGUI_DIR_DOWN:  prim =  dy; secd = dx < 0 ? -dx : dx; break;
        case IMGUI_DIR_LEFT:  prim = -dx; secd = dy < 0 ? -dy : dy; break;
        case IMGUI_DIR_RIGHT: prim =  dx; secd = dy < 0 ? -dy : dy; break;
        default:              return NAV_SCORE_REJECT;
    }
    if ( prim <= 0.0f ) return NAV_SCORE_REJECT;   /* behind / abreast -- not in this direction */
    return prim + secd * 2.0f;                     /* weight misalignment so straight-ahead wins */
}

/*----------------------------------------------------------------------------------------------
    window_nominate_hover -- begin_window calls this with its rect + z + host viewport.  Keeps the
    front-most (highest z) window the cursor is over; promoted to hover_win next frame.

    The cursor lives in exactly one OS window/surface at a time (s_io.mouse_viewport, resolved from
    the win_id on mouse events).  A window on any other surface cannot be under the cursor regardless
    of where its rect sits in its own surface's coordinate space, so it is rejected before the rect
    test -- this is the "physical window is a parent hover" rule: the surface must match first, then
    the window rect within it.  Without this, the polled cursor position (client coords of whatever
    window the mouse is in) would hit-test against identically-placed windows on every surface.
----------------------------------------------------------------------------------------------*/

static void
window_nominate_hover( imgui_id_t id, imgui_rect_t r, u32 z, u32 viewport )
{
    /* Surface gate first: the cursor must be in the OS window hosting this window's viewport. */
    if ( viewport != s_io.mouse_viewport )
        return;

    /* Cheap z test gates the rect_hit; window z is unique so > / >= are equivalent. */
    if ( z >= s_interaction.next_hover_win_z && rect_hit( r ) )
    {
        s_interaction.next_hover_win   = id;
        s_interaction.next_hover_win_z = z;
    }
}

/*----------------------------------------------------------------------------------------------
    ctx_new_frame -- reset per-frame hover state; call at the start of each frame
----------------------------------------------------------------------------------------------*/

static void
ctx_new_frame( void )
{
    /* Widget hover is rebuilt every frame by the hover window's widget calls; clear it now. */
    s_interaction.hover_id = IMGUI_ID_NONE;

    /* Fresh layout stack each frame; no region is open until a begin_window/begin_child.
       The interaction clip starts at the full display, and the wheel is unclaimed -- the
       innermost scrollable region the cursor sits in consumes it (claimed at region pop). */
    s_layout_sp           = 0;
    s_id_sp               = 0;       /* fresh id-scope stack; regions/push_id reseed it */
    s_build.wheel_used    = false;
    s_build.cur_viewport  = 0;       /* ambient viewport resets to primary each frame */

    /* Popup nesting depth is rebuilt as begin_popup / end_popup run; the open set persists. */
    s_popup_begin_count = 0;

    /* Combo body coordination is per-frame and re-set by begin/end_combo; clear as a safety net. */
    s_build.combo_open         = false;
    s_build.combo_item_clicked = false;

    /* Fresh item-flag state each frame: empty stack, no next-item override, nothing disabled. */
    s_item_flag_sp       = 0;
    s_build.item_flags     = IMGUI_ITEM_NONE;
    s_build.next_set       = IMGUI_ITEM_NONE;
    s_build.next_val       = IMGUI_ITEM_NONE;
    s_build.cur_item_flags = IMGUI_ITEM_NONE;

    /* Fresh style stacks each frame: working set re-seeded from the theme, stacks + next cleared. */
    style_new_frame();
    s_build.clip_rect   = ( imgui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    ++s_retained.frame;

    /* Promote the window the cursor was over last frame, then start a fresh nomination.
       hover_win lags the cursor by one frame -- the only deferral, and it is what lets the
       front-most window be known before any widget hit-tests this frame. */
    s_interaction.hover_win        = s_interaction.next_hover_win;
    s_interaction.next_hover_win   = IMGUI_ID_NONE;
    s_interaction.next_hover_win_z = 0;

    /* Release active_id once its initiating button is up.  Most grabs use the left button
       (active_button 0); a middle-button window move sets active_button 2 so it releases on
       the middle button instead.  Keep it alive on the release-edge frame (mouse_released)
       so widgets can still observe the press+release pair this frame; it clears on the
       following frame.  Resetting active_button to 0 on release means every left-button grab
       site needs no bookkeeping -- only the middle grab raises it. */
    u8 ab = s_interaction.active_button;
    if ( !s_io.mouse_down[ ab ] && !s_io.mouse_released[ ab ] )
    {
         s_interaction.active_id     = IMGUI_ID_NONE;
         s_interaction.active_button = 0;
    }

    /* Drop keyboard focus on any press; the widget under the cursor re-claims
       it the same frame (input_text sets focused_id from hover_id + press).
       A press on a button or empty space thus leaves focus cleared. */
    if ( s_io.mouse_pressed[ 0 ] )
         s_interaction.focused_id = IMGUI_ID_NONE;
}

/* Public IO accessors (imgui_want_capture_*, imgui_is_key_*, imgui_is_mouse_*, imgui_get_*)
   are defined in imgui_ctx_io.c, included immediately after imgui_ctx_id.c.
   They read s_interaction, s_nav, s_popup_open_count, s_build, s_io, and rect_hit --
   all visible in the unity build at that point. */

// clang-format on
/*============================================================================================*/
