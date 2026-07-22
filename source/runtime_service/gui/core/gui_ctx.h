#ifndef GUI_CTX_H
#define GUI_CTX_H
/*==============================================================================================

    runtime_service/gui/core/gui_ctx.h -- the interact server's retained-mode storage.

    THE RECORDS: the model says the interact server owns "dedicated
    retained-mode storage -- the id namespace, the keyed state pool, retained rect records,
    the hover/z contest".  This header is that storage: every retained record the server
    dereferences (window, nav cursor, surface, scroll link, the build scratch) is DEFINED
    here, and the context aggregate that pools them closes the file.  The POLICY over each
    record stays where it always was -- window/nav/popup/dock behavior in chrome, surface
    orchestration in frame.  Records whose shape the server never reads (the popup stack,
    the dock-tree node) stay in chrome/gui_chrome.h behind the forward declarations below:
    the server allocates nothing (allocation is frame's) and treats them as opaque.

    Include order: after core/gui_core.h (the aggregate embeds gui_retained_t).

==============================================================================================*/

// clang-format off

/* Chrome-owned retained records the server stores but never reads: pooled behind pointers in
   gui_context_t, shaped in chrome/gui_chrome.h, sized by the allocator in frame/gui_context.c. */
typedef struct gui_popup_t     gui_popup_t;
typedef struct gui_dock_node_t gui_dock_node_t;

/*==============================================================================================
    Scroll link (machinery in flow/gui_scroll.c)

    Persistent scroll offset + last-measured content extent for one scrollable region.
    layout_push_region biases the pen by -scroll and writes content_w/content_h back at pop; the
    owner (gui_window_t, gui_region_t, gui_table_persist_t) holds one by value so layout_frame_t
    can reach it through a single pointer instead of four.
==============================================================================================*/

typedef struct
{
    f32 scroll_x, scroll_y;    // persisted scroll offset; 0 = top-left
    f32 content_w, content_h;  // content extent measured last frame

    /* Bottom-anchor tail-follow (GUI_WIN_ANCHOR_BOTTOM only): pinned_y is the scroll_y layout_push_region
       last left the region at, so a later external move (wheel / bar / scroll_by) is detectable; unstick
       latches once the user scrolls off the bottom and clears when they return to it.  Zero on both = the
       default "follow the tail" state a fresh region opens in. */
    f32  pinned_y;
    bool unstick;

} gui_scroll_link_t;

/*==============================================================================================
    Persisted window record (policy in chrome/window/)

    One persisted window.  Geometry is owned here after the first appearance; the window pool that
    holds these lives in the bound context (gui_context_t), and the record door below is the
    surface service's (core/gui_surface.c).
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

/* The window record door (core/gui_surface.c -- the server owns the pool; chrome is the policy
   that fills it) + the next-window channel records its verbs queue into. */
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
    Keyboard navigation state (driver in chrome/nav/gui_nav.c)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather
    than the mouse -- plus the menu-bar state machine layered on top of it.  The instance is the
    bound context's nav member (g_ctx->nav), so each context keeps its own cursor.  The record
    is server storage (the item protocol registers every emitted item into it); the resolvers
    that MOVE the cursor are chrome policy.
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
    Window context (the `win` member of s_build below; stamped in chrome/window/)

    The flat "window currently between window_begin / window_end" record -- everything begin
    stamps and end consumes.  One named struct so the overlay seam (gui_overlay_save_t,
    chrome/gui_chrome.h) saves and restores the whole window context as a single assignment: a
    field added here is carried across the popup seam automatically.  The frame-global scratch
    that must SURVIVE that seam (wheel_used, the nav dispensers, the combo channel) lives beside
    it in s_build, outside this record.
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
    Render viewport (orchestration in frame/gui_viewport.c; the GPU flush takes its buffer
    fields as parameters -- the render server never sees this record)

    One render surface a context drives: GPU buffers + a color target, the OS window hosting it, and
    the routing/ownership bookkeeping for host-provided vs gui-owned (torn-off floater) surfaces.
    [0] is the main swapchain; the rest are floaters.  Held by value in gui_context_t vp.pool.
==============================================================================================*/

/* Pool index of a dock node (into gui_context_t dock.pool), not to be confused with gui_dock_id_t
   (the stable id_hash-style handle exposed to callers).  The pool is fixed-size and never compacts,
   so an index stays valid across frames exactly like the pointer it replaces -- but at 2 bytes
   instead of 8.  dock_ref()/dock_at() (gui_dock_core.c) convert to/from a live pointer. */
typedef u16 gui_dock_ref_t;
#define GUI_DOCK_REF_NONE ( (gui_dock_ref_t)0xFFFFu )

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
       gui-OWNED surface (a torn-off floater gui spawned): flush of a host-provided surface
       resolves RHI_SWAPCHAIN_COLOR from the host's cmd, so the host viewport leaves this invalid.
       An owned surface has no host driving it -- gui runs frame_begin/end on this ctx itself. */
    i32 rhi_ctx;

    /* true when gui created this surface's OS window + rhi context (tear-off floater) and must
       therefore destroy them.  false for the host-provided main surface (index 0) and any surface
       the host opened via viewport_open -- gui frees only the GPU buffers for those, never the
       window/context it does not own. */
    bool owned;

    /* Set when the user closes an owned floater's OS window (APP_EV_WIN_CLOSE): the surface is torn
       down at the next viewport_update, a safe point between the build and the present, so
       no in-flight draw list references a surface being freed.  Ignored for non-owned surfaces. */
    bool pending_close;

    /* Drawable size of this surface in pixels.  Set by the host (viewport 0 from frame_begin, floaters
       via viewport_resize) BEFORE the build so window_begin clips its windows against THIS surface's
       extent, not the main window's.  0 = unset -> window_begin falls back to the main display size
       (single-window behavior).  Distinct from the win_w/win_h passed to flush, which only sets the
       GPU viewport/scissor clamp at submit time; the clip baked into each draw command is built here. */
    i32 disp_w, disp_h;

    /* Top band (pixels) drawn by this surface's native host caption (the GUI_WIN_NATIVE shell
       window's title bar height), published each frame by that shell.  window_clamp keeps non-native
       windows' top edge at or below this inset so their title bars stay grabbable above the drawn
       chrome band.  0 until first published (no native shell or default OS-chrome main window).
       Sticky: NOT cleared each frame -- persists from the last frame the native shell was active so
       viewport_update always has a valid top bound regardless of build ordering. */
    f32 caption_inset;

    /* Main-menu-bar band on this surface: its height plus the frame it last emitted.  Emit-gated
       like a dockspace (bar_seen_frame against the current frame, one-frame tolerance) so a host
       code path that stops emitting the bar releases the band; window_work_top
       (gui_window_free.c) adds it to caption_inset to bound a maximized window's work area. */
    f32 bar_inset;
    u32 bar_seen_frame;

    /* Additional top band (pixels) the host reserves above the dock area -- a main menu bar, a
       toolbar strip -- published via gui()->dockspace_inset() before dockspace_over_viewport.
       Adds to caption_inset when the dock tree lays out.  Sticky like caption_inset: persists
       until the host publishes a new value (0 to reclaim).  Free-floating windows are
       unaffected -- only the dock tree's layout area shrinks. */
    f32 dock_inset;

    /* Per-surface dock tree root.  GUI_DOCK_REF_NONE = free-float placement (overlapping windows,
       including the main viewport); otherwise a ref into the context dock-node pool that tiles/tabs
       the windows on this surface.  Driven by dock/ (dock.c builds it, dock_drag.c re-tiles on drag,
       dock_serialize.c saves/loads it). */
    gui_dock_ref_t dock_root;

    /* Dockspace policy bits (gui_dockspace_flags_t), re-published by every dockspace_over_viewport
       call.  NO_SPLIT restricts the tree to tab docking: no split drop chips, split verbs refuse. */
    gui_dockspace_flags_t dock_flags;

    /* Frame stamp (g_ctx->retained.frame) of this viewport's last dockspace_over_viewport call.
       A dockspace is emit-gated like every immediate-mode element: the tree is ACTIVE only on
       frames the host emits it; on other frames it is DORMANT -- retained but inert.  Windows
       tabbed in a dormant tree suppress (inactive-tab semantics) instead of rendering into rects
       that no longer lay out, and drag-to-dock offers no chips (dock_vp_emitted, gui_dock_core.c).
       A host code path that stops running its dockspace thus parks the layout instead of
       corrupting it; only dock_clear destroys it. */
    u32 dock_seen_frame;

    /* Dockspace maximize: one LEAF pinned over the whole dock area (dock_max_id; 0 = none), fully
       obscuring the other tree nodes.  dock_max_on is the logical state -- false while the restore
       tween eases the node back to its tree rect, after which the id clears.  dock_max_settled is
       stamped by dockspace_over_viewport's tween step each emitted frame: only once the cover has
       SETTLED do the obscured nodes' windows suppress (inactive-tab semantics via the route seam)
       and the splitter / placeholder chrome stop emitting -- during the tween the siblings are
       still partially visible and keep drawing.  dock_max_from is the rect tween's FROM (captured
       at toggle); the target re-aims every frame, so a live surface resize is tracked mid-flight.
       Driven by dock_max_set / dock_max_node (gui_dock_core.c). */
    gui_dock_id_t dock_max_id;
    bool          dock_max_on;
    bool          dock_max_settled;
    gui_rect_t    dock_max_from;

} gui_viewport_t;

/* viewport drawable size with the s_io fallback (core/gui_ctx.c). */
f32 vp_w( const gui_viewport_t* vp );
f32 vp_h( const gui_viewport_t* vp );

/* The mouse-input path (core/gui_io.c) resolves an event's app win_id to the viewport hosting it,
   but the viewport pool lives on g_ctx (core/gui_ctx.c), a later constituent.  Static: both ends
   live in this unit. */
static u32 viewport_index_for_window( i32 win_id );

/*==============================================================================================
    gui_context_t -- the bound per-context retained state ("bind and use").

    A context is the emission session the code binds once and emits ALL its windows into; it owns
    the state that must persist between frames for that UI.  Every retained access resolves through
    g_ctx via the aliases in gui_ctx.c -- g_ctx->retained, g_ctx->nav, the popup open-set -- so switching
    contexts is a single pointer assignment (ctx_bind): no copy, no backup/restore.

    Ambient state (s_interaction) and frame scratch (s_build, the stacks, s_draw) stay global by design
    -- tier 1 (one physical user) and tier 3 (scratch reused across contexts each frame); see
    ARCHITECTURE.md.  s_io (hardware input snapshot) is always shared.  The `listening` flag gates whether a bound context
    receives hover / click / nav updates -- a deaf context renders but returns inert widget state.
==============================================================================================*/

typedef struct gui_context_t
{
    gui_retained_t   retained;      // id salt, frame clock, keyed state pool (ptr into alloc)
    gui_nav_state_t  nav;           // nav cursor location + menu-bar mode

    struct                          /* popup/ -- the open-popup stack */
    {
        gui_popup_t* open;          // open popup set, ordered parent -> child; ptr into alloc
        u32          open_count;    // live open count
        u32          depth;         // capacity (max nesting depth)
    } popup;

    struct                          /* window/ -- GUI_WIN_MODAL fence (window_modal_apply) */
    {
        gui_id_t     win_id;        // id of the modal overlay window; 0 = none
        u32          seen_frame;    // frame it last emitted -- fence lapses when it stops
    } modal;

    struct                          /* surface/ + window/ -- the persisted window records */
    {
        gui_window_t* pool;         // persisted window records; ptr into alloc
        u32           count;        // live records in the pool
        u32           max;          // capacity
        gui_window_t  scratch;      // transient fallback when the pool is full; stays embedded
        u32           z_counter;    // monotonic paint-order dispenser
        u32           cascade;      // default-spawn cascade slot (window_default_spawn)
    } win;

    struct                          /* frame/ -- render surfaces */
    {
        gui_viewport_t* pool;       // render surfaces: [0]=main swapchain; ptr into alloc
        u32             count;      // high-water slot count (compacted on close; iterate [0, count))
        u32             max;        // capacity
    } vp;

    struct                          /* dock/ -- the dock-tree node pool */
    {
        gui_dock_node_t* pool;      // dock-tree node pool; NULL when max == 0
        u32              count;     // high-water slot count in the pool
        u32              id_seq;    // monotonic node-id dispenser (0 = none)
        u32              max;       // capacity; 0 = docking disabled
    } dock;

    bool  listening;    // true: context receives hover/click/nav input this frame
    void* _alloc;       // the single ctx_alloc_slot malloc backing this header + all
                        // pool arrays; freed at teardown. Non-NULL for every context,
                        // slot 0 included (freed at shutdown; secondaries at ctx_destroy)
    u32   _alloc_size;  // byte size of the _alloc block; reported by gui_mem_stats()

} gui_context_t;

/* Frame-build scratch -- the "where am I emitting right now" context, rebuilt every frame as
   the widget tree is walked.  Nothing survives begin_frame; contexts build sequentially on one
   thread, so it stays a single global builder.  Field story at the definition (core/gui_ctx.c). */
typedef struct
{
    gui_win_ctx_t win;                  // the window currently between window_begin / window_end

    bool          wheel_used;           // a region consumed the wheel this frame (innermost wins)

    u32           nav_region_seq;       // per-frame region dispenser (layout_seed_content)
    u32           nav_line_seq;         // per-frame line dispenser (a line-open takes the next)

    gui_item_flags_t item_flags;        // merged top-of-stack item flags
    gui_item_flags_t next_set;          // bits the next-item override controls
    gui_item_flags_t next_val;          // their values

    bool          combo_open;           // a combo dropdown body is currently being emitted
    bool          combo_item_clicked;   // a selectable in that body was clicked this frame

} gui_build_t;

/*============================================================================================*/
/* exported context data */

extern gui_context_t* g_ctx;            // core/gui_ctx.c -- the bound context
extern gui_build_t    s_build;          // core/gui_ctx.c -- frame-build scratch

/*==============================================================================================
    Context pool + lifecycle seams (core/gui_ctx.c) -- the storage stays with the interact
    server; the PUBLIC lifecycle over it AND the block allocation (which sizes chrome's records,
    so it needs whole-stack type visibility) live in frame/gui_context.c.
==============================================================================================*/

#define GUI_CTX_POOL_MAX  8             /* slot 0 = default + up to 7 secondary contexts */

extern gui_context_t* s_ctx_pool[ GUI_CTX_POOL_MAX ];   /* allocated context data */
extern u32            s_ctx_pool_count;                 /* live slot count; always >= 1 after init */

void           ctx_bind      ( gui_context_t* ctx );    /* NULL rebinds the default           */
void           ctx_new_frame ( void );                  /* per-context scratch reset          */

void           interaction_frame_reset( void );         /* once per APP frame (frame_begin)   */
void           cursor_flush  ( void );                  /* push last frame's cursor to the OS */

// clang-format on
/*============================================================================================*/
#endif    // GUI_CTX_H
