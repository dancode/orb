#ifndef GUI_CTX_H
#define GUI_CTX_H
/*==============================================================================================

    runtime_service/gui/core/gui_ctx.h -- retained-mode storage for the interact server.

    Defines every record the interact server keeps between frames: windows, the nav cursor,
    viewports, scroll offsets, popups, dock nodes, and the gui_context_t that groups them. The
    behavior that reads and updates these records (window dragging, docking, popups) lives
    elsewhere in chrome/ and frame/ -- this header only defines the storage shape.

    Include order: after core/gui_core.h (gui_context_t embeds gui_retained_t).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Scroll link -- persisted scroll state for one scrollable region (flow/gui_scroll.c)

    Holds the scroll offset and the content size measured last frame. Owners (gui_window_t,
    gui_region_t, gui_table_persist_t) embed one by value.
==============================================================================================*/

typedef struct
{
    f32 scroll_x, scroll_y;    // current scroll offset; 0,0 = top-left
    f32 content_w, content_h;  // content size measured last frame

    /* Used only when GUI_WIN_ANCHOR_BOTTOM is set (e.g. a log window that should auto-scroll as
       new content arrives). pinned_y is the scroll position layout last set; if scroll_y differs,
       the user scrolled it manually. unstick is true once they scroll away from the bottom, and
       clears once they scroll back down. */
    f32  pinned_y;
    bool unstick;

} gui_scroll_link_t;

/*==============================================================================================
    Persisted window record -- one window's saved state (behavior in chrome/window/)

    Position, size, and other state that must survive across frames for one window. The pool of
    these records lives in gui_context_t; the accessors below are in core/gui_surface.c.
==============================================================================================*/

typedef struct gui_window_t
{
    gui_id_t    id;              // id_hash(title); 0 = free slot
    f32         x, y;            // persisted top-left (updated by dragging)
    f32         w, h;            // persisted dimensions
    u32         z;               // paint order: higher = more recently raised = in front
    i32         viewport;        // target surface (0 = main swapchain); set via window_set_next_viewport

    u8         set_pos_allow;    // conds still permitted to set position (gui_cond_t bits)
    u8         set_size_allow;   // conds still permitted to set size (gui_cond_t bits)

    /* Packed into one byte so win->bits = 0 clears every flag at once. The union lets call
       sites keep using win->overlay, win->maximized, etc. directly. */
    union
    {
        struct
        {
            u8 overlay   : 1;  // popup/tooltip overlay, not a real OS window or nav target
            u8 collapsed : 1;  // title-bar only; toggled by the collapse arrow
            u8 closed    : 1;  // CLOSEABLE window hidden by its close button
            u8 maximized : 1;  // filling its surface's work area (non-native, non-docked only)
            u8 minimized : 1;  // reduced to a title-bar chip on the bottom shelf
        };
        u8 bits;
    };

    u32                 last_frame;     // frame index last begun; 0 = never begun
    gui_win_flags_t     flags;          // behavior flags supplied to window_begin

    u8                  shelf_slot;     // minimized: the chip's order along the surface's bottom edge (0 = leftmost)

    gui_rect_t          norm;           // maximized / minimized: the saved normal rect to restore to
    gui_scroll_link_t   scroll;         // persisted scroll offset + last-measured content extent

    /* Native windows only. When a floater window is closed, its OS window is destroyed and this
       record falls back to viewport 0. These fields remember enough to re-spawn it as a floater
       the next time it is opened. */
    struct
    {
        bool floater;          // was a floater; re-spawn as one on next open
        bool maximized;        // was maximized; re-maximize after re-spawn
        i32  home_x, home_y;   // restore (non-maximized) position to spawn at
        f32  w, h;             // restore (non-maximized) size to spawn at
    } reopen;

} gui_window_t;

/* Accessors for the window pool (core/gui_surface.c). */

gui_window_t* window_get ( gui_id_t id, f32 x, f32 y, f32 w, f32 h );     // find or create
gui_window_t* window_find( gui_id_t id );                                // find, or NULL
void          window_apply_next( gui_window_t* win, bool appearing );
i32           window_spawn_viewport( void );                             // viewport a NEW record inherits

/* Position/size/viewport queued by gui_set_next_window_* for the next window_begin call. */
typedef struct
{
    bool        has_pos, has_size;     // true if a value is queued for that axis
    gui_cond_t  pos_cond, size_cond;   // condition under which to apply it
    f32         pos_x, pos_y;
    f32         size_w, size_h;

    bool        has_viewport;          // true if a viewport reassignment is queued
    i32         viewport;              // target surface

} gui_next_win_t;

/* A queued request to move a window to a different viewport -- either tearing a docked window
   off into its own floating OS window, or merging a floater back in. Resolved once per frame by
   gui_viewport_update. */

typedef struct
{
    bool        active;     // a request is queued this frame
    bool        by_drag;    // true = dragged off the title bar; false = detach button
    gui_id_t    win_id;     // the window being moved
    u32         from_vp;    // its current surface (0 = main -> tear off; else -> merge back)
    const char* title;      // window title, for the new floater's OS window
    bool        has_home;   // re-opening a closed floater: use its saved restore rect

} gui_vp_request_t;

const gui_next_win_t* gui_next_win_peek( void );    // core/gui_surface.c -- read-only queue peek
extern gui_vp_request_t s_vp_request;               // core/gui_surface.c

/*==============================================================================================
    Keyboard navigation state (behavior in chrome/nav/gui_nav.c)

    Tracks which widget the keyboard cursor is on -- the keyboard equivalent of mouse hover,
    moved by arrow keys and Tab instead of the mouse -- plus the menu-bar state machine built on
    top of it. Lives in gui_context_t (g_ctx->nav).
==============================================================================================*/

/* One navigable widget, recorded in emission order as it is drawn each frame. Arrow-key and Tab
   moves are resolved against LAST frame's list (one frame of lag, like mouse hover), stepping
   through region/line -- the row/column position layout assigned it -- rather than guessing from
   screen coordinates. The rect is only used to pick a starting column when a vertical move
   begins. */

typedef struct
{
    gui_id_t   id;        // widget id
    gui_rect_t rect;      // screen rect when emitted (used for the goal-column pick)
    u32        region;    // the region (window body / child / strip) that placed it
    u32        line;      // row index within the frame; increases in reading order
    bool       chrome;    // title bar button, dock tab, etc: reachable only via F6, not Tab
    bool       drag_kind; // a slider/drag box: Left/Right can adjust it directly if it has no row neighbor

    // lowercased type-ahead label (gui_selectable); "" means this item is skipped by type-ahead
    char       label[ 16 ];

} gui_nav_item_t;

/* Max navigable items per window per frame (includes rows scrolled out of view -- they still
   register, only their draw is clipped). Items past this limit are simply not keyboard-reachable
   that frame. */
#define GUI_NAV_ITEMS_MAX 1024

typedef struct
{
    gui_id_t    id;            // widget the keyboard cursor is on; persists across frames
    gui_id_t    win;           // window/popup the cursor is scoped to (like hover_win, for keyboard)

    /* Window that currently has keyboard focus, set by a click, gui_window_set_nav, Ctrl+Tab, or
       an Alt mnemonic. 0 means no window is focused, and keyboard input falls through to the app
       -- there is no "front-most window" fallback. A window that wants focus on first appearance
       calls gui_window_set_nav on its GUI_COND_APPEARING edge. */
    gui_id_t    focused_win;

    /* active: a keyboard cursor position exists, so its outline ring is drawn (this persists
       even while using the mouse, so the cursor keeps its place).
       highlight: the keyboard is what the user is actively using right now, so the cursor item
       also gets the hover fill and mouse hover is suppressed. A nav key sets both; moving or
       clicking the mouse clears highlight but leaves active. */
    bool        active;
    bool        highlight;

    i32         move_dir;      // directional request this frame (gui_dir_t, or -1 for none)
    i32         tab;           // Tab linear move: +1 forward, -1 back, 0 none
    i32         page;          // PageUp/PageDown: -1 / +1 -- vertical hop by one view height
    i32         home;          // Home/End: -1 / +1 -- first / last item of the cursor's region
    bool        activate;      // Enter/Space -> fire id like a click this frame
    bool        lane;          // F6 -> hop between the body and the chrome strip this frame

    /* Activating a drag widget (slider, drag box) with Enter/Space captures the keyboard for it
       -- Left/Right then step its value, and Enter/Space/Esc or a mouse click release it. */
    gui_id_t    edit_id;       // drag widget currently captured; 0 = none
    i32         edit_dir;      // value-edit step this frame: -1 / +1 / 0

    /* A drag widget with no left/right neighbor to move to instead has Left/Right adjust its
       value directly, without needing Enter/Space to capture it first. */
    gui_id_t    solo_drag_id;

    bool        id_seen;       // id was emitted in win this frame (else it went stale)
    gui_id_t    first_item;    // first layout-placed item this frame (first-focus / recovery)
    gui_id_t    first_chrome;  // first chrome item this frame -- recovery fallback for a window
                                // with no placed items at all (a minimized shelf chip)
    gui_id_t    body_id;       // body cursor to land back on when F6 leaves the chrome lane

    /* Remembered x position for a run of Up/Down moves, so moving through rows of different
       widths doesn't drift sideways (like a text editor's column memory). Set on the first
       vertical move of a run; any other kind of move clears it. */
    f32         goal_x;
    bool        goal_set;

    /* Set when the cursor moves to a new item; scrolls that item into view if it is outside its
       region (nav_scroll_chase, flow/gui_scroll.c). Cleared after one use. */
    bool        scroll_chase;

    /* Type-ahead: typing A-Z/0-9 jumps the cursor to a matching item, like Windows Explorer.
       Keys accumulate here and reset after GUI_TYPEAHEAD_TIMEOUT of no typing. Repeating the
       same single key cycles through matches instead of typing a two-letter prefix. */
    char        type_buf[ 16 ];
    u32         type_len;
    f64         type_last_t;
    bool        type_dirty;    // a key was typed this frame, not yet resolved

    /* The nav item list -- built during emission, resolved at the next nav_new_frame. */
    gui_nav_item_t items[ GUI_NAV_ITEMS_MAX ];
    u32            item_count;

    /* While the keyboard is idle (mouse-only session), most widgets skip registering into the
       item list to avoid the bookkeeping cost -- only type-ahead candidates still register.
       reg_all turns full registration on for this frame's emission. list_full records whether
       LAST frame's list was built with reg_all on; a partial list must never be used for a
       structural move like Tab or an arrow key. */
    bool        reg_all;    // register every item this frame, not just type-ahead ones
    bool        list_full;  // true if last frame's item list was built with reg_all on

    /* Menu-bar navigation: Alt (or an Alt+letter mnemonic) hands the keyboard to the menu bar.
       While active, the cursor is either on a bar entry (in_menus false) or inside that entry's
       open menu (in_menus true). Down/Enter opens a menu; Up/Left/Esc closes back to menu_owner,
       the bar entry that opened it. See gui_nav.c and menu_begin. */

    gui_id_t    bar_win;       // menu bar window being navigated; 0 = not in menu-bar mode
    bool        in_menus;      // false = cursor on a bar entry, true = cursor inside its menu
    gui_id_t    menu_owner;    // bar entry whose menu is open; where Esc/close returns to
    gui_id_t    prev_win;      // nav window to restore when Alt exits menu-bar mode
    gui_id_t    prev_id;       // nav cursor to restore when Alt exits menu-bar mode
    u8          mnemonic;      // pending Alt+letter mnemonic (uppercase ASCII); 0 = none

} gui_nav_state_t;

/*==============================================================================================
    Window context -- state for the window currently open between window_begin/window_end
    (the `win` member of s_build below; filled in chrome/window/)

    Kept as one struct (rather than loose globals) so opening a popup on top of a window can
    save and restore the whole thing with a single assignment (gui_overlay_save_t,
    chrome/gui_chrome.h) -- any field added here is included automatically.
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

    /* Docking (gui_dock.c): the dock node hosting this window, or NULL if free-floating. A
       docked window's geometry and title bar are owned by the node's tab strip. dock_active is
       true for the visible tab; a window docked behind another tab has window_begin return
       false and draw nothing. */
    struct gui_dock_node_t*     dock_node;
    bool                        dock_active;

    f32         x, y;          // current window top-left (outer frame)
    f32         w, h;          // current window dimensions

    gui_rect_t  menubar_rect;  // reserved strip (WIN_MENUBAR); menu_bar_begin fills it

    i32         viewport;      // ambient viewport for new-window inheritance (stamped per window)

} gui_win_ctx_t;

/*==============================================================================================
    Popup stack entry (gui_context_t popup.open; driver in chrome/popup/gui_popup.c)

    A popup is a top-level overlay begun while a parent window is still open but laid out,
    clipped, and painted independent of it.  The open set is a stack ordered parent -> child.
    The parent state a popup_end restores (window context, scopes, layout frame) is chrome's
    own record, kept in a parallel array indexed by the same depth (gui_popup.c).
==============================================================================================*/

typedef struct gui_popup_t
{
    gui_id_t    id;             // popup window id; matches s_build.win.id / hover_win
    bool        modal;          // blocks input behind it + dims the background
    f32         anchor_x;       // open point -- where a non-modal popup is placed
    f32         anchor_y;       //
    u32         open_frame;     // frame popup_open ran -- "appearing" detection
    u32         begun_frame;    // last frame popup_begin ran -- drives stale-close
    gui_rect_t  rect;           // on-screen rect last frame -- drives click-outside

} gui_popup_t;

/*==============================================================================================
    Dock node (behavior in chrome/dock/)

    One node of a viewport's dock tree.  A node is either a LEAF (split == GUI_DOCK_SPLIT_NONE),
    which tabs one or more windows into a single region, or an INTERNAL split (GUI_DOCK_SPLIT_X /
    _Y), which divides its rect between two children at `ratio` with a draggable splitter between
    them.  Nodes live in a fixed pool (gui_context_t dock.pool) so child / parent pool indices
    (gui_dock_ref_t) stay valid across frames; a freed slot has id == 0.

    rect / content are resolved every frame by dock_node_layout from the viewport extent down:
    rect is the node's whole box, content is the leaf's body below its tab strip (where the
    active window draws).
==============================================================================================*/

/* Index into gui_context_t's dock-node pool (not the same as gui_dock_id_t, the stable handle
   callers use). The pool never compacts, so an index stays valid across frames just like a
   pointer would, but takes 2 bytes instead of 8. dock_ref()/dock_at() (gui_dock_core.c) convert
   between this and a live pointer. */

typedef u16 gui_dock_ref_t;
#define GUI_DOCK_REF_NONE ( (gui_dock_ref_t)0xFFFFu )

#define GUI_DOCK_TABS_MAX           8       // windows co-docked (tabbed) in one leaf node
#define GUI_DOCK_NAME_CAP           28      // bytes of a tab's display name, copied at dock time

typedef enum
{
    GUI_DOCK_SPLIT_NONE = 0,    /* leaf -- tabs windows; child[] unused                 */
    GUI_DOCK_SPLIT_X,           /* internal -- vertical split, children side by side    */
    GUI_DOCK_SPLIT_Y,           /* internal -- horizontal split, children top / bottom  */

} gui_dock_split_t;

typedef struct gui_dock_node_t     /* tagged: gui_win_ctx_t above forward-references it */
{
    gui_id_t    id;                          /* stable node handle; 0 = free pool slot            */
    i32         viewport;                    /* surface this tree belongs to                      */
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
       recomputed every build, never serialized.  A floating group is refreshed too (visited
       directly in the pool, since no dock_root reaches it) -- chiefly for the active-tab handoff;
       its hidden flag has no layout consumer. */
    bool hidden;

} gui_dock_node_t;

/*==============================================================================================
    Viewport -- The gui render surface (managed in frame/gui_viewport.c)

    Controls the gui specific state over a render context draw area.

    The viewport has GPU render buffers, a color targets, and the OS window hosting it, 
    its drawable size and reserved chrome bands, and its dock tree. etc.
    
    Slot [0] is the main swapchain; the rest are floating windows torn off from it.
    Viewports live in their own global table (s_vp_pool), not inside gui_context_t.
==============================================================================================*/

typedef struct
{
    bool live;              // slot holds a live surface (viewport_create .. viewport_destroy)

    /* Color target that flush draws into -- main swapchain or a floater's swapchain image
       -- mostly RHI_SWAPCHAIN_COLOR sentinel (whatever swapchain image is current)
       -- Eventually offscreen target to render to (requires create function) */
    rhi_texture_t target;

    /* OS window hosting this surface, or APP_WIN_INVALID if none. Mouse events carry a win_id,
       which is matched against this to find which viewport the cursor is over -- a window only
       hover-tests while the cursor is over its own viewport. */
    i32 win_id;

    /* RHI context driving this surface's swapchain, or RHI_CTX_INVALID. Only set for surfaces
       gui owns (a torn-off floater) -- the host's main viewport gets its swapchain target from
       the host's own command buffer instead, and runs its own frame_begin/end, so this stays
       unset there. */
    i32 rhi_ctx;

    /* True if gui created this surface's OS window and RHI context (a torn-off floater), and so
       must destroy them too. False for the main surface and any surface the host opened itself
       -- gui frees only the GPU buffers for those. */
    bool owned;

    /* Set when the user closes an owned floater's OS window. The surface is actually torn down
       at the next viewport_update (a safe point between building and presenting the frame), not
       immediately, so nothing still drawing to it gets freed out from under it. Ignored for
       surfaces gui does not own. */
    bool pending_close;

    /* Drawable size of this surface in pixels, set before the frame's windows are built
       (viewport 0 by frame_begin, floaters by viewport_resize) windows clip to THIS surface,
       not the main one. 0 means unset, and gui falls back to the main display size.
       Separate from the win_w/win_h passed to flush, which sets the GPU scissor at submit time 
       -- the per-draw clip rects are built from this field instead. */
    i32 disp_w, disp_h;

    /* DPI Data --------------------------------------------------------------------------------- */
    
    /* Per-surface DPI state, for mixed-DPI multi-monitor setups (frame/gui_frame_dpi.c).
       dpi_size_px is the managed font size this surface's windows use, resolved from its own
       monitor's scale -- so surfaces on differently-scaled monitors can use different sizes in
       the same frame. dpi_os_scale is last frame's OS-reported scale, kept so gui can tell an
       OS-driven DPI change (which already resized the window) apart from a gui-driven one
       (a manual ui_scale change where gui must resize the window itself). */

    u32                dpi_size_px;   // landed font size on this surface, px; 0 = unmanaged
    f32                dpi_os_scale;  // OS scale last reported for this surface's window (1.0 = 100%)

    /* Area Data -------------------------------------------------------------------------------- */
    
    /* Height in pixels of the native title bar drawn by a GUI_WIN_NATIVE shell window on this
       surface, and the frame it was last published (window_sync_native). Emit-gated like the
       menu bar band below: read through vp_caption(), which releases the band when no native
       window begins on the surface anymore (e.g. its windows all tabbed into a floating group).
       window_clamp keeps other windows below the live band so their title bars stay clickable. */
    f32 caption_inset;
    u32 caption_seen_frame;

    /* Height of the main menu bar on this surface, and the frame it was last drawn. If the host
       stops calling the menu-bar function, bar_seen_frame falls behind and the band is released.
       vp_work_top adds this to the caption band to size a maximized window's work area. */
    f32 bar_inset;
    u32 bar_seen_frame;

    /* Dock Data -------------------------------------------------------------------------------- */

    /* Extra top band the host reserves above the dock area (e.g. a toolbar strip), set via
       gui()->dockspace_inset() before dockspace_over_viewport. Shrinks the dock tree's layout
       area; free-floating windows are unaffected. 
       Persists until the host sets a new value (0 to clear it). */
    f32 dock_inset;

    /* Root of this surface's dock tree. GUI_DOCK_REF_NONE means windows here are free-floating
       instead of docked. Built by dock/dock.c, re-tiled on drag by dock_drag.c, saved/loaded by
       dock_serialize.c. */
    gui_dock_ref_t dock_root;

    /* Dockspace policy flags, re-published on every dockspace_over_viewport call. 
       NO_SPLIT restricts docking to tabs only -- no split drop zones. */ 
    gui_dockspace_flags_t dock_flags;

    /* Frame this viewport's dockspace was last emitted. Like any immediate-mode element, a
       dockspace is only active on frames the host calls dockspace_over_viewport; on other
       frames it goes dormant -- its tree is kept but its windows stop rendering and it stops
       accepting drag-to-dock. Only dock_clear actually destroys the tree. */
    u32 dock_seen_frame;

    /* Dockspace maximize: dock_max_id names one leaf pinned over the whole dock area, covering
       its siblings (0 = none maximized). Un-maximizing eases the leaf back to its normal rect;
       dock_max_on goes false as soon as that tween starts, and dock_max_id clears once it
       finishes. dock_max_settled is true only once the maximize (not un-maximize) tween has
       finished -- until then covered siblings keep drawing, since they are still partly
       visible. dock_max_from is the rect the tween eases from, captured when maximize/
       un-maximize is toggled. See dock_max_set / dock_max_node in gui_dock_core.c. */

    gui_rect_t    dock_max_from;
    gui_dock_id_t dock_max_id;
    bool          dock_max_on;
    bool          dock_max_settled;

} gui_viewport_t;

/* Drawable width/height of viewport vp, falling back to s_io's display size if unset
   (core/gui_ctx.c). Takes a slot index rather than a pointer since s_vp_pool is a fixed global
   table and an index stays valid across calls. */

f32 vp_w( i32 vp );
f32 vp_h( i32 vp );

/* Caption band published on viewport vp by a native window, emit-gated: 0 unless published this
   build or the one before (core/gui_ctx.c). Every reader of caption_inset goes through this;
   only the publisher (window_sync_native) touches the raw field. */

f32 vp_caption( i32 vp );

/* Top of viewport vp's work area: vp_caption plus the main-menu-bar band while emitted
   (core/gui_ctx.c). Title bars must stay reachable below this line. */

f32 vp_work_top( i32 vp );

/* Is a dockspace laid over viewport vp, emit-gated on the same one-frame tolerance as the bands
   above (core/gui_ctx.c)?  Describes the SURFACE -- what content below pinned chrome looks like.
   The stricter "may this tree place windows and take drops right now" is dock_vp_emitted, in
   chrome/dock/. */

bool vp_docked( i32 vp );

/* Resolve a caller-supplied viewport index to a live slot: GUI_VP_INVALID, out-of-range, and
   torn-down slots all map to the primary (GUI_VP_MAIN).  Run by record-less root callers
   (pane, region) on every open (core/gui_ctx.c). */

i32 vp_resolve( i32 vp );

/*==============================================================================================
    The global viewport table (lifecycle owned by frame/gui_viewport.c; defined in
    core/gui_ctx.c)

    OS windows and RHI contexts are a small, fixed-size resource (sized to APP_WIN_MAX); window
    records name the slot they draw into. [0] is always the main swapchain.
==============================================================================================*/

extern gui_viewport_t s_vp_pool[ APP_WIN_MAX ];
extern i32            s_vp_count;                   /* used count; iterate [0, count) */

/* Finds which viewport slot is hosting the given OS window, by searching s_vp_pool. Used by the
   mouse-input path (core/gui_io.c) to map an event's win_id to a viewport. ORB_UNUSED_FN
   because this header is also included by the backend TU, which does not use it. */

static i32 ORB_UNUSED_FN viewport_index_for_window( i32 win_id );

/*==============================================================================================
    gui_context_t -- the UI's persistent state: everything that must survive between frames.

    One static instance (g_ctx_store below, reached as g_ctx).  Input (s_io) and the per-frame
    build scratch (s_build) stay outside it: input is a physical resource with no per-frame
    lifetime question, and scratch is wiped at every ctx_begin.
==============================================================================================*/

typedef struct
{
    gui_retained_t   retained;      // frame clock + keyed state pool
    gui_nav_state_t  nav;           // nav cursor location + menu-bar mode

    struct                          /* the open-popup stack */
    {
        gui_popup_t  open[ GUI_POPUP_DEPTH ];   // open popup set, ordered parent -> child
        u32          open_count;                // live open count
    } popup;

    struct                          /* GUI_WIN_MODAL fence (window_modal_apply) */
    {
        gui_id_t     win_id;        // id of the modal overlay window; 0 = none
        u32          seen_frame;    // frame it last emitted; fence releases when it stops
    } modal;

    struct                          /* persisted window records (core/gui_surface.c) */
    {
        gui_window_t  pool[ GUI_MAX_WINDOWS ];  // persisted window records
        u32           count;        // live records in the pool
        gui_window_t  scratch;      // fallback used when the pool is full
        u32           z_counter;    // paint-order counter; each raise takes the next value
        u32           cascade;      // next cascade offset for a window with no saved position
    } win;

    /* Render surfaces live in s_vp_pool, not here -- see above. */

    struct                          /* dock-tree node pool (dock/) */
    {
        gui_dock_node_t  pool[ GUI_DOCK_NODES ];    // dock-tree nodes; a free slot has id == 0
        u32              count;     // high-water slot count used
        u32              id_seq;    // next node id to hand out; 0 = none yet
    } dock;

} gui_context_t;

/* Scratch for "what is being emitted right now", reset every frame as the widget tree is
   walked.  One global instance: the build runs on a single thread. */
typedef struct
{
    gui_win_ctx_t win;                  // the window currently between window_begin / window_end

    bool          wheel_used;           // true once some region has consumed the mouse wheel this frame

    u32           nav_region_seq;       // next region id to hand out this frame
    u32           nav_line_seq;         // next line id to hand out this frame

    gui_item_flags_t item_flags;        // merged flags from the item-flag stack
    gui_item_flags_t next_set;          // which flags the next-item override changes
    gui_item_flags_t next_val;          // their overridden values

    bool          combo_open;           // a combo dropdown's body is being emitted right now
    bool          combo_item_clicked;   // a selectable inside it was clicked this frame

} gui_build_t;

/*============================================================================================*/
/* exported context data */

/* The one context, defined in core/gui_ctx.c.  Every reader spells it g_ctx-> so retained state
   reads as distinct from the s_* frame scratch at each call site; the macro makes that a direct
   static access rather than a pointer load. */

extern gui_context_t  g_ctx_store;
#define g_ctx ( &g_ctx_store )

extern gui_build_t    s_build;          // core/gui_ctx.c -- frame-build scratch

/*==============================================================================================
    Context lifecycle (core/gui_ctx.c)
==============================================================================================*/

void           ctx_reset     ( void );                  /* zero the context (gui_init)        */
void           ctx_new_frame ( void );                  /* per-build scratch reset            */

void           interact_new_frame( void );         /* once per APP frame (frame_begin)   */
void           cursor_flush  ( void );                  /* push last frame's cursor to the OS */

// clang-format on
/*============================================================================================*/
#endif    // GUI_CTX_H
