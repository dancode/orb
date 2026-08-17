#ifndef GUI_CTX_H
#define GUI_CTX_H
/*==============================================================================================

    runtime_service/gui/core/gui_ctx.h -- retained-mode storage for the interact server.

    Defines every record the interact server keeps between frames: windows, the nav cursor,
    viewports, scroll offsets, and the gui_context_t that groups them. The behavior that reads
    and updates these records (window dragging, docking, popups) lives elsewhere in chrome/ and
    frame/ -- this header only defines the storage shape.

    Two record types (the popup stack, dock-tree nodes) appear here only as pointers; their full
    definitions live in chrome/gui_chrome.h, which also allocates them.

    Include order: after core/gui_core.h (gui_context_t embeds gui_retained_t).

==============================================================================================*/
// clang-format off

/* Defined in chrome/gui_chrome.h. gui_context_t stores pointers to these but never reads
   their fields. */

typedef struct gui_popup_t     gui_popup_t;
typedef struct gui_dock_node_t gui_dock_node_t;

/* Forward declaration so earlier records can hold a gui_context_t* before the real struct
   is defined below (avoids MSVC C4115 when the tag first appears inside a parameter list). */
struct gui_context_t;

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
gui_window_t* window_find( gui_id_t id );                                // find in the bound context, or NULL
gui_window_t* window_find_in( struct gui_context_t* ctx, gui_id_t id );  // find in a specific context, or NULL
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

    /* The context win_id belongs to, captured when the request is queued. The context bound
       when it is later resolved may be different, so this must be saved rather than looked up again. */

    struct gui_context_t* owner;

} gui_vp_request_t;

const gui_next_win_t* gui_next_win_peek( void );    // core/gui_surface.c -- read-only queue peek
extern gui_vp_request_t s_vp_request;               // core/gui_surface.c

/*==============================================================================================
    Keyboard navigation state (behavior in chrome/nav/gui_nav.c)

    Tracks which widget the keyboard cursor is on -- the keyboard equivalent of mouse hover,
    moved by arrow keys and Tab instead of the mouse -- plus the menu-bar state machine built on
    top of it. Each gui_context_t has its own copy (g_ctx->nav).
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

/* Index into gui_context_t's dock-node pool (not the same as gui_dock_id_t, the stable handle
   callers use). The pool never compacts, so an index stays valid across frames just like a
   pointer would, but takes 2 bytes instead of 8. dock_ref()/dock_at() (gui_dock_core.c) convert
   between this and a live pointer. */

typedef u16 gui_dock_ref_t;
#define GUI_DOCK_REF_NONE ( (gui_dock_ref_t)0xFFFFu )

/*==============================================================================================
    Viewport -- The gui render surface (managed in frame/gui_viewport.c)

    Controls the gui specific state over a render context draw area.

    The viewport has GPU render buffers, a color targets, and the OS window hosting it, 
    its drawable size and reserved chrome bands, and its dock tree. etc.
    
    Slot [0] is the main swapchain; the rest are floating windows torn off from it. 
    Viewports are global (s_vp_pool) not per-context (every gui_context_t shares the table).
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

/* Resolve a caller-supplied viewport index to a live slot: GUI_VP_INVALID, out-of-range, and
   torn-down slots all map to the primary (GUI_VP_MAIN).  Run by record-less root callers
   (pane, region) on every open (core/gui_ctx.c). */

i32 vp_resolve( i32 vp );

/*==============================================================================================
    The global viewport table (lifecycle owned by frame/gui_viewport.c; defined in
    core/gui_ctx.c next to s_ctx_pool)

    Every gui_context_t shares this one table rather than keeping its own -- OS windows and RHI
    contexts are a small, genuinely global resource (sized to APP_WIN_MAX). A context only
    differs in which of its own windows assign into which slot. [0] is always the main
    swapchain.
==============================================================================================*/

extern gui_viewport_t s_vp_pool[ APP_WIN_MAX ];
extern i32            s_vp_count;                   /* used count; iterate [0, count) */

/* Finds which viewport slot is hosting the given OS window, by searching s_vp_pool. Used by the
   mouse-input path (core/gui_io.c) to map an event's win_id to a viewport. ORB_UNUSED_FN
   because this header is also included by the backend TU, which does not use it. */

static i32 ORB_UNUSED_FN viewport_index_for_window( i32 win_id );

/*==============================================================================================
    gui_context_t -- one UI's persistent state ("bind it, then emit into it")

    Code binds one context (ctx_bind) and emits all of that UI's windows into it; the context
    holds everything that must survive between frames for that UI. Switching contexts is just
    reassigning the g_ctx pointer -- no copying.

    Some state stays global instead of living here: input (s_io), since there is one physical
    mouse/keyboard, and per-frame build scratch (s_build), reused by whichever context is
    building. `listening` controls whether a bound context responds to input this frame; when
    false it still renders, but hover/click/nav are inert.
==============================================================================================*/

typedef struct gui_context_t
{
    gui_retained_t   retained;      // id salt, frame clock, keyed state pool (ptr into alloc)
    gui_nav_state_t  nav;           // nav cursor location + menu-bar mode

    struct                          /* the open-popup stack */
    {
        gui_popup_t* open;          // open popup set, ordered parent -> child; ptr into alloc
        u32          open_count;    // live open count
        u32          depth;         // capacity (max nesting depth)
    } popup;

    struct                          /* GUI_WIN_MODAL fence (window_modal_apply) */
    {
        gui_id_t     win_id;        // id of the modal overlay window; 0 = none
        u32          seen_frame;    // frame it last emitted; fence releases when it stops
    } modal;

    struct                          /* persisted window records (core/gui_surface.c) */
    {
        gui_window_t* pool;         // persisted window records; ptr into alloc
        u32           count;        // live records in the pool
        u32           max;          // capacity
        gui_window_t  scratch;      // fallback used when the pool is full
        u32           z_counter;    // paint-order counter; each raise takes the next value
        u32           cascade;      // next cascade offset for a window with no saved position
    } win;

    /* Render surfaces live in s_vp_pool, not here -- see above. A viewport is a real OS window /
       RHI context, a genuinely global resource; contexts only differ in which of their own
       windows (win.pool) assign into which global slot. */

    struct                          /* dock-tree node pool (dock/) */
    {
        gui_dock_node_t* pool;      // dock-tree nodes; NULL when max == 0
        u32              count;     // high-water slot count used
        u32              id_seq;    // next node id to hand out; 0 = none yet
        u32              max;       // capacity; 0 = docking disabled
    } dock;

    bool  listening;    // true: this context receives hover/click/nav input this frame
    void* _alloc;       // single allocation backing this struct + all pool arrays above;
                         // freed at teardown (shutdown for slot 0, ctx_destroy for others)
    u32   _alloc_size;  // size of _alloc, for gui_mem_stats()

} gui_context_t;

/* Scratch for "what is being emitted right now", reset every frame as the widget tree is
   walked. One global instance is enough because contexts build one at a time on a single
   thread. */
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

extern gui_context_t* g_ctx;            // core/gui_ctx.c -- the bound context
extern gui_build_t    s_build;          // core/gui_ctx.c -- frame-build scratch

/*==============================================================================================
    Context pool + lifecycle (storage here in core/gui_ctx.c; the public create/destroy API
    lives in frame/gui_context.c, since sizing the allocation needs chrome's record types too)
==============================================================================================*/

#define GUI_CTX_POOL_MAX  8             /* slot 0 = default + up to 7 secondary contexts */

extern gui_context_t* s_ctx_pool[ GUI_CTX_POOL_MAX ];   /* allocated context data */
extern u32            s_ctx_pool_count;                 /* live slot count; always >= 1 after init */

void           ctx_bind      ( gui_context_t* ctx );    /* NULL rebinds the default           */
void           ctx_new_frame ( void );                  /* per-context scratch reset          */

void           interact_new_frame( void );         /* once per APP frame (frame_begin)   */
void           cursor_flush  ( void );                  /* push last frame's cursor to the OS */

// clang-format on
/*============================================================================================*/
#endif    // GUI_CTX_H
