/*==============================================================================================

    runtime_service/gui/core/gui_ctx.c -- Immediate-mode context state and per-frame drivers.

    Declares all persistent ambient and frame-scratch state (s_interaction, s_build, gui_nav_state_t,
    layout_frame_t, gui_context_t) and drives the per-frame lifecycle via ctx_new_frame.  Owns the
    context pool storage (s_ctx_pool, ctx_bind); the PUBLIC multi-context lifecycle, the block
    ALLOCATION (ctx_alloc_slot -- it sizes chrome's records, whole-stack knowledge), and the
    memory-stats aggregation live in the frame unit (R4, completed R11).

    ID hashing and the keyed state pool (id_hash, id_combine, id_seed/push/pop, gui_state_get,
    GUI_STATE) are in core/gui_id.c + core/gui_state.c, included just after this file.

    Public IO accessors (gui_want_capture_*, gui_is_key_*, gui_is_mouse_*, etc.) are in
    core/gui_query.c, the interact server's read surface.

    Included by gui_core.c (the INTERACT SERVER unit) after core/gui_io.c so s_io is in scope.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    State
==============================================================================================*/

/* s_build.win.rec points at a live gui_window_t pool record (window types in core/gui_ctx.h; the
   pool is reached through g_ctx) so window_end can write scroll / content extent back into it. */

/* Ambient interaction state -- the one live hover / active / focus, persisting across frames.  One
   pointer, one keyboard, one mouse, so none of it is per-viewport or per-context: a single global
   shared by every context, into which listening contexts nominate hover / active during their emit.
   Tier: ambient singular (see ARCHITECTURE.md sec 1, state tiers).  The TYPE lives in
   core/gui_core.h (gui_interaction_t, extern'd for the carved units -- inc 10); this file stays
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
   by gui_replay_scope_enter/_exit (chrome/widgets/gui_volatile.c).  Ambient frame-phase state, same tier as
   hover_id/active_id: item_state (core/gui_item.c) reads it inline to short-circuit before any
   hit-test or write to s_interaction/s_build -- a replay renders against the hover/active/focus the
   last real frame established and can never acquire state or see a fresh click, since interaction is
   resolved only on real frames. */
bool s_replay_mode;

/* Frame-build scratch -- the "where am I emitting right now" context, rebuilt every frame as the
   widget tree is walked.  Nothing here survives begin_frame: it is set and repopulated by the
   window_begin / child_begin / widget calls, never read across a frame boundary.  Because contexts
   build sequentially on one thread, this stays a single global builder reused by each context in
   turn rather than per-context state.  Tier: frame scratch. */

/* The TYPE lives in core/gui_ctx.h (gui_build_t, extern'd for the carved units -- inc 10);
   this file stays the owner (defines and resets it).  Notes worth keeping by the definition:

   - win: everything window_begin stamps and window_end consumes, one record (gui_win_ctx_t) so
     the popup seam saves/restores it with a single assignment; the fields after it are
     frame-global channels that must SURVIVE that seam.
   - Layout pen + scroll region state live on the layout-frame stack below; the window is just
     the root frame.
   - item_flags / next_set / next_val: the push-model behavior set; the value resolved for the
     widget currently emitting is latched into s_scope.flags by item_flags_take.
   - combo_open / combo_item_clicked: combo dropdown coordination (gui_combo.c) -- a click in
     the body dismisses the combo with no caller code; reset each frame as a safety net.
   - Nav state is NOT part of this scratch: it lives in g_ctx->nav so the builder stays small. */

gui_build_t s_build;

/* The interaction scope -- the declared contract between composition and behavior; the full
   contract lives with the type (gui_scope_t, core/gui_core.h).  Stamped directly by composition
   at its seams, read by behavior, saved/restored wholesale at the popup seam -- a field added to
   the type only needs its per-frame reset in ctx_new_frame below.  Tier: frame scratch, same
   lifetime as s_build. */

gui_scope_t s_scope;

#ifdef GUI_DEBUG_OVERLAY
/* The debug overlay (gui_debug_overlay.c) lives in the render backend unit and tags each captured rect
   with the ambient build viewport.  s_build is private to this unit, so the overlay reads it
   across the unit seam through this accessor (declared in gui_render.h, Debug builds only). */
u32 gui_dbg_build_viewport( void ) { return s_build.win.viewport; }
#endif

/*==============================================================================================
    Keyboard navigation state (g_ctx->nav)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather than
    the mouse -- plus the menu-bar state machine layered on it.  gui_nav_state_t is defined in
    core/gui_ctx.h; the instance is the g_ctx->nav member of the bound context, reached through g_ctx, so
    each context keeps its own cursor.  Movement is structural, not spatial: every item of the
    scoped window records itself into the nav item list as it emits (with the region + line the
    layout engine stamped when it placed it), and the next nav_new_frame resolves a move as index
    math over that list -- one frame deferred, exactly as hover_win lags the cursor.  gui_nav.c
    drives it; nav_item_register (core/gui_item.c) is the per-item seam.
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

/* Push: save the current merged flags, then set or clear `flag` in the live set.  Non-static:
   the public brackets (gui_push_item_flag / gui_disabled_begin, style/gui_stacks.c) wrap these
   from the frame unit. */
void
item_flag_push( gui_item_flags_t flag, bool enable )
{
    if ( s_item_flag_sp < GUI_ITEM_FLAG_DEPTH )
        s_item_flag_stack[ s_item_flag_sp ] = s_build.item_flags;
    ++s_item_flag_sp;    /* count truthfully so push/pop stay paired even past the cap */

    if ( enable ) s_build.item_flags |=  flag;
    else          s_build.item_flags &= ~flag;
}

/* Pop: restore the merged flags saved by the matching push. */
void
item_flag_pop( void )
{
    if ( s_item_flag_sp == 0 ) return;
    --s_item_flag_sp;
    u32 i = s_item_flag_sp < GUI_ITEM_FLAG_DEPTH ? s_item_flag_sp : GUI_ITEM_FLAG_DEPTH - 1;
    s_build.item_flags = s_item_flag_stack[ i ];
}

/* Next-item override: mark `flag` as controlled for the next widget, with its on/off value.
   Consumed (and cleared) by item_flags_take when that widget emits -- no pop needed. */
void
item_flag_next( gui_item_flags_t flag, bool enable )
{
    s_build.next_set |= flag;
    if ( enable ) s_build.next_val |=  flag;
    else          s_build.next_val &= ~flag;
}

/* Take the flags for the item now emitting: the stack value with the one-shot next-item override
   applied over it (the override wins on the bits it controls), then clear the override and latch
   the result into the scope for item_state / widgets to read.  This is the PURE half of the
   per-item seam: the style commit and the ambient draw application (alpha dim, default rounding)
   live in the item_flags_resolve wrapper (element/gui_adornment.c) -- the interact server never
   touches a style value or the draw state. */
gui_item_flags_t
item_flags_take( void )
{
    gui_item_flags_t f = ( s_build.item_flags & ~s_build.next_set ) | ( s_build.next_val & s_build.next_set );
    s_build.next_set = 0;
    s_build.next_val = 0;
    s_scope.flags = f;
    return f;
}

/* Clear the per-item scope before chrome runs -- the pure half of item_flags_chrome_reset
   (element/gui_adornment.c), which also restores the ambient draw/style state.  Window/child
   borders, scrollbars, and titlebars are not items; without this they would inherit whatever
   the last body widget latched. */
void
item_flags_chrome_drop( void )
{
    s_scope.flags  = GUI_ITEM_NONE;
    s_scope.nav.placed = false;   /* chrome is not an item to keyboard nav either: whatever
                                          interacts past this seam lists in the F6 chrome lane */
}

/* The layout-frame stack (s_layout_stack, s_layout_sp, lf) moved to the FLOW unit at R11
   (flow/gui_layout_core.c): the frames are composition's records, and with the per-unit
   include graph enforced this server can no longer see layout_frame_t -- correctly. */

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
    clip, and paint independent of that parent.  gui_overlay_save_t (chrome/gui_chrome.h) holds the parent
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

/* GUI_CTX_POOL_MAX lives in core/gui_ctx.h: the public lifecycle (frame/gui_context.c) walks
   the pool too.  The pool itself is the interact server's storage; the frame unit reaches it
   through the externs there. */

gui_context_t* s_ctx_pool[ GUI_CTX_POOL_MAX ];
u32            s_ctx_pool_count;   /* live slot count; always >= 1 after init */

gui_context_t* g_ctx = NULL;   /* bound context (extern'd in core/gui_ctx.h for the carved units) */

/* ctx_alloc_slot (the single-malloc block layout) and ctx_pool_init (the default context)
   moved to frame/gui_context.c at R11: the layout sizes chrome's records (gui_popup_t,
   gui_dock_node_t) with sizeof, and this server holds them opaque -- allocation is the
   orchestrator's whole-stack knowledge.  The pool storage above stays here. */

/* Bind the active context; every alias above resolves into it from here on.  NULL rebinds the
   default.  This is the whole multi-context seam -- no state is copied. */
void
ctx_bind( gui_context_t* ctx )
{
    g_ctx = ctx ? ctx : s_ctx_pool[ 0 ];
}

/* Resolve an app win_id to the primary context's viewport index.  OS windows and their viewport
   slots are owned by the primary context (slot 0) regardless of which context is currently bound;
   secondary contexts share the same OS windows and render surfaces rather than owning separate ones.
   Searches the primary context's viewport array for a live slot (one with GPU buffers) whose
   recorded win_id matches; falls back to 0 (main swapchain) if none found.
   Forward-declared in core/gui_ctx.h; called by the mouse-input path in core/gui_io.c. */
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
   (window, shape) so an unchanged cursor is not re-posted every frame.  Non-static: called
   once per app frame from gui_frame_begin (frame/gui_frame.c). */
void
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

      confine  -- only the mode's own widgets may TAKE focus (focus_allowed, core/gui_item.c);
                  no background window can steal it.
      hold     -- focus is STICKY within the mode: it never falls to nothing.  Focus can drop two
                  ways and the mode blocks both -- a press on non-focusable dead space
                  (interaction_frame_reset below) and a widget releasing its own capture on Enter /
                  Escape (item_focus_release, core/gui_item.c).  You cannot "select nothing"
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
void
interaction_frame_reset( void )
{
    /* Snapshot the active item before this frame mutates it -- the previous-frame baseline the
       is_item_activated / is_item_deactivated edge readers compare against (core/gui_query.c). */
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
   Called by ctx_begin (frame/gui_frame.c) for every context -- does NOT touch the global
   s_interaction fields (those are reset once per app frame by interaction_frame_reset). */
void
ctx_new_frame( void )
{
    /* Last-item introspection resets to "no item": a query before any widget this frame (or in
       a frame that emits none) reports false rather than reading a stale rect / status. */
    s_scope.last_id     = GUI_ID_NONE;
    s_scope.last_rect   = ( gui_rect_t ){ 0 };
    s_scope.last_status = ( gui_item_state_t ){ 0 };

    /* The layout stack's frame reset (s_layout_sp = 0) rides gui_ctx_begin since R11 -- the
       stack is the flow unit's, and the orchestrator pairs the two resets (the R4 precedent:
       ctx_new_frame + style_new_frame).  The interaction clip starts at the full display,
       and the wheel is unclaimed. */
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

    /* The per-frame STYLE reset (style_new_frame) is not called from here: this server knows
       nothing of style.  ctx_begin (frame/gui_frame.c) runs it right after this returns. */
    s_scope.clip = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    ++g_ctx->retained.frame;
}

/* Public IO accessors (gui_want_capture_*, gui_is_key_*, gui_is_mouse_*, gui_get_*)
   are defined in core/gui_query.c, included in the user/ tier below.
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
    Moved out at the R4 carve (GUI_SERVER_PLAN.md):

      gui_mem_stats / gui_print_mem_stats    -> gui_ui_mem.c (frame unit) -- the full-footprint
                                                accounting aggregates BOTH servers
                                                (gui_backend_memory), orchestrator work.
      gui_ctx_create / destroy / bind /
      set_listening                          -> frame/gui_context.c -- context destruction tears
                                                down GPU surfaces (viewport_destroy, a render-
                                                server call this server must never make).  The
                                                pool storage + ctx_alloc_slot/ctx_bind stay here.
==============================================================================================*/

// clang-format on
/*============================================================================================*/
