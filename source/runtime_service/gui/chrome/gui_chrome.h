#ifndef GUI_CHROME_H
#define GUI_CHROME_H
/*==============================================================================================

    runtime_service/gui/chrome/gui_chrome.h -- managed windowing (the chrome unit).

    The stock recipes: window policy, keyboard nav, popups, docking -- policy over the
    mechanism units below.  The RETAINED RECORDS chrome drives (gui_window_t, the nav state,
    the viewport) live in the interact server's storage header (core/gui_ctx.h): the
    server owns retained-mode storage, chrome owns the behavior over it.  Two records whose
    shape only chrome reads stay HERE behind core's forward declarations -- the popup stack
    entry (it embeds the cross-unit overlay save) and the dock-tree node; frame's allocator
    (frame/gui_context.c) sizes them with full visibility.

    Implementation: the six folders under chrome/ (widgets, table, window, dock, popup,
    nav); unit root gui_chrome.c.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    Popup stack entry (gui_context_t popup.open; driver in chrome/popup/gui_popup.c)

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

struct gui_popup_t              /* typedef'd gui_popup_t in core/gui_ctx.h (opaque to core) */
{
    gui_id_t            id;                 // popup window id (salted; matches s_build.win.id / hover_win)
    bool                modal;              // blocks input behind it + dims the background
    f32                 anchor_x;           // open point -- where a non-modal popup is placed
    f32                 anchor_y;           //
    u32                 open_frame;         // frame popup_open ran -- "appearing" detection
    u32                 begun_frame;        // last frame popup_begin ran -- drives stale-close
    gui_rect_t          rect;               // on-screen rect last frame -- drives click-outside
    gui_overlay_save_t  saved;              // parent context to restore at popup_end
};

/*==============================================================================================
    Dock node (behavior in gui_dock.c)

    One node of a viewport's dock tree.  A node is either a LEAF (split == GUI_DOCK_SPLIT_NONE),
    which tabs one or more windows into a single region, or an INTERNAL split (GUI_DOCK_SPLIT_X /
    _Y), which divides its rect between two children at `ratio` with a draggable splitter between
    them.  Nodes live in a fixed per-context pool (gui_context_t dock.pool) so child / parent pool
    indices (gui_dock_ref_t, core/gui_ctx.h) stay valid across frames; a freed slot has id == 0.

    rect / content are resolved every frame by dock_node_layout from the viewport extent down: rect is
    the node's whole box, content is the leaf's body below its tab strip (where the active window draws).
==============================================================================================*/

#define GUI_DOCK_TABS_MAX           8       // windows co-docked (tabbed) in one leaf node
#define GUI_DOCK_NAME_CAP           28      // bytes of a tab's display name, copied at dock time

typedef enum
{
    GUI_DOCK_SPLIT_NONE = 0,    /* leaf -- tabs windows; child[] unused                 */
    GUI_DOCK_SPLIT_X,           /* internal -- vertical split, children side by side    */
    GUI_DOCK_SPLIT_Y,           /* internal -- horizontal split, children top / bottom  */

} gui_dock_split_t;

struct gui_dock_node_t          /* typedef'd gui_dock_node_t in core/gui_ctx.h (opaque to core) */
{
    gui_id_t    id;                          /* stable node handle; 0 = free pool slot            */
    gui_vp_t    viewport;                    /* surface (viewport index) this tree belongs to     */
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
};

/*==============================================================================================
    Frame steps + upward seams -- the few chrome definitions the frame unit calls UP into
==============================================================================================*/

void window_raise_on_press( void );   /* chrome/window/gui_window.c: press-to-front (ctx_begin)      */
void window_modal_apply   ( void );   /* chrome/popup/gui_popup.c: modal fence (ctx_begin)           */
void popup_apply_modal    ( void );   /* chrome/popup/gui_popup.c: per-frame modal inertness         */
void popup_close_check    ( void );   /* chrome/popup/gui_popup.c: click-outside close (frame)       */
void nav_new_frame        ( void );   /* chrome/nav/gui_nav.c: per-frame nav turnover                */
void dock_hidden_refresh  ( void );   /* chrome/dock/gui_dock_core.c: hidden-node upkeep (frame)     */
void windows_dpi_rescale  ( gui_vp_t vp, f32 ratio ); /* chrome/window/gui_window_free.c: one surface's DPI step */
u32  chrome_unit_mem_bytes( void );

/* gui_popup.c is included after the widgets/ files; selectable calls this to auto-close the
   enclosing popup on click (the Dear ImGui CloseCurrentPopup default behavior). */
void gui_popup_close_current( void );

/* Window text selection (chrome/window/gui_select.c, chrome -- it reads the render capture
   and font metrics, so it is policy astride both servers, not an interact mechanism).  The
   bands paint under the body at the window begins; the protocol + overlay resolve at end. */
void select_paint_under( void );
void select_window_end( void );

/* scrollbar_widget -- the region gutter's ONE widget -- is declared in flow/gui_flow.h:
   flow is its caller (the sanctioned flow -> chrome seam), and upward-seam declarations
   live with their LOWEST consumer. */

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

// ORB_UNUSED_FN: this header is also pulled into the backend TU, which neither defines nor
// calls these; a bare static declaration would read as dead code there.

static gui_win_route_t ORB_UNUSED_FN window_route_resolve( gui_id_t id, const char* title,
                                                           gui_window_t* win );
static void            ORB_UNUSED_FN window_route_drag   ( gui_id_t id, gui_window_t* win );
static void            ORB_UNUSED_FN window_route_commit ( gui_id_t id, const char* title );
static void            ORB_UNUSED_FN window_route_chrome ( gui_dock_node_t* node );
static bool            ORB_UNUSED_FN window_route_raise  ( gui_id_t id );

// clang-format on
/*============================================================================================*/
#endif    // GUI_CHROME_H
