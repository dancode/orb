#ifndef GUI_CHROME_H
#define GUI_CHROME_H
/*==============================================================================================

    runtime_service/gui/chrome/gui_chrome.h -- managed windowing (the chrome unit).

    The stock recipes: window policy, keyboard nav, popups, docking -- policy over the
    mechanism units below.  The RETAINED RECORDS chrome drives (gui_window_t, the nav state,
    the viewport, the popup stack entry, the dock node) live in the interact server's storage
    header (core/gui_ctx.h): the server owns retained-mode storage, chrome owns the behavior
    over it.  The one record that stays HERE is the popup overlay save, since it embeds flow
    and render types core cannot see.

    Implementation: the six folders under chrome/ (widgets, table, window, dock, popup,
    nav); unit root gui_chrome.c.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    Popup overlay save (chrome/popup/gui_popup.c)

    The parent state popup_end restores: whole-struct copies of the window context, the
    interaction scope, and the draw scope, plus the parent's top layout frame (its pen must
    survive the popup's region pop).  Because the copies are wholesale, nothing here is
    maintained field-by-field -- a field added to any of those records rides the seam
    automatically.  One per popup depth, parallel to g_ctx->popup.open (core/gui_ctx.h).
==============================================================================================*/

typedef struct
{
    gui_win_ctx_t    win;      // s_build.win -- the flat window context
    gui_scope_t      scope;    // s_scope -- the interaction scope (behavior contract)
    gui_draw_scope_t draw;     // backend paint cursor + ambient glyph-clip window

    bool             had_parent;    // a layout region was open (parent frame valid)
    layout_frame_t   parent_frame;  // the parent's top frame, restored after the popup

} gui_overlay_save_t;

/*==============================================================================================
    Frame steps + upward seams -- the few chrome definitions the frame unit calls UP into
==============================================================================================*/

void window_raise_on_press( void );   /* chrome/window/gui_window.c: press-to-front (ctx_begin)      */
void window_modal_apply   ( void );   /* chrome/popup/gui_popup.c: modal fence (ctx_begin)           */
void popup_apply_modal    ( void );   /* chrome/popup/gui_popup.c: per-frame modal inertness         */
void popup_close_check    ( void );   /* chrome/popup/gui_popup.c: click-outside close (frame)       */
void nav_new_frame        ( void );   /* chrome/nav/gui_nav.c: per-frame nav turnover                */
void dock_hidden_refresh  ( void );   /* chrome/dock/gui_dock_core.c: hidden-node upkeep (frame)     */
void windows_dpi_rescale  ( i32 vp, f32 ratio ); /* chrome/window/gui_window_free.c: one surface's DPI step */
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
static bool            ORB_UNUSED_FN window_route_is_drop_target( gui_id_t id );

// clang-format on
/*============================================================================================*/
#endif    // GUI_CHROME_H
