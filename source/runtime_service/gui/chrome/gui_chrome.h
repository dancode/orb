#ifndef GUI_CHROME_H
#define GUI_CHROME_H
/*==============================================================================================

    runtime_service/gui/chrome/gui_chrome.h -- managed windowing (the chrome unit).

    The stock recipes: window records, keyboard nav, popups, docking -- policy over the
    mechanism units below.  Included by gui_internal.h after flow/gui_flow.h (the overlay
    seam saves a layout frame and the draw scope wholesale, so both types must exist).
    Implementation: the six folders under chrome/ (widgets, table, window, dock, popup,
    nav) since R9; unit root gui_chrome.c.

==============================================================================================*/

// clang-format off

#define GUI_DOCK_TABS_MAX           8       // windows co-docked (tabbed) in one leaf node
#define GUI_DOCK_NAME_CAP           28      // bytes of a tab's display name, copied at dock time

struct gui_dock_node_t;         // the dock tree node -- defined in full below

/*==============================================================================================
    Persisted window record (behavior in gui_window.c)

    One persisted window.  Geometry is owned here after the first appearance; the window pool that
    holds these lives in the bound context (gui_context_t).  Behavior is in gui_window.c.
==============================================================================================*/

typedef struct gui_window_t
{
    gui_id_t    id;             // id_hash(title); 0 = free slot
    f32         x, y;           // persisted top-left (updated by dragging)
    f32         w, h;           // persisted dimensions
    u32         z;              // paint order: higher = more recently raised = in front
    u32         viewport;       // target surface index (0 = main swapchain); set via window_set_next_viewport

    /* Popup / tooltip overlay: an anchored overlay on its surface, never the OS-window frame
       (window_is_native), never a nav or tab-drop target.  Stamped by the popup layer each begin,
       alongside a z in the reserved overlay band (see the z band map in core/gui_surface.c) --
       the flag carries the TYPE fact so z stays pure paint order. */
    bool       overlay;

    gui_scroll_link_t scroll;   // persisted scroll offset + last-measured content extent

    bool       collapsed;       // title-bar-only when set; toggled by the arrow
    bool       closed;          // CLOSEABLE: hidden by the X until re-opened

    /* Maximize / minimize for a regular (non-native, non-docked) floater -- state transitions in
       gui_window_free.c (window_maximize_set / window_minimize_set), chrome in gui_window_end.c.
       Maximized pins the window to its surface work area every frame; minimized parks it as a
       title-bar chip on a shelf along the surface's bottom edge.  norm is the saved normal rect
       both states restore to; shelf_slot orders the chips (taken at minimize time, compacted to
       a paint position each frame as neighbours restore).  These three fields are the POLICY --
       the rect tween between the states (and the norm save / restore), and the collapse height
       tween off `collapsed` above, are the feat kit's mechanisms (feat_pin / gui_feat_collapse,
       interact/gui_feature.c), keyed off the window id: their edge latches and from-values live
       in the keyed state pool, not on this record. */
    bool       maximized;
    bool       minimized;
    u32        shelf_slot;
    gui_rect_t norm;

    /* Re-open of a CLOSEABLE floater: closing it lets the abandoned-teardown free its OS window,
       reverting this record to viewport 0.  `floater` remembers it was one so the next begin
       re-spawns it.  The geometry is the floater's RESTORE (normal) state, sampled every frame it
       is not maximized -- so a floater closed while maximized re-opens maximized yet still
       restores to its previous normal size. */
    struct
    {
        bool floater;          // re-spawn as a floater on the next begin
        bool maximized;        // floater was maximized -- re-maximize after re-spawn
        i32  home_x, home_y;   // saved restore (normal) client-corner screen pos
        f32  w, h;             // saved restore (normal) size
    } reopen;

    gui_win_flags_t flags;              // behavior flags supplied to window_begin

    /* Next-window channel bookkeeping (see window_set_next_pos / _size).  last_frame drives the
       "appearing" test; the allow masks track which conditions a queued value may still fire. */

    u32        last_frame;              // frame index last begun; 0 = never begun

    u8         set_pos_allow;           // conds still permitted to set position (gui_cond_t bits)
    u8         set_size_allow;          // conds still permitted to set size (gui_cond_t bits)

} gui_window_t;

/*==============================================================================================
    Keyboard navigation state (driver in gui_nav.c)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather
    than the mouse -- plus the menu-bar state machine layered on top of it.  The instance is the
    bound context's nav member (g_ctx->nav), so each context keeps its own cursor.
==============================================================================================*/

/* One entry of the per-frame nav item list.  Every item of the nav window records itself here
   (emission order) as it passes through item_state; the resolvers in gui_nav.c consume the
   list at the NEXT nav_new_frame, so a move steps over last frame's items -- the same one-frame
   deferral hover_win uses.  region/line are the structural coordinate the layout engine stamped
   when it placed the item (cell_next_w), so moves are index math over real rows, not a
   spatial guess over rects; the rect remains only for the goal-column pick. */

typedef struct
{
    gui_id_t   id;       // widget id
    gui_rect_t rect;     // screen rect as emitted (goal-column pick + ring)
    u32        region;   // region sequence that placed it (a window body / child / strip region)
    u32        line;     // line sequence within the frame -- monotonic, so order == reading order
    bool       chrome;   // not layout-placed (title button, dock tab): the F6 chrome lane
    bool       drag_kind; // ITEM_DRAG (slider / drag box): eligible for row-solo auto-adjust

    /* Type-ahead label (gui_nav.c): lowercased, truncated, stamped by an opt-in widget right after
       it registers (gui_selectable) -- empty ("") means this item does not participate.  Kept on
       the nav item itself rather than a side table so type-ahead reuses the exact same one-frame-
       lagged list every other resolver (Up/Down, Tab, Home/End) already consumes. */
    char       label[ 16 ];

} gui_nav_item_t;

/* List capacity.  Every emitted item of the nav window registers -- including rows scrolled out
   of view (they still emit; only their draw is clipped) -- so this bounds the navigable item
   count of one window; overflow items simply drop off the keyboard map for that frame. */
#ifdef GUI_STRESS_TEST
#define GUI_NAV_ITEMS_MAX 4096   /* stress-bench build: 4x */
#else
#define GUI_NAV_ITEMS_MAX 1024
#endif

typedef struct
{
    gui_id_t    id;            // the highlighted item (keyboard cursor); persists across frames
    gui_id_t    win;           // window/popup nav is scoped to (the hover_win analogue)

    /* Keyboard-focused window (click / gui_window_set_nav / Ctrl+Tab / Alt-mnemonic), the SOLE
       authority for where the keyboard goes.  NONE means no window has focus -- a background/viewport
       click clears it -- and the keyboard then falls through to the app; there is no front-most
       fallback, so a defocused window does not silently keep the keyboard.  A tool that wants a window
       focused on first appearance sets it via gui_window_set_nav on its own GUI_COND_APPEARING edge. */
    gui_id_t    focused_win;

    /* Two visual states, the Dear ImGui NavDisableHighlight split.  active means a nav cursor
       position exists -> the outline ring is drawn at id (and follows clicks), persisting even in
       mouse mode so it keeps its location.  highlight means the keyboard is the *active* instrument
       right now -> the nav item also takes the fill (like a hovered button), mouse hover is
       suppressed (so the two never double-fill), and the keyboard is captured.  A nav key sets both;
       a mouse move or click drops highlight (back to ring-only), leaving active. */

    bool        active;        // a nav cursor exists -> draw the ring (cleared rarely)
    bool        highlight;     // keyboard is the active instrument -> fill + hover-suppress

    i32         move_dir;      // directional request this frame (gui_dir_t, or -1 for none)
    i32         tab;           // Tab linear move: +1 forward, -1 back, 0 none
    i32         page;          // PageUp/PageDown: -1 / +1 -- vertical hop by one view height
    i32         home;          // Home/End: -1 / +1 -- first / last item of the cursor's region
    bool        activate;      // Enter/Space -> fire id like a click this frame
    bool        lane;          // F6 -> hop between the body and the chrome strip this frame

    /* Keyboard value edit: activating a DRAG widget (slider, drag box) captures it -- the drag
       twin of focused_id text capture.  While captured, Left/Right step the value through
       gui_item_state_t.nav_adjust and Enter/Space/Esc (or a mouse press) release. */
    gui_id_t    edit_id;       // DRAG widget captured for keyboard value edit; 0 = none
    i32         edit_dir;      // value-edit arrow step this frame: -1 / +1 / 0

    /* Row-solo auto-adjust: a DRAG widget with no row neighbor on either side has nothing for
       Left/Right to navigate to, so those keys step its value directly instead of doing nothing --
       no Enter/Space capture required.  Up/Down are untouched (never fenced), unlike edit_id
       capture.  Resolved once per frame in nav_finish (last frame's list, before it resets) and
       consumed by nav_item_register during this frame's emission to mirror edit_id's captured
       presentation (st->focused, the COL_NAV_CAPTURE ring) and to gate st->nav_adjust. */
    gui_id_t    solo_drag_id;

    bool        id_seen;       // id was emitted in win this frame (else it went stale)
    gui_id_t    first_item;    // first layout-placed item this frame (first-focus / recovery)
    gui_id_t    body_id;       // body cursor to land back on when F6 leaves the chrome lane

    /* Goal column: the remembered x a run of Up/Down steers by, so vertical travel through rows
       of differing shapes does not drift sideways (the text-editor goal-column behavior).  Set
       lazily from the cursor item when a vertical run starts; any other adoption (horizontal
       move, Tab, click) clears it so the next run re-anchors. */
    f32         goal_x;
    bool        goal_set;

    /* Scroll-to-view request: set when a resolver adopts a new cursor item, consumed by
       nav_item_register when that item registers -- if its rect sits outside its region's view,
       the region (and its ancestors) scroll it into view (nav_scroll_chase, core/gui_item.c).
       One-shot per adoption so the chase never fights the wheel or a scrollbar drag. */
    bool        scroll_chase;

    /* Type-ahead query (gui_nav.c): A-Z / 0-9 keys accumulate here while nav owns the keyboard,
       timed out and restarted after GUI_TYPEAHEAD_TIMEOUT of no typing.  A repeated single key is
       the Explorer-style cycle case (buf stays length 1; the resolver scans past the current
       cursor instead of from the top) rather than a two-letter prefix.  type_dirty marks a fresh
       keystroke this frame so nav_finish resolves it at most once. */
    char        type_buf[ 16 ];
    u32         type_len;
    f64         type_last_t;
    bool        type_dirty;

    /* The nav item list -- built during emission, resolved at the next nav_new_frame. */
    gui_nav_item_t items[ GUI_NAV_ITEMS_MAX ];
    u32            item_count;

    /* Registration gate (decided per frame in nav_finish for the emission about to run).  While
       the keyboard is fully disengaged (no cursor, not active, no menu bar, no value edit) plain
       widgets skip the per-item list append -- pure bookkeeping waste in a mouse-only session.
       Type-ahead candidates (labeled selectables) still enter via nav_item_stamp_label, so a
       typed letter engages exactly as before; the first Tab/arrow engages and lands through the
       first-focus recovery one frame later.  list_full records whether the list the RESOLVERS
       see (last frame's) was built ungated -- a partial (labels-only) list must never be used
       for structural moves. */
    bool        reg_all;    // emission side: register every item this frame
    bool        list_full;  // resolve side: last frame's list was complete

    /* Menu-bar navigation -- a small state machine layered on the nav cursor + popup stack, entered
       by Alt (toggle) or an Alt+letter mnemonic.  While active, nav lives either on the bar entries
       (in_menus false: win is the bar window, a highlighted entry drops its menu) or inside the open
       menu popups (in_menus true: win is the top popup).  Down/Enter descend, Up at a top item and
       Left/Esc ascend -- always landing back on menu_owner so closing a menu returns to the bar
       entry that opened it (not the first entry).  See gui_nav.c + menu_begin. */

    gui_id_t    bar_win;       // menu-bar window nav is driving; 0 = not in menu-bar mode
    bool        in_menus;      // menu mode: false = on the bar entries, true = inside the popups
    gui_id_t    menu_owner;    // bar entry whose menu is open -- the ascend / close return target
    gui_id_t    prev_win;      // nav target to restore when Alt toggles out of the menu bar
    gui_id_t    prev_id;       // nav cursor to restore on Alt toggle-out (the last focus location)
    u8          mnemonic;      // pending Alt+letter mnemonic (uppercase ASCII); 0 = none

} gui_nav_state_t;

/*==============================================================================================
    Window context (the `win` member of s_build, core/gui_ctx.c; stamped in window/)

    The flat "window currently between window_begin / window_end" record -- everything begin
    stamps and end consumes.  One named struct so the overlay seam (gui_overlay_save_t below)
    saves and restores the whole window context as a single assignment: a field added here is
    carried across the popup seam automatically.  The frame-global scratch that must SURVIVE
    that seam (wheel_used, the nav dispensers, the combo channel) lives beside it in s_build,
    outside this record.
==============================================================================================*/

typedef struct
{
    gui_id_t            id;             // id of the window currently between begin/window_end
    const char*         title;          // title string, cached for window_end's deferred chrome
    bool                collapsed;      // current window is collapsed (title bar only this frame)
    bool                minimized;      // current window is a shelf chip (title bar only, chip chrome)
    bool                hidden;         // CLOSEABLE + closed: begin emitted nothing, end early-outs
    gui_win_flags_t     flags;          // behavior flags supplied to window_begin
    f32                 title_h;        // title bar height (0 if NOTITLEBAR)
    struct gui_window_t* rec;           // persisted window record; scroll write-back target

    /* Docking (gui_dock.c): the node hosting the current window, or NULL when it is
       free-floating.  When set, the window's geometry is owned by the node and its title bar is
       replaced by the node's tab strip; dock_active distinguishes the visible tab (draws a body)
       from a window docked behind another tab (window_begin returns false, draws nothing). */
    struct gui_dock_node_t*     dock_node;
    bool                        dock_active;

    f32         x, y;          // current window top-left (outer frame)
    f32         w, h;          // current window dimensions

    gui_rect_t  menubar_rect;  // reserved strip (WIN_MENUBAR); menu_bar_begin fills it

    u32         viewport;      // ambient viewport for new-window inheritance (stamped per window)

} gui_win_ctx_t;

/*==============================================================================================
    Popup stack (gui_ctx.c; driver in gui_popup.c)

    A popup is a top-level overlay begun while a parent window is still open but laid out,
    clipped, and painted independent of it.  gui_overlay_save_t is the parent context popup_end
    restores: whole-struct copies of the window context, the interaction scope, and the draw
    scope, plus the parent's top layout frame (its pen must survive the popup's region pop).
    Because the copies are wholesale, nothing here is maintained field-by-field -- a field added
    to any of those records rides the seam automatically.  The open set is a stack ordered
    parent -> child, held in gui_context_t.
==============================================================================================*/

typedef struct
{
    gui_win_ctx_t    win;      // s_build.win -- the flat window context
    gui_scope_t      scope;    // s_scope -- the interaction scope (behavior contract)
    gui_draw_scope_t draw;     // backend paint cursor + ambient glyph-clip window

    bool             had_parent;    // a layout region was open (parent frame valid)
    layout_frame_t   parent_frame;  // the parent's top frame, restored after the popup

} gui_overlay_save_t;

typedef struct
{
    gui_id_t            id;                 // popup window id (salted; matches s_build.win.id / hover_win)
    bool                modal;              // blocks input behind it + dims the background
    f32                 anchor_x;           // open point -- where a non-modal popup is placed
    f32                 anchor_y;           //
    u32                 open_frame;         // frame popup_open ran -- "appearing" detection
    u32                 begun_frame;        // last frame popup_begin ran -- drives stale-close
    gui_rect_t          rect;               // on-screen rect last frame -- drives click-outside
    gui_overlay_save_t  saved;              // parent context to restore at popup_end

} gui_popup_t;

/*==============================================================================================
    Dock node (behavior in gui_dock.c)

    One node of a viewport's dock tree.  A node is either a LEAF (split == GUI_DOCK_SPLIT_NONE),
    which tabs one or more windows into a single region, or an INTERNAL split (GUI_DOCK_SPLIT_X /
    _Y), which divides its rect between two children at `ratio` with a draggable splitter between
    them.  Nodes live in a fixed per-context pool (gui_context_t dock.pool) so child / parent pool
    indices (gui_dock_ref_t) stay valid across frames; a freed slot has id == 0.

    rect / content are resolved every frame by dock_node_layout from the viewport extent down: rect is
    the node's whole box, content is the leaf's body below its tab strip (where the active window draws).
==============================================================================================*/

/* Pool index of a dock node (into gui_context_t dock.pool), not to be confused with gui_dock_id_t
   (the stable id_hash-style handle exposed to callers).  The pool is fixed-size and never compacts,
   so an index stays valid across frames exactly like the pointer it replaces -- but at 2 bytes
   instead of 8.  dock_ref()/dock_at() (gui_dock_core.c) convert to/from a live pointer. */
typedef u16 gui_dock_ref_t;
#define GUI_DOCK_REF_NONE ( (gui_dock_ref_t)0xFFFFu )

typedef enum
{
    GUI_DOCK_SPLIT_NONE = 0,    /* leaf -- tabs windows; child[] unused                 */
    GUI_DOCK_SPLIT_X,           /* internal -- vertical split, children side by side    */
    GUI_DOCK_SPLIT_Y,           /* internal -- horizontal split, children top / bottom  */

} gui_dock_split_t;

typedef struct gui_dock_node_t
{
    gui_id_t    id;                          /* stable node handle; 0 = free pool slot            */
    u32         viewport;                    /* surface (viewport index) this tree belongs to     */
    u8          split;                       /* gui_dock_split_t: NONE = leaf, else internal     */
    f32         ratio;                       /* child[0]'s fraction of the split axis (0.5 default) */

    gui_dock_ref_t parent;       /* owning split, or GUI_DOCK_REF_NONE for the tree root  */
    gui_dock_ref_t child[ 2 ];   /* internal only (GUI_DOCK_REF_NONE on a leaf)           */

    /* Leaf payload: the windows tabbed into this node.  Names are copied at dock time so the tab
       bar is self-sufficient (no dependence on a window emitting this frame or its title lifetime). */

    gui_id_t    tabs [ GUI_DOCK_TABS_MAX ];
    char        names[ GUI_DOCK_TABS_MAX ][ GUI_DOCK_NAME_CAP ];
    u32         tab_count;
    u32         active_tab;                   /* index of the visible tab                          */

    gui_rect_t rect;                       /* whole node box, resolved this frame               */
    gui_rect_t content;                    /* leaf body below the tab strip (active window's rect) */

    /* Floating tab group (gui_dock_float.c): a leaf living OUTSIDE any viewport tree -- windows
       tabbed onto one shared free-floating frame, no split panes.  Not reachable from dock_root,
       so dock_node_layout never touches it: `rect` doubles as its PERSISTED geometry (moved by the
       strip drag, sized by the edge resize), and `z` stacks it among the free windows (a tree node
       draws at z 0 behind them).  Always GUI_DOCK_SPLIT_NONE with parent GUI_DOCK_REF_NONE. */
    bool floating;
    u32  z;

    /* Hidden pane: every window tabbed into this leaf stopped emitting (menu-hidden, X-closed);
       on a split, both children are hidden.  Refreshed at ctx_end (dock_hidden_refresh,
       gui_dock_core.c) from the windows' last_frame stamps; dock_node_layout collapses a hidden
       child to a zero-extent slice so the visible sibling absorbs its space -- tree structure and
       ratio stay untouched, so a window that re-emits gets its exact pane back.  Derived state:
       recomputed every build, never serialized.  Always false on a floating group (not reachable
       from any dock_root; a group tears down through the float paths instead). */
    bool hidden;

} gui_dock_node_t;

/*==============================================================================================
    Window record door + the next-window channel (core/gui_surface.c -- the interact server owns
    the pool; chrome is the policy that fills it)
==============================================================================================*/

gui_window_t* window_get ( gui_id_t id, f32 x, f32 y, f32 w, f32 h );
gui_window_t* window_find( gui_id_t id );       /* record by id / NULL */
void          window_apply_next( gui_window_t* win, bool appearing );

typedef struct
{
    bool        has_pos, has_size;     /* a value is queued on this axis */
    gui_cond_t  pos_cond, size_cond;   /* when to apply it               */
    f32         pos_x, pos_y;
    f32         size_w, size_h;

    bool        has_viewport;          /* a viewport reassignment is queued for the next window */
    u32         viewport;              /* its target surface index                              */

} gui_next_win_t;

typedef struct
{
    bool        active;     /* a request is queued this frame                                  */
    bool        by_drag;    /* true = seamless title-bar drag; false = detach-button click     */
    gui_id_t    win_id;     /* the dragged window record                                       */
    u32         from_vp;    /* surface it was on (0 = main -> tear off; else floater -> merge) */
    const char* title;      /* window title, to label the spawned floater's OS window          */
    bool        has_home;   /* re-open of a closed floater: spawn reads the record's restore   */

} gui_vp_request_t;

extern gui_next_win_t   s_next_win;    /* core/gui_surface.c */
extern gui_vp_request_t s_vp_request;  /* core/gui_surface.c */

/*==============================================================================================
    Frame steps + upward seams -- the few chrome definitions the core/frame unit calls UP into
==============================================================================================*/

void window_raise_on_press( void );   /* chrome/window/gui_window.c: press-to-front (ctx_begin)      */
void window_modal_apply   ( void );   /* chrome/popup/gui_popup.c: modal fence (ctx_begin)           */
void popup_apply_modal    ( void );   /* chrome/popup/gui_popup.c: per-frame modal inertness         */
void popup_close_check    ( void );   /* chrome/popup/gui_popup.c: click-outside close (frame)       */
void nav_new_frame        ( void );   /* chrome/nav/gui_nav.c: per-frame nav turnover                */
void dock_hidden_refresh  ( void );   /* chrome/dock/gui_dock_core.c: hidden-node upkeep (frame)     */
u32  gui_chrome_unit_mem_bytes( void );

/* gui_popup.c is included after the widgets/ files; selectable calls this to auto-close the
   enclosing popup on click (the Dear ImGui CloseCurrentPopup default behavior). */
void gui_popup_close_current( void );

/* Window text selection (chrome/window/gui_select.c, chrome since R6 -- it reads the render capture
   and font metrics, so it is policy astride both servers, not an interact mechanism).  The
   bands paint under the body at the window begins; the protocol + overlay resolve at end. */
void select_paint_under( void );
void select_window_end( void );

/* The region engine (flow/gui_scroll.c) emits the scrollbar widget into the gutter
   it reserved at layout_pop_region -- but the widget lives above it (chrome/widgets/gui_scrollbar.c):
   flow's one upward call beside the anim ease.  Compose hands the track rect + scroll slot; the
   widget owns the feel and the look. */
void scrollbar_widget( gui_id_t region_id, gui_rect_t track, bool vertical,
                       f32 content, f32 view, f32* scroll );

/* The window <-> dock route seam (implemented in chrome/dock/gui_dock_route.c).

   window/ is included BEFORE dock/, yet a window must ask the dock whether it owns placement.
   This seam is the ONLY door between them: window/ calls these five verbs and nothing else in
   dock/; a build without docking is a stub implementation of this block.  One verb per protocol
   point in the window lifecycle:

     resolve  (window_begin)  -- service any pending tab-group request, then answer "who places
                                 this window?".  route.node == NULL is the free-float path; a
                                 floating group's active tab also gets its frame resolved (strip
                                 drag / edge resize applied) and the hot edge mask back.
     drag     (free path,     -- preview a drop target (per-node 5-way overlay) while this free
               during drag)      window is title-dragged over a dockspace.
     commit   (window_end,    -- execute the drop computed by drag on the release edge; no-ops
               free path)        unless this is the dragged window releasing.
     chrome   (window_end,    -- tab strip + tabs + node border, drawn in place of a title bar;
               docked path)      reads the current window rect from s_build.
     raise    (press-to-front)-- the dock exception to raise-on-press: true when the window is
                                 dock-managed (a tile never reorders; a floating group raises
                                 its node's z as a whole -- done inside). */

typedef struct
{
    gui_dock_node_t* node;         // NULL = free-float; else the dock leaf that places this window
    bool             active;       // this window is the node's active tab (its body opens)
    u8               resize_hot;   // floating-group hot edge mask for s_scope; 0 otherwise

} gui_win_route_t;

static gui_win_route_t window_route_resolve( gui_id_t id, const char* title, gui_window_t* win );
static void            window_route_drag   ( gui_id_t id, gui_window_t* win );
static void            window_route_commit ( gui_id_t id, const char* title );
static void            window_route_chrome ( gui_dock_node_t* node );
static bool            window_route_raise  ( gui_id_t id );

// clang-format on
/*============================================================================================*/
#endif    // GUI_CHROME_H
