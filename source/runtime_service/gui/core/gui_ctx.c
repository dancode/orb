/*==============================================================================================

    runtime_service/gui/core/gui_ctx.c -- Immediate-mode context state and per-frame drivers.

    Declares all persistent ambient and frame-scratch state (s_interaction, s_build, nav_state_t,
    layout_frame_t, gui_context_t) and drives the per-frame lifecycle via ctx_new_frame.

    ID hashing and the keyed state pool (id_hash, id_combine, id_seed/push/pop, gui_state_get,
    GUI_STATE) are in gui_ctx_id.c, included just after this file.

    Public IO accessors (gui_want_capture_*, gui_is_key_*, gui_is_mouse_*, etc.) are in
    gui_ctx_io.c, also included after this file.

    Included by gui.c after gui_input.c so s_io is in scope.

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   /* nav_state_t, layout_frame_t, gui_popup_t,
                                                        gui_retained_t, gui_viewport_t, gui_context_t */
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* gui_window_t is defined early in gui.c (it is embedded by value in the context below);
   s_build.cur_win points at a pool record so window_end can write scroll_y / content_h back. */

/* Ambient interaction state -- the one user's live hover / active / focus, persisting across
   frames.  There is one pointer, one keyboard, one mouse, so none of this is ever duplicated per
   viewport: when the multi-context model lands this stays a single global the focused context is
   bound to (an adapter), never per-context state.  Tier: ambient singular.  See the three-tier
   note in gui.c. */

static struct
{
    gui_id_t  hover_id;         // widget under the cursor this frame (rebuilt each frame)
    gui_id_t  active_id;        // widget with the mouse button held (drag / hold)
    gui_id_t  active_id_prev;   // active_id as of the end of the previous frame (snapshot at new_frame)
    u8        active_button;    // which button holds active_id (0=left); reset to 0 on release
    gui_id_t  focused_id;       // widget that owns keyboard input

    /* Auto-repeat timing for the held button (GUI_ITEM_BUTTON_REPEAT).  Only one widget is active
       at a time, so a single timer suffices: repeat_t accumulates held time since the last fire, and
       repeat_on flips true once the initial delay has elapsed (switching to the faster rate).  Both
       are reset on the press frame, so a new button starts its own cadence. */

    f32         repeat_t;
    bool        repeat_on;

    /* Window occlusion is resolved one frame deferred: the single window the cursor is
       over (front-most by z) is only known after every window has been submitted.
       Each window_begin nominates itself into next_hover_win; ctx_new_frame promotes it to
       hover_win.  Next frame a window compares its id against hover_win at entry -- if it
       isn't the hover window it cannot be hit, so it (and all its widgets) skip hit-testing
       entirely.  Only the hover window does widget hit-testing, and within one window
       widgets don't overlap, so widget hover can be resolved immediately (no deferral). */

    gui_id_t  hover_win;      // the window the cursor is over (resolved last frame)
    gui_id_t  next_hover_win; // front-most window nominee gathered this frame
    u32       next_hover_win_z;

    /* Hardware-cursor request.  Widgets nominate a shape during the build (set_mouse_cursor); the
       last writer wins -- safe because exactly one widget hovers per frame, and the resize bands
       suppress widget hover so a window edge and a widget never both request.  cursor_flush pushes
       it to the OS window under the pointer at the next frame_begin (deferred one frame, like
       hover_win).  Reset to APP_CURSOR_ARROW each frame by interaction_frame_reset. */
    app_cursor_t mouse_cursor;

    /* Focus-departure tracking for is_item_deactivated_after_edit.
       focused_id_at_frame_start snapshots focused_id at interaction_frame_reset.  At frame_end, if
       focused_id changed this frame, the departing widget's id and edit history are latched into
       focus_ended_* for one frame so the next frame's is_item_deactivated_after_edit can read them.
       focused_id_edited accumulates while a widget holds focus (set by mark_item_edited in
       input_field_edit); cleared and snapshotted into focus_ended_edited on departure. */
    gui_id_t focused_id_at_frame_start;
    bool     focused_id_edited;
    gui_id_t focus_ended_id;
    bool     focus_ended_edited;

} s_interaction;

/* Frame-build scratch -- the "where am I emitting right now" context, rebuilt every frame as the
   widget tree is walked.  Nothing here survives begin_frame: it is set and repopulated by the
   window_begin / child_begin / widget calls, never read across a frame boundary.  Because contexts
   build sequentially on one thread, this stays a single global builder reused by each context in
   turn rather than per-context state.  Tier: frame scratch. */

static struct
{
    gui_id_t  last_item_id;   // id of the most recent widget emitted -- context-menu / tooltip anchor

    /* Last-item introspection (the Dear ImGui IsItem* model).  widget_behavior latches the rect and
       the resolved interaction state of each emitted item here, so the item-query readers
       (gui_ctx_io.c) report on "the widget just emitted" with no per-widget bookkeeping -- the same
       anchor last_item_id already provides for context menus / tooltips, widened to the full result. */
    gui_rect_t   last_item_rect;     // screen rect of the most recent widget
    widget_state_t last_item_status; // its resolved hover / active / clicked / focused / nav flags

    u32         cur_viewport;       // ambient viewport for new-window inheritance (updated per window emitted)

    gui_id_t  win_id;               // id of the window currently between begin/window_end
    const char* win_title;          // title string, cached for window_end's deferred chrome
    bool        win_collapsed;      // current window is collapsed (title bar only this frame)
    bool        win_hidden;         // current window is CLOSEABLE + closed: begin emitted nothing, end early-outs
    gui_win_flags_t win_flags;      // current window's behavior flags (window_begin arg)
    f32         win_title_h;        // current window's title bar height (0 if NOTITLEBAR)
    u8          win_resize_hot;     // resize edges hot this frame -- suppresses widget hover
    bool        win_grip_hot;       // cursor over the CAN_AUTOSIZE grip -- suppresses widget hover
    struct gui_window_t* cur_win;   // persisted window record; scroll write-back target

    /* Docking (gui_dock.c): the node hosting the current window, or NULL when it is free-floating.
       When set, the window's geometry is owned by the node and its title bar is replaced by the
       node's tab strip; win_dock_active distinguishes the visible tab (draws a body) from a window
       that is docked but behind another tab (window_begin returns false, draws nothing). */
    struct gui_dock_node_t* cur_dock_node;
    bool        win_dock_active;    // current docked window is its node's active (visible) tab

    f32  win_x, win_y;        // current window top-left (outer frame)
    f32  win_w, win_h;        // current window dimensions

    gui_rect_t menubar_rect; // reserved menu-bar strip for the current window (WIN_MENUBAR); menu_bar_begin fills it

    /* Layout pen + scroll region state now live on the layout-frame stack below; the
       window is just the root frame.  s_build keeps only the cross-cutting per-frame
       context the chrome and widgets read regardless of which region is active. */

    gui_rect_t clip_rect;   // active interaction clip -- widget hover is gated by it
    bool         wheel_used;  // a region consumed the wheel this frame (innermost wins)

    /* Item flags -- the push-model behavior set a widget reads at emit time (see gui_item_flags_t).
       item_flags is the merged top of the push/pop stack; next_set / next_val are the one-shot
       override for the next widget (which bits it controls + their values); cur_item_flags is the
       value resolved for the widget currently being emitted, latched by item_flags_resolve and read
       by widget_behavior and the widgets.  All reset to 0 each frame. */

    gui_item_flags_t item_flags;       // merged top-of-stack item flags
    gui_item_flags_t next_set;         // bits the next-item override controls
    gui_item_flags_t next_val;         // their values
    gui_item_flags_t cur_item_flags;   // flags resolved for the item being emitted

    /* Combo dropdown coordination (gui_widget_combo.c).  combo_begin sets combo_open true while
       emitting its popup body; a selectable clicked in that body sets combo_item_clicked, and
       combo_end closes the dropdown when it sees it -- so picking an item dismisses the combo with
       no caller code, exactly as a selection should.  Both are scoped to the combo body and reset
       each frame as a safety net. */

    bool               combo_open;          // a combo dropdown body is currently being emitted
    bool               combo_item_clicked;  // a selectable in that body was clicked this frame

    /* Keyboard navigation state lives in its own subsystem struct (nav_state_t s_nav, below) so
       the per-frame build scratch does not bloat it.  See gui_nav.c for the driver and
       nav_item_register (gui_widget_core.c) for the per-item seam. */

} s_build;

#ifdef GUI_DEBUG_OVERLAY
/* The debug overlay (gui_debug_overlay.c) lives in the render backend unit and tags each captured rect
   with the ambient build viewport.  s_build is private to this unit, so the overlay reads it
   across the unit seam through this accessor (declared in gui_backend.h, Debug builds only). */
u32 gui_dbg_build_viewport( void ) { return s_build.cur_viewport; }
#endif

/*----------------------------------------------------------------------------------------------
    Keyboard navigation state (s_nav)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather
    than the mouse -- plus the menu-bar state machine layered on top of it.  The type is defined
    here; the instance is a member of the bound context (gui_context_t, below) reached via the
    s_nav alias, apart from the ambient s_interaction / frame-scratch s_build.  gui_nav.c drives
    it and nav_item_register (gui_widget_core.c) is the per-item seam.

    `win` is the window or popup nav is scoped to, chosen each frame the way hover_win is (a popup
    captures it while open).  Movement is resolved one frame deferred against `ref_rect`, exactly
    like hover_win lags the cursor: the request is set at nav_new_frame, every item in `win`
    registers itself through widget_behavior during emission, and the winner is committed at the
    next nav_new_frame.
----------------------------------------------------------------------------------------------*/

/* nav_state_t (the nav cursor + menu-bar state machine) is defined in gui_internal.h. */

/* s_nav lives in the bound context (gui_context_t, below) and is reached through the g_ctx alias,
   so each context keeps its own nav cursor location. */

/*----------------------------------------------------------------------------------------------
    Item-flag stack

    push_item_flag saves the current merged value here and pop_item_flag restores it, so a push
    nests cleanly regardless of which bits it touched.  An over-deep push aliases the top slot and
    is still counted truthfully, mirroring the id / layout stacks, so push/pop stay paired.
----------------------------------------------------------------------------------------------*/

#define GUI_ITEM_FLAG_DEPTH 16

static gui_item_flags_t s_item_flag_stack[ GUI_ITEM_FLAG_DEPTH ];
static u32                s_item_flag_sp;

/* Disabled items draw at this opacity (the rest of the dim is in the draw list's global alpha). */
#define GUI_DISABLED_ALPHA 0.5f

/* Push: save the current merged flags, then set or clear `flag` in the live set. */
static void
item_flag_push( gui_item_flags_t flag, bool enable )
{
    if ( s_item_flag_sp < GUI_ITEM_FLAG_DEPTH )
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
    u32 i = s_item_flag_sp < GUI_ITEM_FLAG_DEPTH ? s_item_flag_sp : GUI_ITEM_FLAG_DEPTH - 1;
    s_build.item_flags = s_item_flag_stack[ i ];
}

/* Next-item override: mark `flag` as controlled for the next widget, with its on/off value.
   Consumed (and cleared) by item_flags_resolve when that widget emits -- no pop needed. */
static void
item_flag_next( gui_item_flags_t flag, bool enable )
{
    s_build.next_set |= flag;
    if ( enable ) s_build.next_val |=  flag;
    else          s_build.next_val &= ~flag;
}

/* Resolve the flags for the item now emitting: the stack value with the one-shot next-item override
   applied over it (the override wins on the bits it controls), then clear the override.  Latches the
   result for widget_behavior / widgets to read, and sets the draw alpha so a disabled item dims with
   no per-widget code.  Called once per item from widget_next_rect_w (the universal emit seam). */
static gui_item_flags_t
item_flags_resolve( void )
{
    gui_item_flags_t f = ( s_build.item_flags & ~s_build.next_set ) | ( s_build.next_val & s_build.next_set );
    s_build.next_set = 0;
    s_build.next_val = 0;
    s_build.cur_item_flags = f;

    /* Same seam for the style stacks: promote any next_style_* override into the active per-item
       layer so it applies for this widget's whole draw, then clears for the following one. */
    style_item_commit();

    draw_set_alpha( ( f & GUI_ITEM_DISABLED ) ? GUI_DISABLED_ALPHA : 1.0f );
    /* Default this widget's rects to the control-frame radius (base + push/pop + next-* override).
       A widget that draws a grab (slider knob, scrollbar) or a squared-off mark (check, bullet)
       overrides draw_set_rounding locally for that sub-element.  (style_var directly: the ROUND_*
       macros live in gui_widget_core.c, included after this file.) */
    draw_set_rounding( style_var( GUI_VAR_WIDGET_ROUNDING ) );
    return f;
}

/* Clear the per-item state before chrome runs.  Window/child borders, scrollbars, titlebars, and
   the collapse arrow are not items -- they never go through widget_next_rect_w, so without this they
   would inherit whatever the last body widget latched (a disabled trailing widget would dim the
   border and deaden the scrollbar).  Called at the chrome seams (begin/window_end, child_begin,
   layout_pop_region) so chrome always interacts undisabled and paints opaque. */
static void
item_flags_chrome_reset( void )
{
    s_build.cur_item_flags = GUI_ITEM_NONE;
    draw_set_alpha( 1.0f );
    style_chrome_reset();   /* drop lingering next_style_* overrides; keep the push/pop stack */
    /* Chrome (window / child / dock backgrounds, title bars, borders) defaults to the window radius,
       read after the item override is cleared so a trailing widget's next-* radius cannot leak in. */
    draw_set_rounding( style_var( GUI_VAR_WIN_ROUNDING ) );
}

/*----------------------------------------------------------------------------------------------
    Layout-frame stack

    - Every scrollable region (a window body or a child_begin box) pushes one frame.  
    - The top frame owns the layout pen and the content column the leaf widgets emit into.
    - The rest of the struct is the resolve context layout_pop_region needs to measure 
      content and draw the region's scrollbars.
    - The pen fields used to live flat in the window context; nesting moved them here.

    - Memory is just the fixed array -- a frame carries only what is needed to emit widget 
      rects and resolve scroll at pop, so a deep nesting costs nothing beyond these slots.
----------------------------------------------------------------------------------------------*/

/* GUI_LAYOUT_DEPTH is defined in gui_internal.h. */

/* layout_frame_t (one scroll-region layout frame) is defined in gui_internal.h. */

static layout_frame_t s_layout_stack[ GUI_LAYOUT_DEPTH ];
static u32            s_layout_sp;   // active frame count; top = s_layout_sp - 1

/* Top layout frame.  Valid between a window_begin/child_begin and its matching end.  When the
   stack is empty (a caller emitted a widget into a collapsed window despite the false return)
   slot 0 is returned instead of indexing out of bounds -- the stray widget draws into whatever
   the last frame's root region left there rather than crashing.  The read index is also clamped
   to the top slot so an over-deep nesting (capped in layout_push_region) never reads past the
   array. */

static layout_frame_t*
lf( void )
{
    u32 i = s_layout_sp ? s_layout_sp - 1 : 0;
    if ( i >= GUI_LAYOUT_DEPTH ) i = GUI_LAYOUT_DEPTH - 1;
    return &s_layout_stack[ i ];
}

/*----------------------------------------------------------------------------------------------
    Popup stack

    The set of currently-open popups *is* a stack, ordered parent -> child: index 0 is the
    top-level popup, each deeper index a popup opened while inside the one above.  This single
    array is the source of truth for open / close, nesting, and the click-outside policy; the
    popups themselves are rendered as ordinary windows on a reserved high z-band (see gui_popup.c).

      s_popup_open_count  -- persists across frames; the live open set is [0, count).
      s_popup_begin_count -- rebuilt each frame; the current popup nesting depth while emitting
                             (0 at top level, 1 inside one popup_begin, ...).  popup_open writes
                             a request at this depth; popup_begin matches its id against it.

    A popup is a *top-level overlay*: it is begun while a parent window is still open, but it must
    lay out, clip, and paint independent of that parent (a context menu escapes the window's
    bounds, paints above it, and must not disturb its layout pen).  gui_overlay_save_t snapshots
    exactly the cross-cutting state begin/window_end mutate -- the flat window context, the
    interaction clip, the draw sort key, and the parent's top layout frame -- so popup_end can
    restore the parent verbatim.  The stack *counters* (layout / id / clip depth) are left intact
    and balance naturally through the normal push/pop, so no slot is ever reused or lost. */

/* gui_overlay_save_t (parent context a popup saves/restores) is defined in gui_internal.h. */

/* gui_popup_t (one open popup record) is defined in gui_internal.h. */

/* The open set (s_popups_open) and its live count (s_popup_open_count) are members of the bound
   context (gui_context_t, below), reached through the g_ctx alias -- popups persist per context.
   s_popup_begin_count is per-frame scratch (rebuilt as begin/popup_end run) and stays a plain global. */
static u32           s_popup_begin_count;                 // current nesting depth (per frame)

/*----------------------------------------------------------------------------------------------
    Keyed state pool -- persistent per-id widget state.

    The single store a widget uses to keep a few bytes alive across frames, keyed by its id: a
    region's scroll offset, a tree node's open flag, a combo's popup state.  gui_state_get hands
    back a stable, zero-on-create pointer to `size` bytes for `id`; the GUI_STATE( T, id ) sugar
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

/* GUI_STATE_SLOTS / _MASK / _CAP are defined in gui_internal.h. */

/* gui_state_slot_t (one keyed-state-pool slot) is defined in gui_internal.h. */

/* ---- First slice of per-context retained state ----
   The keyed state pool and the frame clock that ages it are the first members of what will become
   gui_context_t.  They move together because the clock only has meaning relative to the pool it
   stamps -- and, more pointedly, the clock must advance per context, not per app-frame: a context
   not rebuilt on a given frame must not tick, or its live entries would read as cold and be
   reclaimed (losing scroll / open state) while it is merely hidden.  Window / popup / combo
   "appearing" detection keys off the same per-context clock for the same reason.  Bundling them now
   makes the eventual lift into gui_context_t a regrouping, not a rewrite.  Tier: per-context
   retained. */

/* gui_retained_t (id salt, frame clock, keyed state pool) is defined in gui_internal.h. */

/*----------------------------------------------------------------------------------------------
    gui_context_t -- the bound per-context retained state ("bind and use").

    A context is the emission session the code binds once and emits ALL its windows into; it owns the
    state that must persist between frames for that UI.  Every retained access in the module resolves
    through g_ctx via the aliases below -- s_retained (id salt + frame clock + keyed state pool),
    s_nav (the nav cursor location + menu mode), and the popup open-set -- so switching contexts is a
    single pointer assignment (ctx_bind): no copy, no backup/restore.

    Ambient state (one user: s_interaction, s_io) and frame scratch (s_build, the stacks, s_draw) are
    NOT per context -- they stay global and target whichever context is bound.  The window pool,
    viewports, popup set, and dock nodes ARE per context: members of gui_context_t reached through
    the aliases below.  The primary context (slot 0) owns the OS windows; secondary contexts share
    the same OS windows and render surfaces rather than owning separate ones.
----------------------------------------------------------------------------------------------*/

/* gui_context_t (the bound per-context retained state) is defined in gui_internal.h. */

/* Context pool.  Slot 0 is always the default context (bound at init, never freed).  Slots 1..N are
   secondary contexts allocated by gui_ctx_create.  g_ctx is the one indirection every retained
   access goes through; switching contexts is a single pointer assignment (ctx_bind).
   Each context carries a `listening` flag; only listening contexts receive hover/click/nav input. */

#define GUI_CTX_POOL_MAX  8       /* slot 0 = default + up to 7 secondary contexts */

/* Pool of context pointers.  Slot 0 = default context (heap-allocated at init, freed at shutdown).
   Slots 1..N are secondary contexts returned by ctx_create.  All slots use the same block layout. */
static gui_context_t* s_ctx_pool      [ GUI_CTX_POOL_MAX ];
static u32              s_ctx_pool_count;   /* live slot count; always >= 1 after init */

static gui_context_t* g_ctx = NULL;   /* bound context */


#define s_retained         ( g_ctx->retained )
#define s_nav              ( g_ctx->nav )
#define s_popups_open      ( g_ctx->popups_open )
#define s_popup_open_count ( g_ctx->popup_open_count )
#define s_windows          ( g_ctx->windows )
#define s_window_count     ( g_ctx->window_count )
#define s_window_scratch   ( g_ctx->window_scratch )
#define s_z_counter        ( g_ctx->z_counter )
#define s_dock_nodes       ( g_ctx->dock_nodes )
#define s_dock_node_count  ( g_ctx->dock_node_count )
#define s_dock_id_seq      ( g_ctx->dock_id_seq )

/* Single-malloc layout for one context block.  The header (gui_context_t) sits at offset 0;
   all pool arrays follow at ALIGN8 boundaries.  Caller sets `listening` and wires s_ctx_pool. */
static gui_context_t*
ctx_alloc_slot( const gui_ctx_config_t* c, u32 slots, i32 slot )
{
    #define ALIGN8( x ) ( ( ( x ) + 7u ) & ~7u )
    u32 sz_state = slots            * (u32)sizeof( gui_state_slot_t );
    u32 sz_pop   = c->popup_depth   * (u32)sizeof( gui_popup_t      );
    u32 sz_win   = c->max_windows   * (u32)sizeof( gui_window_t     );
    u32 sz_vp    = c->max_viewports * (u32)sizeof( gui_viewport_t   );
    u32 sz_dock  = c->max_dock_nodes* (u32)sizeof( gui_dock_node_t  );

    u32 off_state = ALIGN8( (u32)sizeof( gui_context_t ) );
    u32 off_pop   = ALIGN8( off_state + sz_state );
    u32 off_win   = ALIGN8( off_pop   + sz_pop   );
    u32 off_vp    = ALIGN8( off_win   + sz_win   );
    u32 off_dock  = ALIGN8( off_vp    + sz_vp    );
    u32 total     = ALIGN8( off_dock  + sz_dock  );
    #undef ALIGN8

    char* blk = (char*)malloc( total );
    if ( !blk ) return NULL;
    memset( blk, 0, total );

    gui_context_t* ctx      = (gui_context_t*)blk;
    ctx->retained.state       = (gui_state_slot_t*)( blk + off_state );
    ctx->retained.state_count = slots;
    ctx->retained.state_mask  = slots - 1;
    ctx->retained.id_salt     = (u32)slot * 0x9e3779b9u;
    ctx->popups_open          = (gui_popup_t*)   ( blk + off_pop  );
    ctx->popup_depth          = c->popup_depth;
    ctx->windows              = (gui_window_t*)  ( blk + off_win  );
    ctx->max_windows          = c->max_windows;
    ctx->viewports            = (gui_viewport_t*)( blk + off_vp   );
    ctx->max_viewports        = c->max_viewports;
    ctx->dock_nodes           = c->max_dock_nodes
                                ? (gui_dock_node_t*)( blk + off_dock ) : NULL;
    ctx->max_dock_nodes       = c->max_dock_nodes;
    ctx->_alloc               = blk;
    return ctx;
}

/* Allocate the default context (slot 0) with editor-profile defaults; called from gui_init. */
static void
ctx_pool_init( void )
{
    gui_ctx_config_t c = GUI_CTX_CONFIG_EDITOR;
    u32 slots = c.state_slots;
    u32 p = 1;
    while ( p < slots ) p <<= 1;
    slots = p;

    gui_context_t* ctx = ctx_alloc_slot( &c, slots, 0 );
    ORB_ASSERT( ctx != NULL );   /* no gui without a default context */
    ctx->listening = true;       /* default context listens to input */

    s_ctx_pool[ 0 ]  = ctx;
    s_ctx_pool_count = 1;
    g_ctx            = ctx;
}

/* Bind the active context; every alias above resolves into it from here on.  NULL rebinds the
   default.  This is the whole multi-context seam -- no state is copied. */
static void
ctx_bind( gui_context_t* ctx )
{
    g_ctx = ctx ? ctx : s_ctx_pool[ 0 ];
}

/* Resolve an app win_id to the primary context's viewport index.  OS windows and their viewport
   slots are owned by the primary context (slot 0) regardless of which context is currently bound;
   secondary contexts share the same OS windows and render surfaces rather than owning separate ones.
   Searches the primary context's viewport array for a live slot (one with GPU buffers) whose
   recorded win_id matches; falls back to 0 (main swapchain) if none found.
   Forward-declared in gui_internal.h; called by the mouse-input path in gui_input.c. */
static u32
viewport_index_for_window( i32 win_id )
{
    const gui_context_t* primary = s_ctx_pool[ 0 ];
    for ( u32 i = 0; i < primary->max_viewports; ++i )
    {
        const gui_viewport_t* vp = &primary->viewports[ i ];
        if ( rhi_handle_valid( vp->vb ) && vp->win_id == win_id )
            return i;
    }
    return 0;   /* no live slot matches -> main swapchain surface */
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

#define GUI_ID_STACK_DEPTH 32

static gui_id_t s_id_stack[ GUI_ID_STACK_DEPTH ];
static u32        s_id_sp;

/* id_seed, id_push, id_pop, id_hash, id_combine, gui_state_get, and GUI_STATE are defined
   in gui_ctx_id.c (included immediately after this file).  They reference s_id_stack/s_id_sp
   and s_retained (via g_ctx) which are defined here and visible in the unity build. */

/*----------------------------------------------------------------------------------------------
    rect_hit -- true when the mouse cursor (from s_io) is inside the given rect
----------------------------------------------------------------------------------------------*/

static bool
rect_hit( gui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/* rect_intersect (rect overlap) is a shared geometry helper defined in gui.c, ahead of the
   unity includes, so gui_emit_draw.c can use it for clip intersection too. */

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
nav_score_dir( gui_rect_t cur, gui_rect_t cand, gui_dir_t dir )
{
    f32 ccx = cur.x  + cur.w  * 0.5f, ccy = cur.y  + cur.h  * 0.5f;
    f32 ncx = cand.x + cand.w * 0.5f, ncy = cand.y + cand.h * 0.5f;
    f32 dx  = ncx - ccx, dy = ncy - ccy;

    f32 prim, secd;   /* primary = distance along the axis; secondary = perpendicular offset */
    switch ( dir )
    {
        case GUI_DIR_UP:    prim = -dy; secd = dx < 0 ? -dx : dx; break;
        case GUI_DIR_DOWN:  prim =  dy; secd = dx < 0 ? -dx : dx; break;
        case GUI_DIR_LEFT:  prim = -dx; secd = dy < 0 ? -dy : dy; break;
        case GUI_DIR_RIGHT: prim =  dx; secd = dy < 0 ? -dy : dy; break;
        default:              return NAV_SCORE_REJECT;
    }
    if ( prim <= 0.0f ) return NAV_SCORE_REJECT;   /* behind / abreast -- not in this direction */
    return prim + secd * 2.0f;                     /* weight misalignment so straight-ahead wins */
}

/*----------------------------------------------------------------------------------------------
    window_nominate_hover -- window_begin calls this with its rect + z + host viewport.  Keeps the
    front-most (highest z) window the cursor is over; promoted to hover_win next frame.

    The cursor lives in exactly one OS window/surface at a time (s_io.mouse_viewport, resolved from
    the win_id on mouse events).  A window on any other surface cannot be under the cursor regardless
    of where its rect sits in its own surface's coordinate space, so it is rejected before the rect
    test -- this is the "physical window is a parent hover" rule: the surface must match first, then
    the window rect within it.  Without this, the polled cursor position (client coords of whatever
    window the mouse is in) would hit-test against identically-placed windows on every surface.
----------------------------------------------------------------------------------------------*/

static void
window_nominate_hover( gui_id_t id, gui_rect_t r, u32 z, u32 viewport )
{
    /* Deaf context: not listening for input this frame, skip hover nomination. */
    if ( !g_ctx->listening )
        return;

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
    Hardware cursor

    gui owns the OS cursor shape only while it owns the mouse (hover_win set, or a widget drag in
    flight) -- the same want_capture_mouse fence non-UI code gates on.  A widget requests a shape for
    the frame with set_mouse_cursor; cursor_flush (called at frame_begin) pushes the PREVIOUS frame's
    request to the OS window the cursor was over, deferred one frame exactly like hover_win.

    app()->window_set_cursor is sticky (it latches win->cursor), so the request is flushed only on a
    change, and on the frame gui releases the mouse it pushes ARROW once so a stale I-beam / resize
    shape does not linger on that window -- after which the cursor is left to the host (game scene).
----------------------------------------------------------------------------------------------*/

/* Request a hardware cursor shape for this frame.  Last writer wins (one hover per frame). */
static void set_mouse_cursor( app_cursor_t c ) { s_interaction.mouse_cursor = c; }

/* Flush the requested cursor to the OS window under the pointer.  Reads last frame's request +
   hover state (called before interaction_frame_reset promotes the new frame's hover).  Dedupes on
   (window, shape) so an unchanged cursor is not re-posted every frame. */
static void
cursor_flush( void )
{
    static i32          s_flushed_win   = -1;                 /* last window we pushed a shape to    */
    static app_cursor_t s_flushed_cur   = APP_CURSOR_ARROW;   /* last shape pushed there             */

    bool want = ( s_interaction.hover_win != GUI_ID_NONE )   /* gui owns the mouse: same fence  */
             || ( s_interaction.active_id != GUI_ID_NONE );  /* as want_capture_mouse             */

    if ( want )
    {
        /* The OS window the cursor is in: viewport slot index -> its app win_id. */
        i32 win = 0;
        if ( s_io.mouse_viewport < g_ctx->viewport_count )
            win = g_ctx->viewports[ s_io.mouse_viewport ].win_id;

        if ( win != s_flushed_win || s_interaction.mouse_cursor != s_flushed_cur )
        {
            app()->window_set_cursor( win, s_interaction.mouse_cursor );
            s_flushed_win = win;
            s_flushed_cur = s_interaction.mouse_cursor;
        }
    }
    else if ( s_flushed_win >= 0 )
    {
        /* Release edge: gui no longer owns the mouse -- clear our shape once, then leave the
           cursor to the host so game / scene code can set its own. */
        if ( s_flushed_cur != APP_CURSOR_ARROW )
            app()->window_set_cursor( s_flushed_win, APP_CURSOR_ARROW );
        s_flushed_win = -1;
        s_flushed_cur = APP_CURSOR_ARROW;
    }
}

/*----------------------------------------------------------------------------------------------
    ctx_new_frame -- reset per-frame hover state; call at the start of each frame
----------------------------------------------------------------------------------------------*/

/* Reset the per-frame GLOBAL interaction snapshot.  Called ONCE per application frame from
   gui_frame_begin before any ctx_begin -- shared across all contexts (there is one mouse,
   one keyboard, one hover window).  Must NOT be called from ctx_new_frame (which runs per
   context) or the second ctx_begin would clobber hover_win/active_id set by the first. */
static void
interaction_frame_reset( void )
{
    /* Snapshot the active item before this frame mutates it -- the previous-frame baseline the
       is_item_activated / is_item_deactivated edge readers compare against (gui_ctx_io.c). */
    s_interaction.active_id_prev = s_interaction.active_id;

    /* Snapshot focused_id so frame_end can detect whether focus moved this frame. */
    s_interaction.focused_id_at_frame_start = s_interaction.focused_id;

    /* Widget hover is rebuilt from hit tests during emission; clear it now. */
    s_interaction.hover_id = GUI_ID_NONE;

    /* Promote the window the cursor was over last frame, then start a fresh nomination.
       hover_win lags the cursor by one frame -- all contexts contribute nominations during
       emission; the front-most (highest z) winner is promoted at the NEXT frame_begin. */
    s_interaction.hover_win        = s_interaction.next_hover_win;
    s_interaction.next_hover_win   = GUI_ID_NONE;
    s_interaction.next_hover_win_z = 0;

    /* Release active_id once its initiating button is up.  Keep it alive on the release-edge
       frame (mouse_released) so widgets can still see the press+release pair this frame. */
    u8 ab = s_interaction.active_button;
    if ( !s_io.mouse_down[ ab ] && !s_io.mouse_released[ ab ] )
    {
        s_interaction.active_id     = GUI_ID_NONE;
        s_interaction.active_button = 0;
    }

    /* Drop keyboard focus on any press; the widget under the cursor re-claims it immediately
       (input_text sets focused_id from hover_id + press this same frame). */
    if ( s_io.mouse_pressed[ 0 ] )
        s_interaction.focused_id = GUI_ID_NONE;

    /* Fresh cursor request for the new frame -- defaults to the arrow until a widget asks otherwise. */
    s_interaction.mouse_cursor = APP_CURSOR_ARROW;
}

/* Mark the currently focused item as edited this frame.  Called by input_field_edit whenever the
   buffer changes.  Accumulates in focused_id_edited (never cleared while focus stays); frame_end
   snapshots it into focus_ended_edited when focus departs so is_item_deactivated_after_edit can
   read it for one frame after the focus moves. */
static void mark_item_edited( void ) { s_interaction.focused_id_edited = true; }

/* Per-context frame reset: rebuilds the frame-scratch and per-context retained state.
   Called by ctx_begin for every context -- does NOT touch the global s_interaction fields
   (those are reset once per app frame by interaction_frame_reset in frame_begin). */
static void
ctx_new_frame( void )
{
    /* Last-item introspection resets to "no item": a query before any widget this frame (or in
       a frame that emits none) reports false rather than reading a stale rect / status. */
    s_build.last_item_id     = GUI_ID_NONE;
    s_build.last_item_rect   = ( gui_rect_t ){ 0 };
    s_build.last_item_status = ( widget_state_t ){ 0 };

    /* Fresh layout stack each frame; no region is open until a window_begin/child_begin.
       The interaction clip starts at the full display, and the wheel is unclaimed. */
    s_layout_sp           = 0;
    s_id_sp               = 0;       /* fresh id-scope stack; regions/push_id reseed it */
    s_build.wheel_used    = false;
    s_build.cur_viewport  = 0;       /* ambient viewport resets to primary each frame */

    /* Popup nesting depth is rebuilt as popup_begin / popup_end run; the open set persists. */
    s_popup_begin_count = 0;

    /* Combo body coordination is per-frame and re-set by begin/combo_end; clear as a safety net. */
    s_build.combo_open         = false;
    s_build.combo_item_clicked = false;

    /* Fresh item-flag state each frame: empty stack, no next-item override, nothing disabled. */
    s_item_flag_sp         = 0;
    s_build.item_flags     = GUI_ITEM_NONE;
    s_build.next_set       = GUI_ITEM_NONE;
    s_build.next_val       = GUI_ITEM_NONE;
    s_build.cur_item_flags = GUI_ITEM_NONE;

    /* Fresh style stacks each frame: working set re-seeded from the theme, stacks + next cleared. */
    style_new_frame();
    s_build.clip_rect = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    ++s_retained.frame;
}

/* Public IO accessors (gui_want_capture_*, gui_is_key_*, gui_is_mouse_*, gui_get_*)
   are defined in gui_ctx_io.c, included immediately after gui_ctx_id.c.
   They read s_interaction, s_nav, s_popup_open_count, s_build, s_io, and rect_hit --
   all visible in the unity build at that point. */

/*----------------------------------------------------------------------------------------------
    Viewport display-size accessors.

    A viewport's stored disp_w / disp_h is the authoritative drawable size once a surface has
    been opened.  Before the first open (or after a close), it is 0 and the per-frame s_io
    snapshot -- populated from the primary OS window -- is the best available fallback.  Every
    window-placement and clip-rect computation uses one of these two helpers rather than spelling
    the ternary out inline.
----------------------------------------------------------------------------------------------*/

static f32 vp_w( const gui_viewport_t* vp ) { return vp->disp_w > 0 ? (f32)vp->disp_w : (f32)s_io.display_w; }
static f32 vp_h( const gui_viewport_t* vp ) { return vp->disp_h > 0 ? (f32)vp->disp_h : (f32)s_io.display_h; }

// clang-format on
/*============================================================================================*/
