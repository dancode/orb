/*==============================================================================================

    runtime_service/gui/core/gui_ctx.c -- the interact server's state, and the per-frame drivers
    that turn it over.

    One rule places code here: it DEFINES an ambient record, resets one, or is a bare verb over
    one.  The POLICY over a record lives with its feature -- keyboard focus in core/gui_focus.c,
    nav registration in core/gui_nav_item.c, the per-item recipe in core/gui_item.c.  The record
    TYPES live in core/gui_core.h (s_interaction, s_scope) and core/gui_ctx.h (s_build,
    gui_context_t); this file holds the instances.

    Sections, in file order:

        ambient records   s_interaction, s_replay_mode, s_build, s_scope
        bracketing        the item-flag stack + its verbs, the id-scope stack storage (verbs in
                          core/gui_id.c), the popup nesting counter
        context pool      s_ctx_pool + ctx_bind -- the whole multi-context seam
        viewport table    s_vp_pool + the win_id -> slot and drawable-size lookups
        pointer           rect_hit, the hardware-cursor request / flush
        arbitration       the interact_* verbs over hover / active, the focus edit latch
        frame drivers     interact_new_frame (once per APP frame), ctx_new_frame (once per
                          CONTEXT), and the frame-clock / redraw doors over the retained record

    Included by gui_core.c after core/gui_io.c, so s_io is in scope.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Ambient records

    One pointer, one keyboard, one mouse, so none of this is per-viewport or per-context: a
    single set of globals every context nominates into during its emit.  Contexts build
    sequentially on one thread, so the frame scratch is shared for the same reason -- each
    context in turn targets whichever records are live.
==============================================================================================*/

/* Hover / active / focus, persisting across frames.  Tier: ambient singular -- one physical user,
   so one record for the whole app.  Field notes worth keeping by the definition:

     repeat_t / repeat_on  one timer serves GUI_ITEM_BUTTON_REPEAT -- only one widget is active
                           at a time; both reset on the press frame.
     hover_win             one frame deferred: window_begin nominates into next_hover_win,
                           interact_new_frame promotes it.  Only the hover window
                           hit-tests its widgets.
     mouse_cursor          last writer wins (exactly one widget hovers, and resize bands
                           suppress widget hover); cursor_flush pushes it to the OS one frame
                           later; reset each frame.
     focused_id_at_frame_start / focus_ended_*
                           the departing widget's id + edit history latch for one frame so
                           is_item_deactivated_after_edit can read them. */

gui_interaction_t s_interaction;

/* True only while a volatile-widget callback is replayed standalone on an idle frame; set and
   cleared by replay_scope_enter/_exit (chrome/widgets/gui_volatile.c).  Same tier as
   hover_id/active_id: item_state reads it to short-circuit before any hit-test or record write,
   so a replay renders against the state the last real frame established and can never acquire
   state or see a fresh click. */

bool s_replay_mode;

/* Frame-build scratch -- "where am I emitting right now", rebuilt every frame as the widget
   tree is walked.  Nothing here survives frame_begin.  Field notes:

     win                   everything window_begin stamps and window_end consumes, in ONE record,
                           so the popup seam saves and restores it with a single assignment; the
                           fields after it are frame-global channels that must SURVIVE that seam.
     item_flags / next_*   the push-model behavior set; item_flags_take resolves the value for
                           the widget now emitting into s_scope.flags.
     combo_open / combo_item_clicked
                           combo dropdown coordination (gui_combo.c) -- a click in the body
                           dismisses the combo with no caller code; reset each frame as a
                           safety net.

   Layout pen and scroll state are NOT here (they live on the layout-frame stack, the window
   being just its root frame), nor is nav state (g_ctx->nav) -- the builder stays small. */

gui_build_t s_build;

/* The interaction scope -- the declared contract between composition and behavior (full contract
   with the type, core/gui_core.h).  Composition stamps it at its seams, behavior reads only it
   (never s_build) and publishes its result back into last_*.  The overlay seam saves and restores
   it wholesale, so a field added to the type only needs its per-frame reset in ctx_new_frame.
   Tier: frame scratch, same lifetime as s_build. */

gui_scope_t s_scope;

#ifdef GUI_DEBUG_OVERLAY

/* s_build is private to this unit; the debug overlay (gui_debug_overlay.c, in the render backend
   unit) tags each captured rect with the ambient build viewport through this accessor -- declared
   in gui_render.h, Debug builds only. */

gui_vp_t dbg_build_viewport( void ) { return s_build.win.viewport; }

#endif

/*==============================================================================================
    Item-flag stack

    The push-model behavior set (disabled, button-repeat, ...).  item_flag_push saves the current
    merged value and item_flag_pop restores it, so a push nests cleanly regardless of which bits
    it touched; item_flag_next queues a one-shot override for the next widget only.  item_flags_
    take resolves the two into the per-item scope at the emit seam.

    An over-deep push aliases the top slot and is still counted truthfully, mirroring the id /
    layout stacks, so push/pop stay paired past the cap.
==============================================================================================*/

#define GUI_ITEM_FLAG_DEPTH 16

static gui_item_flags_t s_item_flag_stack[ GUI_ITEM_FLAG_DEPTH ];
static u32              s_item_flag_sp;

/* Save the current merged flags, then set or clear `flag` in the live set.  Non-static: the
   public brackets (gui_push_item_flag / gui_disabled_begin, style/gui_stacks.c) wrap these. */
void
item_flag_push( gui_item_flags_t flag, bool enable )
{
    if ( s_item_flag_sp < GUI_ITEM_FLAG_DEPTH )
        s_item_flag_stack[ s_item_flag_sp ] = s_build.item_flags;
    ++s_item_flag_sp;    /* count truthfully so push/pop stay paired even past the cap */

    if ( enable ) s_build.item_flags |=  flag;
    else          s_build.item_flags &= ~flag;
}

/* Restore the merged flags saved by the matching push. */
void
item_flag_pop( void )
{
    if ( s_item_flag_sp == 0 ) return;
    --s_item_flag_sp;
    u32 i = s_item_flag_sp < GUI_ITEM_FLAG_DEPTH ? s_item_flag_sp : GUI_ITEM_FLAG_DEPTH - 1;
    s_build.item_flags = s_item_flag_stack[ i ];
}

/* Mark `flag` as controlled for the NEXT widget, with its on/off value.  Consumed and cleared by
   item_flags_take when that widget emits -- no pop needed. */
void
item_flag_next( gui_item_flags_t flag, bool enable )
{
    s_build.next_set |= flag;
    if ( enable ) s_build.next_val |=  flag;
    else          s_build.next_val &= ~flag;
}

/* Take the flags for the item now emitting: the stack value with the one-shot override applied
   over it (the override wins on the bits it controls), then clear the override and latch the
   result into the scope for item_state / widgets to read.  The PURE half of the per-item seam --
   the style commit and ambient draw application live in item_flags_resolve
   (stock/gui_adornment.c); this server never touches a style value or the draw state. */
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
   (stock/gui_adornment.c), which also restores the ambient draw/style state.  Window borders,
   scrollbars, and title bars are not items; without this they would inherit whatever the last
   body widget latched. */
void
item_flags_chrome_drop( void )
{
    s_scope.flags      = GUI_ITEM_NONE;
    s_scope.nav.placed = false;   /* chrome is not an item to keyboard nav either: whatever
                                     interacts past this seam lists in the F6 chrome lane */
}

/*==============================================================================================
    Id-scope stack (verbs in core/gui_id.c)

    The top of this stack is the seed every widget id combines against, so identical labels in
    different scopes never collide.  Regions seed it automatically (layout_push_region /
    layout_pop_region); push_id / pop_id add temporary levels for repeated widgets inside one
    region (list rows keyed by index).  Reset to empty each frame.

    Over-deep pushes alias the top slot rather than writing past the array, and id_seed clamps its
    read the same way -- so deep nesting degrades instead of crashing.
==============================================================================================*/

#define GUI_ID_STACK_DEPTH 32

static gui_id_t s_id_stack[ GUI_ID_STACK_DEPTH ];
u32             s_id_sp;

/*==============================================================================================
    Popup nesting depth (frame scratch)

    The open popup SET persists per context (g_ctx->popup.open / open_count, ordered parent ->
    child).  This counter is its per-frame half: the nesting depth while emitting.  popup_open
    writes a request at this depth and popup_begin matches its id against it, so the two counters
    balance through the normal push/pop and no slot is reused or lost.  The stack contract itself
    is chrome/popup/gui_popup.c's.
==============================================================================================*/

u32 s_popup_begin_count;   // current popup nesting depth (rebuilt per frame)

/*==============================================================================================
    Context pool -- the whole multi-context seam

    A context is the emission session code binds once and emits ALL its windows into; it owns the
    state that must persist between frames for that UI (gui_context_t, core/gui_ctx.h).  Slot 0 is
    the default context -- bound at init, freed only at shutdown, never torn down by ctx_destroy;
    slots 1..N come from ctx_create and share the same single-malloc block layout.  The storage is
    the server's; the PUBLIC lifecycle and the block ALLOCATION (which sizes chrome's records, so
    it needs whole-stack type visibility) live in frame/gui_context.c.

    Binding copies nothing, and every retained access spells the bound context out (g_ctx->retained,
    g_ctx->nav, g_ctx->win) so per-context state reads as distinct from the global s_* scratch at
    every call site.

    The frame clock (g_ctx->retained.frame) advances per CONTEXT at ctx_begin, not per app frame:
    a context not rebuilt on a given frame must not tick, or its live keyed-state entries would
    read as cold and be reclaimed -- losing scroll / open state -- while it is merely hidden.
==============================================================================================*/

gui_context_t* s_ctx_pool[ GUI_CTX_POOL_MAX ];
u32            s_ctx_pool_count;   /* live slot count; always >= 1 after init */
gui_context_t* g_ctx = NULL;       /* the bound context (extern'd in core/gui_ctx.h) */

/* Bind the active context; NULL rebinds the default. */
void
ctx_bind( gui_context_t* ctx )
{
    g_ctx = ctx ? ctx : s_ctx_pool[ 0 ];
}

/*==============================================================================================
    Viewport table -- the one real surface set

    Not per-context: OS windows and RHI contexts are a genuinely global, small, fixed-size
    resource (APP_WIN_MAX), so every context that ever existed shares this ONE table and differs
    from another only in which of its own windows assign into which slot.  [0] is the main
    swapchain, the rest are floaters.  A plain zero-init global -- not part of any context's
    malloc block -- sized once at compile time and outliving every context; the open/close
    lifecycle over it is frame/gui_viewport.c's.
==============================================================================================*/

gui_viewport_t s_vp_pool[ APP_WIN_MAX ];
i32            s_vp_count;

/* Resolve an app win_id to its viewport slot: the live slot (one with GPU buffers) whose recorded
   win_id matches, else 0 (the main swapchain).  Context-independent -- there is only one table.
   Forward-declared in core/gui_ctx.h; called by the mouse-input path in core/gui_io.c. */
static gui_vp_t
viewport_index_for_window( i32 win_id )
{
    for ( gui_vp_t i = 0; i < APP_WIN_MAX; ++i )
        if ( rhi_handle_valid( s_vp_pool[ i ].vb ) && s_vp_pool[ i ].win_id == win_id )
            return i;
    return 0;
}

/* Drawable size of a surface: its own extent once opened, else the s_io snapshot of the primary
   OS window (0 = not yet opened, or closed).  Every window-placement and clip computation goes
   through these rather than spelling the fallback out inline. */

f32 vp_w( gui_vp_t vp ) { i32 w = s_vp_pool[ vp ].disp_w; return w > 0 ? (f32)w : (f32)s_io.display_w; }
f32 vp_h( gui_vp_t vp ) { i32 h = s_vp_pool[ vp ].disp_h; return h > 0 ? (f32)h : (f32)s_io.display_h; }

/*==============================================================================================
    Pointer hit test

    Cursor (s_io) inside a rect -- the one primitive every hover gate, chrome grab, and hover
    nomination is built on.  Its rect-vs-rect sibling (rect_intersect) is not here: it needs no
    ambient cursor, so it is static inline in the leaf rect kit (rect/gui_rect.h) where every
    unit -- including the render server, for clip nesting -- can reach it.
==============================================================================================*/

bool
rect_hit( gui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/*==============================================================================================
    Hardware cursor

    gui owns the OS cursor shape only while it owns the mouse (hover_win set, or a widget drag in
    flight) -- the same want_capture_mouse fence non-UI code gates on.  A widget requests a shape
    for the frame with cursor_set; cursor_flush pushes the PREVIOUS frame's request to the OS
    window the cursor was over, deferred one frame exactly like hover_win.
==============================================================================================*/

/* Request a shape for this frame.  Last writer wins (one hover per frame). */
void cursor_set( app_cursor_t c )
{
    s_interaction.mouse_cursor = c;
}

/* Push the requested shape to the OS window under the pointer.  Reads last frame's request +
   hover state (called before interact_new_frame promotes the new frame's hover).
   app()->window_set_cursor is sticky, so dedupe on (window, shape); and on the frame gui releases
   the mouse, push ARROW once so a stale I-beam / resize shape does not linger -- after which the
   cursor belongs to the host (game scene).  Called once per app frame from gui_frame_begin. */
void
cursor_flush( void )
{
    static i32          s_flushed_win   = -1;                   // last window we pushed a shape to
    static app_cursor_t s_flushed_cur   = APP_CURSOR_ARROW;     // last shape pushed there

    bool want = ( s_interaction.hover_win != GUI_ID_NONE )      // gui owns the mouse: same fence
             || ( s_interaction.active_id != GUI_ID_NONE );     // as want_capture_mouse

    if ( want )
    {
        /* The OS window the cursor is in: viewport slot index -> its app win_id. */

        i32 win = 0;
        if ( s_io.mouse_viewport < s_vp_count )
            win = s_vp_pool[ s_io.mouse_viewport ].win_id;

        if ( win != s_flushed_win || s_interaction.mouse_cursor != s_flushed_cur )
        {
            app()->window_set_cursor( win, s_interaction.mouse_cursor );
            s_flushed_win = win;
            s_flushed_cur = s_interaction.mouse_cursor;
        }
    }
    else if ( s_flushed_win >= 0 )
    {
        /* Release edge: clear our shape once, then leave the cursor to the host. */
        if ( s_flushed_cur != APP_CURSOR_ARROW )
            app()->window_set_cursor( s_flushed_win, APP_CURSOR_ARROW );
        s_flushed_win = -1;
        s_flushed_cur = APP_CURSOR_ARROW;
    }
}

/*==============================================================================================
    Interaction arbitration -- the verbs over s_interaction, defined with the record.

    The read half is the questions every gesture gate asks, named once so a compound gate reads as
    a sentence instead of a chain of raw field comparisons; the write half is the two sanctioned
    doors a higher tier starts a gesture through.  core/ and interact/ are the only writers of the
    arbitration fields: everything above claims through these and reads the record for gating.
    Pure over the record -- no policy, no rect math; the per-item recipe that runs them in order
    is item_state (core/gui_item.c).
==============================================================================================*/

/* Nothing holds the pointer capture: no widget, drag, or resize is in flight. */
bool
interact_idle( void )
{
    return s_interaction.active_id == GUI_ID_NONE;
}

/* `id` holds the pointer capture (its press-drag gesture is in flight). */
bool
interact_held( gui_id_t id )
{
    return s_interaction.active_id == id;
}

/* The cursor is over window `win_id`'s BARE surface: it is the front-most window under the cursor
   AND no widget sits beneath the cursor -- the gate for chrome gestures (title-bar drag,
   double-click collapse, context-menu press) that must yield to any widget above them. */
bool
interact_hover_bare( gui_id_t win_id )
{
    return s_interaction.hover_win == win_id && s_interaction.hover_id == GUI_ID_NONE;
}

/* Claim the pointer capture for `id` (held by `button`) -- the one sanctioned door for a HIGHER
   tier to start a press-drag, so chrome-side gestures claim through this instead of poking the
   record.  Release is global: interact_new_frame drops active_id when `button` lifts. */
void
interact_claim( gui_id_t id, u8 button )
{
    s_interaction.active_id     = id;
    s_interaction.active_button = button;
}

/* Point hover arbitration at `owner`, making every OTHER window inert for the rest of this frame:
   item_state gates all hover on s_scope.win == hover_win, so redirecting hover_win freezes
   everything behind the owner with no per-widget code -- the window-scale analogue of active_id
   drag-modality.  The verb behind the popup modal fence (popup_apply_modal). */
void
interact_hover_fence( gui_id_t owner )
{
    s_interaction.hover_win = owner;
}

/* Mark the focused item as edited this frame -- called by input_field_edit whenever the buffer
   changes.  Accumulates in focused_id_edited (never cleared while focus stays); frame_end
   snapshots it into focus_ended_edited when focus departs, so is_item_deactivated_after_edit can
   read it for one frame after. */
void item_mark_edited( void ) { s_interaction.focused_id_edited = true; }

/*==============================================================================================
    Frame drivers

    Two resets, at two rates.  interact_new_frame turns over the GLOBAL records once per APP
    frame (one mouse, one keyboard, one hover window); ctx_new_frame turns over the frame scratch
    and ticks the retained clock once per CONTEXT.  Calling the global one per context would let
    the second ctx_begin clobber the hover_win / active_id the first resolved.
==============================================================================================*/

/* Once per app frame, from gui_frame_begin, before any ctx_begin. */
void
interact_new_frame( void )
{
    /* Snapshot the active + focused ids before this frame mutates them: the previous-frame
       baselines the is_item_activated / is_item_deactivated edge readers (core/gui_query.c) and
       frame_end's focus-departure check compare against. */
    s_interaction.active_id_prev            = s_interaction.active_id;
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
       (input_text sets focused_id from hover_id + press this same frame).  EXCEPTION -- the HOLD
       rule: while a live exclusive input mode (a GUI_WIN_MODAL window; the dev console) owns the
       focused widget, a press on non-focusable dead space keeps the focus instead of clearing it,
       so the console input holds its caret while the user sweeps a scrollback selection.  A press
       on another focusable widget in the mode still moves focus (item_state overwrites
       focused_id).  The mode and both of its rules live in core/gui_focus.c. */
    if ( s_io.mouse_pressed[ 0 ] && !focus_scope_holds( s_interaction.focused_id ) )
        s_interaction.focused_id = GUI_ID_NONE;

    /* Cleared each frame; the focused text field re-asserts it during its emit (input_field_edit). */
    s_interaction.focus_has_selection = false;

    /* Fresh cursor request -- the arrow until a widget asks otherwise. */
    s_interaction.mouse_cursor = APP_CURSOR_ARROW;
}

/* Once per context, from ctx_begin (frame/gui_frame_loop.c).  Touches no global s_interaction
   field -- those are the reset above.  The layout-stack reset and the per-frame STYLE reset are
   the flow and style units' own; ctx_begin pairs all three, since this server knows nothing of
   either. */
void
ctx_new_frame( void )
{
    /* Last-item introspection resets to "no item": a query before any widget this frame (or in a
       frame that emits none) reports false rather than reading a stale rect / status. */
    s_scope.last_id     = GUI_ID_NONE;
    s_scope.last_rect   = ( gui_rect_t ){ 0 };
    s_scope.last_status = ( gui_item_state_t ){ 0 };

    s_id_sp               = 0;       /* fresh id-scope stack; regions/push_id reseed it */
    s_build.wheel_used    = false;   /* the wheel starts unclaimed */
    s_build.win.viewport  = 0;       /* ambient viewport resets to primary */

    /* Fresh nav-stamp dispensers; nothing is placed until a layout cell is handed out. */

    s_build.nav_region_seq      = 0;
    s_build.nav_line_seq        = 0;
    s_scope.nav.placed          = false;
    s_scope.nav.skip            = false;

    /* Popup nesting depth is rebuilt as popup_begin / popup_end run; the open set persists. */

    s_popup_begin_count = 0;

    /* Combo body coordination is per-frame and re-set by combo_begin / combo_end; clear as a
       safety net. */

    s_build.combo_open          = false;
    s_build.combo_item_clicked  = false;

    /* Fresh item-flag state: empty stack, no next-item override, nothing disabled. */

    s_item_flag_sp              = 0;
    s_build.item_flags          = GUI_ITEM_NONE;
    s_build.next_set            = GUI_ITEM_NONE;
    s_build.next_val            = GUI_ITEM_NONE;
    s_scope.flags               = GUI_ITEM_NONE;

    /* The interaction clip starts at the full display, and the context's clock ticks. */

    s_scope.clip = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    ++g_ctx->retained.frame;
}

/*==============================================================================================
    Frame clock + redraw request -- the read / request doors over the retained record.

    Layers above the server read the monotonic per-context build counter for emit-gating and raise
    the bound context's dirty flag through these, rather than reaching into g_ctx->retained.  The
    owner still touches the fields directly: the bump is in ctx_new_frame above, the anim / item
    writes are in their own files, and the frame loop keeps the clear + read.
==============================================================================================*/

u32  gui_frame_index( void ) { return g_ctx->retained.frame; }
void redraw_request ( void ) { if ( g_ctx ) g_ctx->retained.wants_redraw = true; }

// clang-format on
/*============================================================================================*/
