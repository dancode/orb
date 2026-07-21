/*==============================================================================================

    runtime_service/gui/core/gui_ctx.c -- Immediate-mode context state and per-frame drivers.

    Declares all persistent ambient and frame-scratch state (s_interaction, s_build, gui_nav_state_t,
    layout_frame_t, gui_context_t) and drives the per-frame lifecycle via ctx_new_frame.  Also owns
    the public services that operate directly on the context pool and belong to no tier of their
    own: memory stats (gui_mem_stats/gui_print_mem_stats) and the multi-context lifecycle
    (gui_ctx_create/destroy/bind/set_listening).

    ID hashing and the keyed state pool (id_hash, id_combine, id_seed/push/pop, gui_state_get,
    GUI_STATE) are in core/gui_id.c + core/gui_state.c, included just after this file.

    Public IO accessors (gui_want_capture_*, gui_is_key_*, gui_is_mouse_*, etc.) are in
    user/gui_query.c, included in the user/ tier below.

    Included by gui.c after core/gui_io.c so s_io is in scope.

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   /* gui_nav_state_t, layout_frame_t, gui_popup_t,
                                                        gui_retained_t, gui_viewport_t, gui_context_t */
// clang-format off

/*==============================================================================================
    State
==============================================================================================*/

/* s_build.win.rec points at a live gui_window_t pool record (window types in gui_internal.h; the
   pool is reached through g_ctx) so window_end can write scroll / content extent back into it. */

/* Ambient interaction state -- the one live hover / active / focus, persisting across frames.  One
   pointer, one keyboard, one mouse, so none of it is per-viewport or per-context: a single global
   shared by every context, into which listening contexts nominate hover / active during their emit.
   Tier: ambient singular (see ARCHITECTURE.md sec 1, state tiers).  The TYPE lives in
   gui_internal.h (gui_interaction_t, extern'd for the carved units -- inc 10); this file stays
   the owner: it defines, resets, and turns the record over.  Field notes worth keeping close:

   - Auto-repeat (repeat_t / repeat_on, GUI_ITEM_BUTTON_REPEAT): only one widget is active at a
     time, so a single timer suffices; both reset on the press frame.
   - Window occlusion is one frame deferred: window_begin nominates into next_hover_win,
     ctx_new_frame promotes it to hover_win; only the hover window hit-tests its widgets.
   - mouse_cursor: last writer wins (exactly one widget hovers; resize bands suppress widget
     hover); cursor_flush pushes it to the OS one frame later; reset each frame.
   - Focus departure (focused_id_at_frame_start / focus_ended_*): the departing widget's id +
     edit history latch for one frame so is_item_deactivated_after_edit can read them. */

gui_interaction_t s_interaction;

/* True only while a volatile-widget callback is replayed standalone on an idle frame; set/cleared
   by gui_replay_scope_enter/_exit (widgets/gui_volatile.c).  Ambient frame-phase state, same tier as
   hover_id/active_id: item_state (interact/gui_item.c) reads it inline to short-circuit before any
   hit-test or write to s_interaction/s_build -- a replay renders against the hover/active/focus the
   last real frame established and can never acquire state or see a fresh click, since interaction is
   resolved only on real frames. */
bool s_replay_mode;

/* Frame-build scratch -- the "where am I emitting right now" context, rebuilt every frame as the
   widget tree is walked.  Nothing here survives begin_frame: it is set and repopulated by the
   window_begin / child_begin / widget calls, never read across a frame boundary.  Because contexts
   build sequentially on one thread, this stays a single global builder reused by each context in
   turn rather than per-context state.  Tier: frame scratch. */

/* The TYPE lives in gui_internal.h (gui_build_t, extern'd for the carved units -- inc 10);
   this file stays the owner (defines and resets it).  Notes worth keeping by the definition:

   - win: everything window_begin stamps and window_end consumes, one record (gui_win_ctx_t) so
     the popup seam saves/restores it with a single assignment; the fields after it are
     frame-global channels that must SURVIVE that seam.
   - Layout pen + scroll region state live on the layout-frame stack below; the window is just
     the root frame.
   - item_flags / next_set / next_val: the push-model behavior set; the value resolved for the
     widget currently emitting is latched into s_scope.flags by item_flags_resolve.
   - combo_open / combo_item_clicked: combo dropdown coordination (gui_combo.c) -- a click in
     the body dismisses the combo with no caller code; reset each frame as a safety net.
   - Nav state is NOT part of this scratch: it lives in g_ctx->nav so the builder stays small. */

gui_build_t s_build;

/* The interaction scope -- the declared contract between composition and behavior; the full
   contract lives with the type (gui_scope_t, gui_internal.h).  Stamped directly by composition
   at its seams, read by behavior, saved/restored wholesale at the popup seam -- a field added to
   the type only needs its per-frame reset in ctx_new_frame below.  Tier: frame scratch, same
   lifetime as s_build. */

gui_scope_t s_scope;

#ifdef GUI_DEBUG_OVERLAY
/* The debug overlay (gui_debug_overlay.c) lives in the render backend unit and tags each captured rect
   with the ambient build viewport.  s_build is private to this unit, so the overlay reads it
   across the unit seam through this accessor (declared in gui_backend.h, Debug builds only). */
u32 gui_dbg_build_viewport( void ) { return s_build.win.viewport; }
#endif

/*==============================================================================================
    Keyboard navigation state (g_ctx->nav)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather than
    the mouse -- plus the menu-bar state machine layered on it.  gui_nav_state_t is defined in
    gui_internal.h; the instance is the g_ctx->nav member of the bound context, reached through g_ctx, so
    each context keeps its own cursor.  Movement is structural, not spatial: every item of the
    scoped window records itself into the nav item list as it emits (with the region + line the
    layout engine stamped when it placed it), and the next nav_new_frame resolves a move as index
    math over that list -- one frame deferred, exactly as hover_win lags the cursor.  gui_nav.c
    drives it; nav_item_register (interact/gui_item.c) is the per-item seam.
==============================================================================================*/

/*==============================================================================================
    Item-flag stack

    push_item_flag saves the current merged value here and pop_item_flag restores it, so a push
    nests cleanly regardless of which bits it touched.  An over-deep push aliases the top slot and
    is still counted truthfully, mirroring the id / layout stacks, so push/pop stay paired.
==============================================================================================*/

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
   result for item_state / widgets to read, and sets the draw alpha so a disabled item dims with
   no per-widget code.  Called once per item from cell_next_w (the universal emit seam). */
gui_item_flags_t
item_flags_resolve( void )
{
    gui_item_flags_t f = ( s_build.item_flags & ~s_build.next_set ) | ( s_build.next_val & s_build.next_set );
    s_build.next_set = 0;
    s_build.next_val = 0;
    s_scope.flags = f;

    /* Same seam for the style stacks: promote any next_style_* override into the active per-item
       layer so it applies for this widget's whole draw, then clears for the following one. */
    style_item_commit();

    draw_set_alpha( ( f & GUI_ITEM_DISABLED ) ? GUI_DISABLED_ALPHA : 1.0f );
    /* Default this widget's rects to the control-frame radius (base + push/pop + next-* override).
       A widget that draws a grab (slider knob, scrollbar) or a squared-off mark (check, bullet)
       overrides draw_set_rounding locally for that sub-element. */
    draw_set_rounding( ROUND_WIDGET );
    return f;
}

/* Clear the per-item state before chrome runs.  Window/child borders, scrollbars, titlebars, and
   the collapse arrow are not items -- they never go through cell_next_w, so without this they
   would inherit whatever the last body widget latched (a disabled trailing widget would dim the
   border and deaden the scrollbar).  Called at the chrome seams (begin/window_end, child_begin,
   layout_pop_region) so chrome always interacts undisabled and paints opaque. */
void
item_flags_chrome_reset( void )
{
    s_scope.flags  = GUI_ITEM_NONE;
    s_scope.nav.placed = false;   /* chrome is not an item to keyboard nav either: whatever
                                          interacts past this seam lists in the F6 chrome lane */
    draw_set_alpha( 1.0f );
    style_chrome_reset();   /* drop lingering next_style_* overrides; keep the push/pop stack */
    /* Chrome (window / child / dock backgrounds, title bars, borders) defaults to the window radius,
       read after the item override is cleared so a trailing widget's next-* radius cannot leak in. */
    draw_set_rounding( style_var( GUI_VAR_WIN_ROUNDING ) );
}

/*==============================================================================================
    Layout-frame stack

    Every scrollable region (a window body or a child_begin box) pushes one frame; the top frame
    owns the layout pen and the content column leaf widgets emit into.  The rest of the frame is the
    resolve context layout_pop_region needs to measure content and draw the region's scrollbars.
    Storage is just the fixed array, so a deep nesting costs nothing beyond these slots.
==============================================================================================*/

layout_frame_t s_layout_stack[ GUI_LAYOUT_DEPTH ];
u32            s_layout_sp;   // active frame count; top = s_layout_sp - 1

/* Top layout frame.  Valid between a window_begin/child_begin and its matching end.  When the
   stack is empty (a caller emitted a widget into a collapsed window despite the false return)
   slot 0 is returned instead of indexing out of bounds -- the stray widget draws into whatever
   the last frame's root region left there rather than crashing.  The read index is also clamped
   to the top slot so an over-deep nesting (capped in layout_push_region) never reads past the
   array. */

layout_frame_t*
lf( void )
{
    u32 i = s_layout_sp ? s_layout_sp - 1 : 0;
    if ( i >= GUI_LAYOUT_DEPTH ) i = GUI_LAYOUT_DEPTH - 1;
    return &s_layout_stack[ i ];
}

/*==============================================================================================
    Popup stack

    The open popups form a stack, parent -> child: index 0 is the top-level popup, each deeper index
    one opened inside the one above.  It is the source of truth for open / close, nesting, and the
    click-outside policy; the popups themselves render as ordinary windows on a reserved high z-band
    (gui_popup.c).  Two counters, split by lifetime:

      g_ctx->popup.open_count  -- persists across frames (per context); the live open set is [0, count).
      s_popup_begin_count -- rebuilt each frame; the popup nesting depth while emitting.  popup_open
                             writes a request at this depth; popup_begin matches its id against it.

    A popup is a top-level overlay begun while its parent window is still open, yet it must lay out,
    clip, and paint independent of that parent.  gui_overlay_save_t (gui_internal.h) holds the parent
    context -- whole-struct copies of the window context, interaction scope, and draw scope, plus the
    parent's top layout frame -- so popup_end restores the parent verbatim; the stack counters
    balance through the normal push/pop, so no slot is reused or lost.
==============================================================================================*/

/* The open set (g_ctx->popup.open) and its count (g_ctx->popup.open_count) are per-context members reached
   through g_ctx; s_popup_begin_count is per-frame scratch and stays a plain global. */
u32           s_popup_begin_count;   // current popup nesting depth (rebuilt per frame)

/*==============================================================================================
    Keyed state pool -- persistent per-id widget state.

    The store a widget uses to keep a few bytes alive across frames, keyed by its id (a region's
    scroll offset, a tree node's open flag, a combo's popup state).  It is a member of the bound
    context's retained store (gui_retained_t); gui_state_get and the open-addressing / tombstone
    contract live in core/gui_state.c, next to the id system that keys it (core/gui_id.c).
==============================================================================================*/

/*==============================================================================================
    gui_context_t -- the bound per-context retained state ("bind and use").

    A context is the emission session the code binds once and emits ALL its windows into; it owns the
    state that must persist between frames for that UI.  Every retained access in the module spells
    the bound context out -- g_ctx->retained (id salt + frame clock + keyed state pool), g_ctx->nav
    (the nav cursor location + menu mode), the popup open-set, the window / viewport / dock pools --
    so per-context state is visibly distinct from the global s_* scratch at every call site, and
    switching contexts is a single pointer assignment (ctx_bind): no copy, no backup/restore.

    The frame clock (g_ctx->retained.frame) advances per context, at ctx_begin, NOT per app-frame: a
    context not rebuilt on a given frame must not tick, or its live keyed-state entries would read as
    cold and be reclaimed (losing scroll / open state) while it is merely hidden.  Window / popup /
    combo "appearing" detection keys off the same per-context clock.

    Ambient state (one user: s_interaction, s_io) and frame scratch (s_build, the stacks, s_draw) are
    NOT per context -- they stay global and target whichever context is bound.  The primary context
    (slot 0) owns the OS windows; secondary contexts share the same OS windows and render surfaces
    rather than owning separate ones.
==============================================================================================*/

/* Context pool.  Slot 0 is the default context (heap-allocated and bound at init, freed only at
   shutdown -- never torn down by ctx_destroy at runtime); slots 1..N are secondary contexts from
   ctx_create, all sharing the same single-malloc block layout.  Each context's `listening` flag
   gates whether it receives hover / click / nav input. */

#define GUI_CTX_POOL_MAX  8       /* slot 0 = default + up to 7 secondary contexts */

static gui_context_t* s_ctx_pool[ GUI_CTX_POOL_MAX ];
static u32            s_ctx_pool_count;   /* live slot count; always >= 1 after init */

gui_context_t* g_ctx = NULL;   /* bound context (extern'd in gui_internal.h for the carved units) */

/* Single-malloc layout for one context block.  The header (gui_context_t) sits at offset 0;
   all pool arrays follow at ALIGN8 boundaries.  Caller sets `listening` and wires s_ctx_pool. */
static gui_context_t*
ctx_alloc_slot( const gui_ctx_config_t* c, u32 slots, i32 slot )
{
    /* Keyed-state class partition: tiny gets the full state_slots (the hot renters), small 3/4
       of it -- counts are free of the power-of-two rule (multiply-shift bucketing, gui_state.c). */
    u32 slots_small = ( slots / 4u ) * 3u;
    if ( slots_small == 0 ) slots_small = slots;

    #define ALIGN8( x ) ( ( ( x ) + 7u ) & ~7u )
    u32 sz_tiny  = slots               * (u32)sizeof( gui_state_tiny_slot_t );
    u32 sz_state = slots_small         * (u32)sizeof( gui_state_slot_t     );
    u32 sz_big   = GUI_STATE_BIG_SLOTS * (u32)sizeof( gui_state_big_slot_t );
    u32 sz_pop   = c->popup_depth      * (u32)sizeof( gui_popup_t          );
    u32 sz_win   = c->max_windows      * (u32)sizeof( gui_window_t         );
    u32 sz_vp    = c->max_viewports    * (u32)sizeof( gui_viewport_t       );
    u32 sz_dock  = c->max_dock_nodes   * (u32)sizeof( gui_dock_node_t      );

    u32 off_tiny  = ALIGN8( (u32)sizeof( gui_context_t ) );
    u32 off_state = ALIGN8( off_tiny  + sz_tiny  );
    u32 off_big   = ALIGN8( off_state + sz_state );
    u32 off_pop   = ALIGN8( off_big   + sz_big   );
    u32 off_win   = ALIGN8( off_pop   + sz_pop   );
    u32 off_vp    = ALIGN8( off_win   + sz_win   );
    u32 off_dock  = ALIGN8( off_vp    + sz_vp    );
    u32 total     = ALIGN8( off_dock  + sz_dock  );
    #undef ALIGN8

    char* blk = (char*)malloc( total );
    if ( !blk ) return NULL;
    memset( blk, 0, total );

    gui_context_t* ctx      = (gui_context_t*)blk;
    ctx->retained.state_tiny  = (gui_state_tiny_slot_t*)( blk + off_tiny );
    ctx->retained.tiny_count  = slots;
    ctx->retained.state       = (gui_state_slot_t*)( blk + off_state );
    ctx->retained.state_count = slots_small;
    ctx->retained.state_big   = (gui_state_big_slot_t*)( blk + off_big );
    ctx->retained.big_count   = GUI_STATE_BIG_SLOTS;
    ctx->retained.id_salt     = (u32)slot * 0x9e3779b9u;
    ctx->popup.open           = (gui_popup_t*)   ( blk + off_pop  );
    ctx->popup.depth          = c->popup_depth;
    ctx->win.pool             = (gui_window_t*)  ( blk + off_win  );
    ctx->win.max              = c->max_windows;
    ctx->vp.pool              = (gui_viewport_t*)( blk + off_vp   );
    ctx->vp.max               = c->max_viewports;
    ctx->dock.pool            = c->max_dock_nodes
                                ? (gui_dock_node_t*)( blk + off_dock ) : NULL;
    ctx->dock.max             = c->max_dock_nodes;
    ctx->_alloc               = blk;
    ctx->_alloc_size          = total;
    return ctx;
}

/* Allocate the default context (slot 0) at the internal maxima -- the compile-time caps the
   library is built with (GUI_STRESS_TEST scales these); no preset overrides them. */
static void
ctx_pool_init( void )
{
    gui_ctx_config_t c = {
        .max_windows    = GUI_DEFAULT_MAX_WINDOWS,
        .state_slots    = GUI_DEFAULT_STATE_SLOTS,
        .popup_depth    = GUI_DEFAULT_POPUP_DEPTH,
        .max_viewports  = GUI_MAX_VIEWPORTS,
        .max_dock_nodes = GUI_DEFAULT_DOCK_NODES,
    };
    gui_context_t* ctx = ctx_alloc_slot( &c, c.state_slots, 0 );
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
   Forward-declared in gui_internal.h; called by the mouse-input path in core/gui_io.c. */
static u32
viewport_index_for_window( i32 win_id )
{
    const gui_context_t* primary = s_ctx_pool[ 0 ];
    for ( u32 i = 0; i < primary->vp.max; ++i )
    {
        const gui_viewport_t* vp = &primary->vp.pool[ i ];
        if ( rhi_handle_valid( vp->vb ) && vp->win_id == win_id )
            return i;
    }
    return 0;   /* no live slot matches -> main swapchain surface */
}

/*==============================================================================================
    Id-scope stack

    The top of this stack is the seed every widget id combines against, so identical labels in
    different scopes never collide.  Regions seed it automatically (layout_push_region pushes the
    region id, layout_pop_region restores), and push_id / pop_id add temporary levels for repeated
    widgets inside one region (e.g. list rows keyed by index).  Reset to empty each frame.

    Over-deep pushes alias the top slot rather than writing past the array, and id_seed clamps its
    read the same way -- mirroring the layout stack, so deep nesting degrades instead of crashing.
==============================================================================================*/

#define GUI_ID_STACK_DEPTH 32

static gui_id_t s_id_stack[ GUI_ID_STACK_DEPTH ];
u32        s_id_sp;

/* id_seed / id_push / id_pop / id_hash / id_combine and the keyed-state pool (gui_state_get,
   GUI_STATE) are in core/gui_id.c + core/gui_state.c, included just after this file. they operate on s_id_stack / s_id_sp
   and g_ctx->retained (via g_ctx) defined here. */

/*==============================================================================================
    rect_hit -- true when the mouse cursor (from s_io) is inside the given rect
==============================================================================================*/

bool
rect_hit( gui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/* rect_intersect (rect overlap) is a shared geometry helper defined in gui.c, ahead of the
   unity includes, so gui_emit_draw.c can use it for clip intersection too. */

/* Directional moves are resolved structurally over the nav item list (gui_nav.c), not scored
   over rects -- the geometric scorer that lived here (nav_score_dir) is gone with it. */

/*==============================================================================================
    Hardware cursor

    gui owns the OS cursor shape only while it owns the mouse (hover_win set, or a widget drag in
    flight) -- the same want_capture_mouse fence non-UI code gates on.  A widget requests a shape for
    the frame with cursor_set; cursor_flush (called at frame_begin) pushes the PREVIOUS frame's
    request to the OS window the cursor was over, deferred one frame exactly like hover_win.

    app()->window_set_cursor is sticky (it latches win->cursor), so the request is flushed only on a
    change, and on the frame gui releases the mouse it pushes ARROW once so a stale I-beam / resize
    shape does not linger on that window -- after which the cursor is left to the host (game scene).
==============================================================================================*/

/* Request a hardware cursor shape for this frame.  Last writer wins (one hover per frame). */
void cursor_set( app_cursor_t c ) { s_interaction.mouse_cursor = c; }

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
        if ( s_io.mouse_viewport < g_ctx->vp.count )
            win = g_ctx->vp.pool[ s_io.mouse_viewport ].win_id;

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

/*==============================================================================================
    Exclusive input mode (focus scope) -- the game-menu model of UI focus.

    A GUI_WIN_MODAL window is an exclusive input MODE, the immediate-mode analogue of a game's
    menu screen: while it is up it owns interaction (the hover fence, window_modal_apply) AND
    keyboard focus.  Two rules make focus behave like a menu selection rather than a desktop
    caret:

      confine  -- only the mode's own widgets may TAKE focus (focus_allowed, interact/gui_item.c);
                  no background window can steal it.
      hold     -- focus is STICKY within the mode: it never falls to nothing.  Focus can drop two
                  ways and the mode blocks both -- a press on non-focusable dead space
                  (interaction_frame_reset below) and a widget releasing its own capture on Enter /
                  Escape (item_focus_release, interact/gui_item.c).  You cannot "select nothing"
                  inside a menu; focus only MOVES when another focusable widget in the mode claims
                  it (a direct focused_id overwrite, not a release), so the console input keeps its
                  caret across dead-space clicks AND across every command it submits.

    Both key off modal.win_id + seen_frame, exactly like the hover fence -- one exclusive-mode
    fact, read three ways.  FUTURE: a stack of modes (nested dialogs) would push/pop this the way
    the popup layer already stacks; today there is one level, which the console needs.
==============================================================================================*/

/* An exclusive input mode is live -- a GUI_WIN_MODAL window emitted this frame or last (the
   console re-stamps modal.seen_frame at its window_begin every frame it is open, so the fence
   never lapses while it is up and lapses one frame after it closes). */
static bool
gui_modal_scope_live( void )
{
    u32 f = g_ctx->retained.frame;
    return g_ctx->modal.win_id != 0u &&
           ( g_ctx->modal.seen_frame == f || g_ctx->modal.seen_frame + 1u == f );
}

/* True when the live exclusive mode owns `id` -- the focused widget belongs to the modal window.
   The frame-begin focus-clear consults this to keep the mode's focus sticky. */
static bool
focus_scope_holds( gui_id_t id )
{
    return id != GUI_ID_NONE && gui_modal_scope_live() &&
           s_interaction.focused_win == g_ctx->modal.win_id;
}

/*==============================================================================================
    ctx_new_frame -- reset per-frame hover state; call at the start of each frame
==============================================================================================*/

/* Reset the per-frame GLOBAL interaction snapshot.  Called ONCE per application frame from
   gui_frame_begin before any ctx_begin -- shared across all contexts (there is one mouse,
   one keyboard, one hover window).  Must NOT be called from ctx_new_frame (which runs per
   context) or the second ctx_begin would clobber hover_win/active_id set by the first. */
static void
interaction_frame_reset( void )
{
    /* Snapshot the active item before this frame mutates it -- the previous-frame baseline the
       is_item_activated / is_item_deactivated edge readers compare against (user/gui_query.c). */
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
       (input_text sets focused_id from hover_id + press this same frame).  EXCEPTION -- an
       exclusive input mode (a GUI_WIN_MODAL window; the dev console) holds its focus the way a
       game menu holds a selection: you cannot "select nothing" inside it.  While the mode owns the
       focused widget, a press on non-focusable dead space keeps the focus instead of clearing it;
       a press on another focusable widget in the mode still moves focus (that widget overwrites
       focused_id in item_state).  This is what lets the console input keep its caret while the
       user sweeps a scrollback text selection -- the mode stays "on the input". */
    if ( s_io.mouse_pressed[ 0 ] && !focus_scope_holds( s_interaction.focused_id ) )
        s_interaction.focused_id = GUI_ID_NONE;

    /* Cleared each frame; the focused text field re-asserts it during its emit (input_field_edit). */
    s_interaction.focus_has_selection = false;

    /* Fresh cursor request for the new frame -- defaults to the arrow until a widget asks otherwise. */
    s_interaction.mouse_cursor = APP_CURSOR_ARROW;
}

/* Mark the currently focused item as edited this frame.  Called by input_field_edit whenever the
   buffer changes.  Accumulates in focused_id_edited (never cleared while focus stays); frame_end
   snapshots it into focus_ended_edited when focus departs so is_item_deactivated_after_edit can
   read it for one frame after the focus moves. */
void item_mark_edited( void ) { s_interaction.focused_id_edited = true; }

/* Per-context frame reset: rebuilds the frame-scratch and per-context retained state.
   Called by ctx_begin for every context -- does NOT touch the global s_interaction fields
   (those are reset once per app frame by interaction_frame_reset in frame_begin). */
static void
ctx_new_frame( void )
{
    /* Last-item introspection resets to "no item": a query before any widget this frame (or in
       a frame that emits none) reports false rather than reading a stale rect / status. */
    s_scope.last_id     = GUI_ID_NONE;
    s_scope.last_rect   = ( gui_rect_t ){ 0 };
    s_scope.last_status = ( gui_item_state_t ){ 0 };

    /* Fresh layout stack each frame; no region is open until a window_begin/child_begin.
       The interaction clip starts at the full display, and the wheel is unclaimed. */
    s_layout_sp           = 0;
    s_id_sp               = 0;       /* fresh id-scope stack; regions/push_id reseed it */
    s_build.wheel_used    = false;
    s_build.win.viewport  = 0;       /* ambient viewport resets to primary each frame */

    /* Fresh nav-stamp dispensers; nothing is placed until a layout cell is handed out. */
    s_build.nav_region_seq  = 0;
    s_build.nav_line_seq    = 0;
    s_scope.nav.placed = false;
    s_scope.nav.skip        = false;

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
    s_scope.flags = GUI_ITEM_NONE;

    /* Fresh style stacks each frame: working set re-seeded from the theme, stacks + next cleared. */
    style_new_frame();
    s_scope.clip = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    ++g_ctx->retained.frame;
}

/* Public IO accessors (gui_want_capture_*, gui_is_key_*, gui_is_mouse_*, gui_get_*)
   are defined in user/gui_query.c, included in the user/ tier below.
   They read s_interaction, g_ctx->nav, g_ctx->popup.open_count, s_build, s_io, and rect_hit --
   all visible in the unity build at that point. */

/*==============================================================================================
    Viewport display-size accessors.

    A viewport's stored disp_w / disp_h is the authoritative drawable size once a surface has
    been opened.  Before the first open (or after a close), it is 0 and the per-frame s_io
    snapshot -- populated from the primary OS window -- is the best available fallback.  Every
    window-placement and clip-rect computation uses one of these two helpers rather than spelling
    the ternary out inline.
==============================================================================================*/

f32 vp_w( const gui_viewport_t* vp ) { return vp->disp_w > 0 ? (f32)vp->disp_w : (f32)s_io.display_w; }
f32 vp_h( const gui_viewport_t* vp ) { return vp->disp_h > 0 ? (f32)vp->disp_h : (f32)s_io.display_h; }

/*==============================================================================================
    Memory Stats

    A full accounting of the gui system's resident footprint, split by where it lives (GPU device
    memory / fixed CPU .bss / per-context CPU heap).  The backend fills its own buckets through
    gui_backend_memory (GPU buffers + the fixed backend buffers); this frontend owns the context
    pool, so it counts the live GPU surfaces to scale the geometry buffers and sums the per-context
    malloc blocks for the heap total.  See gui_mem_stats_t (gui.h) for the bucket meanings.
==============================================================================================*/

/* Frontend statics total, defined in gui_ui_mem.c -- the LAST constituent include of this unity
   TU, so it can sizeof statics declared after this file.  Forward-declared here (same TU). */
u32 gui_ui_memory( void );

gui_mem_stats_t
gui_mem_stats( void )
{
    /* Count live GPU surfaces across every context (a viewport is live once it owns geometry
       buffers) so the backend can scale the per-surface VB/IB by the true surface count -- the
       old report assumed a single surface and undercounted every floater / secondary window. */
    u32 live_viewports = 0;
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        gui_context_t* ctx = s_ctx_pool[ i ];
        if ( !ctx ) continue;
        for ( u32 v = 0; v < ctx->vp.max; ++v )
            if ( rhi_handle_valid( ctx->vp.pool[ v ].vb ) )
                ++live_viewports;
    }

    /* Backend fills GPU + CPU .bss; frontend adds its own unit's statics, the CPU-heap context
       blocks, and the totals. */
    gui_mem_stats_t s = gui_backend_memory( live_viewports );

    s.cpu_frontend_bytes = gui_ui_memory();
    s.cpu_static_total  += s.cpu_frontend_bytes;

    /* CPU heap: one malloc block per live context (recorded at allocation as _alloc_size). */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        gui_context_t* ctx = s_ctx_pool[ i ];
        if ( !ctx ) continue;
        s.cpu_context_bytes += ctx->_alloc_size;
        ++s.context_count;
    }
    s.cpu_dynamic_total = s.cpu_context_bytes;

    s.total_bytes = s.gpu_total + s.cpu_static_total + s.cpu_dynamic_total;
    return s;
}

/* Dump the full breakdown to stdout as a sectioned table: GPU / CPU static / CPU heap, each with
   its own subtotal, then the grand total.  Bytes and KiB side by side so small (font glyph tables)
   and large (geometry buffers) buckets are both legible at a glance. */
void
gui_print_mem_stats( void )
{
    gui_mem_stats_t s = gui_mem_stats();
    const f32 kb = 1024.0f;

    #define GUI_MEM_ROW( label, bytes ) \
        printf( "  %-22s %10u B  (%8.1f KB)\n", (label), (u32)(bytes), (u32)(bytes) / kb )

    printf( "[gui] memory usage -- full breakdown:\n" );

    printf( "  -- GPU device (%u live surface%s) ------------------------\n",
            s.viewport_count, s.viewport_count == 1u ? "" : "s" );
    GUI_MEM_ROW( "vertex buffers",   s.gpu_vertex_bytes  );
    GUI_MEM_ROW( "index buffers",    s.gpu_index_bytes   );
    GUI_MEM_ROW( "font atlas texture", s.gpu_texture_bytes );
    if ( s.gpu_debug_bytes )
        GUI_MEM_ROW( "debug overlay buffers", s.gpu_debug_bytes );
    GUI_MEM_ROW( "  GPU subtotal",   s.gpu_total         );

    printf( "  -- CPU static (fixed backend buffers) ------------------\n" );
    GUI_MEM_ROW( "draw command list",  s.cpu_drawlist_bytes );
    GUI_MEM_ROW( "tessellation stage", s.cpu_tess_bytes     );
    GUI_MEM_ROW( "retained cache",     s.cpu_cache_bytes    );
    GUI_MEM_ROW( "font registry",      s.cpu_font_bytes     );
    GUI_MEM_ROW( "atlas + icons",      s.cpu_res_bytes      );
    GUI_MEM_ROW( "render + shaders",   s.cpu_render_bytes   );
    GUI_MEM_ROW( "text-select capture", s.cpu_select_bytes  );
    if ( s.cpu_debug_bytes )
        GUI_MEM_ROW( "debug tooling",  s.cpu_debug_bytes    );
    GUI_MEM_ROW( "frontend statics",   s.cpu_frontend_bytes );
    GUI_MEM_ROW( "  CPU static subtotal", s.cpu_static_total );

    printf( "  -- CPU heap (%u context%s) -----------------------------\n",
            s.context_count, s.context_count == 1u ? "" : "s" );
    GUI_MEM_ROW( "context blocks",        s.cpu_context_bytes );
    GUI_MEM_ROW( "  CPU heap subtotal",   s.cpu_dynamic_total );

    printf( "  --------------------------------------------------------\n" );
    printf( "  %-22s %10u B  (%8.1f KB)  (%.1f MB)\n",
            "TOTAL", s.total_bytes, s.total_bytes / kb, s.total_bytes / ( kb * kb ) );

    #undef GUI_MEM_ROW
}

/*==============================================================================================
    Multi-context API
==============================================================================================*/

/* Set whether a context listens for hover/click/nav input.  Call between frames.
   Multiple contexts may listen simultaneously; a deaf context renders but returns inert
   widget state.  The default context starts listening; secondary contexts start deaf. */
void
gui_ctx_set_listening( gui_ctx_id_t ctx, bool listen )
{
    if ( ctx >= 0 && ctx < GUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        s_ctx_pool[ ctx ]->listening = listen;
}

/* Allocate a fresh secondary context sized to `cfg` (NULL = the internal maxima).
   Each gets a unique id_salt so same-named widgets do not alias across contexts.
   Returns GUI_CTX_INVALID when the pool is full.  Call between frames. */
gui_ctx_id_t
gui_ctx_create( const gui_ctx_config_t* cfg )
{
    /* Resolve config: zero fields fall back to the internal caps.  max_dock_nodes == 0 in an
       EXPLICIT cfg is valid (disables docking); only a NULL cfg gets the dock default. */
    gui_ctx_config_t c = cfg ? *cfg
                             : ( gui_ctx_config_t ){ .max_dock_nodes = GUI_DEFAULT_DOCK_NODES };
    if ( !c.max_windows   ) c.max_windows   = GUI_DEFAULT_MAX_WINDOWS;
    if ( !c.state_slots   ) c.state_slots   = GUI_DEFAULT_STATE_SLOTS;
    if ( !c.popup_depth   ) c.popup_depth   = GUI_DEFAULT_POPUP_DEPTH;
    if ( !c.max_viewports ) c.max_viewports = GUI_MAX_VIEWPORTS;

    /* Counts are free of the old power-of-two rule (multiply-shift bucketing, gui_state.c);
       just floor so the small class (3/4 of this) keeps usable headroom. */
    u32 slots = c.state_slots;
    if ( slots < 16 ) slots = 16;

    /* Find a free pool slot (1..GUI_CTX_POOL_MAX-1). */
    i32 slot = -1;
    for ( i32 i = 1; i < GUI_CTX_POOL_MAX; ++i )
        if ( !s_ctx_pool[ i ] ) { slot = i; break; }
    if ( slot < 0 ) return GUI_CTX_INVALID;

    gui_context_t* ctx = ctx_alloc_slot( &c, slots, slot );
    if ( !ctx ) return GUI_CTX_INVALID;
    ctx->listening = false;   /* secondary contexts start deaf; caller opts in */

    s_ctx_pool[ slot ] = ctx;
    if ( (u32)slot >= s_ctx_pool_count ) s_ctx_pool_count = (u32)slot + 1u;
    return (gui_ctx_id_t)slot;
}

/* Free a secondary context; rebinds the default if this was current.  Never destroys slot 0. */
void
gui_ctx_destroy( gui_ctx_id_t ctx )
{
    if ( ctx <= 0 || ctx >= GUI_CTX_POOL_MAX || !s_ctx_pool[ ctx ] )
        return;
    gui_context_t* c = s_ctx_pool[ ctx ];
    if ( g_ctx == c ) ctx_bind( NULL );
    /* Destroy any GPU surfaces the context opened before releasing its memory block. */
    for ( u32 i = 0; i < c->vp.max; ++i )
        viewport_destroy( &c->vp.pool[ i ] );
    if ( c->_alloc ) free( c->_alloc );
    s_ctx_pool[ ctx ] = NULL;
}

/* Make ctx the current context.  GUI_CTX_DEFAULT (0) or an invalid handle rebinds the default. */
void
gui_ctx_bind( gui_ctx_id_t ctx )
{
    if ( ctx >= 0 && ctx < GUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        ctx_bind( s_ctx_pool[ ctx ] );
    else
        ctx_bind( NULL );
}

// clang-format on
/*============================================================================================*/
