/*==============================================================================================

    runtime_service/gui/core/gui_item.c -- The standard item protocol.

    item_state: the default COMPOSITION of the interaction services, run once per item
    over a finished rect.  Every stock widget, chrome control, and gui_item() (user/) obtains
    its interaction state through this one recipe, so they all share the same feel:

        latch      -- record the item as "the last item" for the IsItem* queries / context menus
        gate       -- disabled flag, deaf context, volatile replay: return inert without arbitrating
        arbitrate  -- hover via the exclusive-ownership rules (one mouse: occlusion by hover_win,
                      modal-while-dragging via active_id), press captures active / focus
        resolve    -- click = press + release on the same item; auto-repeat replaces it when
                      GUI_ITEM_BUTTON_REPEAT is set (the repeat clock lives here too)
        track      -- register the item as a keyboard-nav candidate (the nav list is index math
                      for gui_nav.c) and apply a pending nav activation as a synthesized click

    The protocol is deliberately a straight-line composition: each step is a service that knows
    nothing about sliders or buttons -- it serves exclusivity, clicks, or tracking over (id, rect,
    kind).  A widget that needs a different recipe (drag-only, no click) should grow from these
    services, not from a flag on this function.

    The ownership state itself (s_interaction) lives in core/gui_ctx.c -- the context owns the
    record and the verbs over it (interact_idle / interact_claim / ...); this file is the recipe
    that runs them in order.  Everything behavior consumes about "where is this item emitting"
    comes from the interaction scope (s_scope, same file): the owner window, the interaction
    clip, the chrome suppression, and the per-item flag/nav stamps, all placed there by
    composition at its seams.  Behavior never reads the composer scratch (s_build) -- the scope
    record IS the composition->behavior contract.

    The two steps big enough to own their own files are called from here and live beside it:
    the nav registration seam (nav_item_register, core/gui_nav_item.c) and the focus policy
    (focus_allowed / focus_request_take, core/gui_focus.c).

    The file ends with its public door: gui_item / gui_invisible_button, the same protocol run
    from a LABEL and a caller-derived rect -- a custom widget is rect + item() + draw_*.

    Part of the core unit (gui_core.c); its consumers are every tier above (flow/ scrollbars,
    widgets/, window/ chrome, dock/, table/) plus gui()->item callers.

==============================================================================================*/
// clang-format off

/* Auto-repeat cadence for a held button (GUI_ITEM_BUTTON_REPEAT): the pause before the first
   repeat, then the interval between repeats.  Seconds; matches the familiar key-repeat feel. */

#define REPEAT_DELAY 0.30f
#define REPEAT_RATE  0.05f

/* One tick of the held-button repeat clock (state in s_interaction, since only one widget is active at a
   time).  Fires immediately on the press frame, then again once repeat_t crosses the initial delay
   and thereafter each rate interval.  Returns true on a fire frame; the caller routes it to
   st.clicked.  Subtracting the threshold (vs zeroing) keeps the cadence steady across uneven dt. */

static bool
item_repeat_tick( bool pressed )
{
    if ( pressed )
    {
        s_interaction.repeat_t  = 0.0f;
        s_interaction.repeat_on = false;    /* the next fire waits the longer initial delay */
        return true;                        /* press itself is the first fire */
    }

    s_interaction.repeat_t += s_io.dt;
    f32 thresh = s_interaction.repeat_on ? REPEAT_RATE : REPEAT_DELAY;
    if ( s_interaction.repeat_t >= thresh )
    {
        s_interaction.repeat_t -= thresh;
        s_interaction.repeat_on = true;    /* past the delay -- switch to the faster rate */
        return true;
    }
    return false;
}

/* Arm the press-to-drag distance tracker for `id` at the cursor's current position -- call once,
   on st.pressed.  Single-slot in s_interaction, the same reasoning as active_id: only one widget
   is ever held at a time, so one arm record covers whichever widget currently owns it. */
void
item_drag_arm( gui_id_t id )
{
    s_interaction.drag_armed_id = id;
    s_interaction.drag_press_x  = s_io.mouse_x;
    s_interaction.drag_press_y  = s_io.mouse_y;
    s_interaction.drag_exceeded = false;
}

/* True once the cursor has moved more than thresh_px from where item_drag_arm(id) was called --
   sticky for the rest of the hold, so a drag that wanders back near the press point still reads
   as a drag, not a click.  False if `id` never armed, or a different widget now owns the slot. */
bool
item_drag_exceeded( gui_id_t id, f32 thresh_px )
{
    if ( s_interaction.drag_armed_id != id )
        return false;

    if ( !s_interaction.drag_exceeded )
    {
        f32 dx = s_io.mouse_x - s_interaction.drag_press_x;
        f32 dy = s_io.mouse_y - s_interaction.drag_press_y;
        if ( dx * dx + dy * dy > thresh_px * thresh_px )
            s_interaction.drag_exceeded = true;
    }
    return s_interaction.drag_exceeded;
}

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

gui_item_state_t
item_state( gui_id_t id, gui_rect_t r, gui_item_kind_t kind )
{
    gui_item_state_t st = { 0 };

    /* Latch the most recent item id for context menus / tooltips (popup_context_item_begin,
       set_item_tooltip).  Done before the disabled early-out so a disabled widget still counts
       as the last item -- the anchor is "what was just emitted", regardless of its state. */

    s_scope.last_id   = id;
    s_scope.last_rect = r;             /* item-query getters read this for "the widget just emitted" */
    STEP_SET_OWNER( id );              /* command-stepper attribution: this widget's paint follows */

    /* Consume the one-shot nav opt-out here, before any early-out below can leak it onto the
       next widget.  A flagged item (scrollbar, drag strip) still interacts normally with the
       mouse; it just never registers as a keyboard target. */
    bool nav_skip    = s_scope.nav.skip;
    s_scope.nav.skip = false;

    /* Disabled item: inert this frame -- no hover, active, focus, or click.  Returning the zeroed
       state here is the one place that suppresses interaction for every widget, the behavioral half
       of GUI_ITEM_DISABLED (the visual dim is the draw list's global alpha, set at resolve).  The
       flags were latched by cell_next_w just before this call. */
    if ( s_scope.flags & GUI_ITEM_DISABLED )
    {
        s_scope.last_status = st;      /* a disabled item is still the last item, reported inert */
        return st;
    }

    /* Deaf context: not listening this frame -- render but return inert state.
       last_id/rect are latched above so item-query calls still work. */

    if ( !g_ctx->listening )
    {
        s_scope.last_status = st;
        return st;
    }

    /* Volatile-callback replay -- full feature in chrome/widgets/gui_volatile.c and
       render/pipeline/gui_build_volatile.c; s_replay_mode itself is declared in gui_ctx.c.  Report the
       ambient hover/active/focused state as-is, but never touch it and never hit-test -- the
       interaction scope (s_scope: win, clip, flags, ...) is only meaningful between a real
       window_begin/window_end, and a replay runs outside that entirely. */
    if ( s_replay_mode )
    {
        st.hover   = ( s_interaction.hover_id   == id );
        st.active  = ( s_interaction.active_id  == id );
        st.focused = ( s_interaction.focused_id == id );
        return st;
    }

    /* Hot only when this widget belongs to the window the cursor is over (hover_win,
       resolved last frame).  Widgets in any other window short-circuit before rect_hit,
       so occluded windows do no hit-testing at all -- occlusion is decided once, at the
       window level, not per widget.

       Modal-while-dragging: once any item owns active_id (a slider, scrollbar, or window
       drag is in flight) every other item is frozen -- only the active item may hover.
       The active item keeps interacting through st.active below, which reads active_id
       directly, so a drag stays live while the cursor sweeps over inert neighbours. */

    bool can_hover = interact_idle() || interact_held( id );
    bool win_hover = ( s_scope.win == s_interaction.hover_win );
    bool eligible  = can_hover && win_hover && !s_scope.resize_hot;

    /* While the keyboard is the active nav instrument (nav_highlight), the mouse does not set hover:
       the fill is mutually exclusive, so a mouse-hovered item never fills alongside the nav item
       (the nav ring still shows its location).  A mouse move or click drops nav_highlight
       (gui_nav.c), re-enabling hover that same frame. */

    if ( eligible && !g_ctx->nav.highlight && rect_hit( s_scope.clip ) && rect_hit( r ) )
         s_interaction.hover_id = id;

    /* Programmatic focus: a queued set_keyboard_focus request lands on the first focusable
       widget emitted after it -- the keyboard twin of click-to-focus below.  Taken only for a
       window the exclusive fence allows, so the request stays latched and lands on the modal
       window's own input rather than a background field emitted earlier in the frame
       (both verbs: core/gui_focus.c). */
    if ( kind == ITEM_FOCUSABLE && focus_allowed( s_scope.win ) && focus_request_take() )
        s_interaction.focused_id = id;

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_interaction.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_interaction.active_id = id;
        st.pressed      = true;
        if ( kind == ITEM_FOCUSABLE && focus_allowed( s_scope.win ) )
            s_interaction.focused_id = id;

        /* A click moves the nav cursor to the clicked item, so the keyboard resumes from what
           was clicked -- Shift+arrow then extends a selection from the clicked row instead of
           re-entering the window at its first item.  Unconditional on nav.active: this frame's
           press already dropped it (nav_new_frame), so gating on it would never fire.  Nothing
           paints until a nav key raises nav.active again.  Same gate as the registration below:
           only an item the nav window would list may take the cursor. */

        if ( s_scope.win == g_ctx->nav.win && !nav_skip )
        {
            g_ctx->nav.id       = id;
            g_ctx->nav.goal_set = false;
        }
    }

    st.hover   = ( s_interaction.hover_id == id );
    st.active  = ( s_interaction.active_id == id );
    st.focused = ( s_interaction.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_interaction.hover_id == id && s_interaction.active_id == id;

    if ( st.focused )
    {
        /* Record which window owns the keyboard focus, for the exclusive-scope focus lock (a modal
           input mode holds its focus sticky; see interact_new_frame).  Refreshed every frame the
           focused widget re-emits, so it always names that widget's current window. */
        s_interaction.focused_win = s_scope.win;

        /* The focus ring, from this seam for the same reason as the nav ring below: it must be
           uniform across every focusable widget, and this is the one point every widget passes
           through.  Marking only -- the skin paints it at the end of the window body
           (rings_paint, stock/gui_adornment.c), where the widgets that would cover it are
           already down.  Do not read style or draw here. */
        ring_mark_focus( r );
    }

    /* Keyboard nav: an item in the nav window registers as a candidate and, if it is the nav
       cursor, takes a synthesized click from an Enter/Space activation -- the keyboard mirror of
       the mouse hit-test above, through the same one seam every widget already passes through. */

    if ( s_scope.win == g_ctx->nav.win && !nav_skip )
        nav_item_register( id, r, &st, kind );

    /* Auto-repeat (GUI_ITEM_BUTTON_REPEAT): while held with the cursor still over it, fire on the
       press then repeatedly on the timed cadence -- replacing the release-click for this widget.
       Gated on the cursor being over it so sliding off pauses the repeat, like a real spin button. */

    if ( ( s_scope.flags & GUI_ITEM_BUTTON_REPEAT ) && st.active && s_interaction.hover_id == id )
    {
        st.clicked = item_repeat_tick( st.pressed );

        /* A held repeat button advances by time alone, not by mouse movement, so it needs a frame
           even when the mouse sits dead still -- without this the idle-skip sees no input change,
           the build never re-runs, and repeat_t never accumulates past the first fire. */
        g_ctx->retained.wants_redraw = true;
    }

    /* Debug overlay: every interactive widget passes through here, so this one site captures
       the hit rects -- tinted by hover/active so the live interaction is visible.  Capture the
       *visible* rect (the widget clipped to the active region clip): a row scrolled fully
       outside its child box has an empty intersection and is not hit-testable, so it is dropped
       from the overlay too, rather than drawing an interaction rect outside the clip box. */

#ifdef GUI_DEBUG_OVERLAY
    if ( eligible )
    {
        gui_rect_t vis = rect_intersect( r, s_scope.clip );
        if ( vis.w > 0.0f && vis.h > 0.0f )
            DBG_WIDGET( id, vis, st.hover, st.active );
    }
#endif

    s_scope.last_status = st;   /* publish the resolved state for the item-query readers */
    return st;
}

/* One frame of a bare grab protocol over (id, r): the behavior seam for hot chrome that is not
   a widget -- a dock splitter gutter, a table column boundary.  No hover_id, no focus, no nav;
   just "hot while the cursor sits on r, claim active_id on press".  `gate` is the caller's
   hover-domain test, since this chrome lives in different domains (a splitter gutter sits over
   no window: hover_win == NONE on its viewport; a table boundary: its window front-most) --
   arbitration stays here, the domain is policy.  The grab claims the left button, released
   globally when it lifts.  Returns hot; *active reports this id's in-flight grab (true from
   the press frame on). */
bool
item_grab( gui_id_t id, gui_rect_t r, bool gate, bool* active )
{
    *active  = interact_held( id );
    bool hot = gate && interact_idle() && rect_hit( r );
    if ( hot && s_io.mouse_pressed[ 0 ] )
    {
        s_interaction.active_id     = id;
        s_interaction.active_button = 0;
        *active = true;
    }
    return hot;
}

/*==============================================================================================
    Compound-widget bracket -- see the contract comment in core/gui_core.h.

    A widget made of widgets runs item_state for its OWN id/rect, then brackets its inner
    emissions: begin snapshots what the outer item published into s_scope (last_* + flags), the
    body emits sub-items (which overwrite them freely), end restores the snapshot -- so the
    item-query readers, context menus, and tooltips after the widget report the OUTER item,
    not the last internal part.  The layout variant additionally roots an id scope at the
    outer id and opens a transient sub-layout over the widget's rect, so the body can emit
    real widgets with the normal layout verbs; end unwinds both.
==============================================================================================*/

gui_item_sub_t
gui_item_sub_begin( void )
{
    gui_item_sub_t s;
    s.last_id     = s_scope.last_id;
    s.last_rect   = s_scope.last_rect;
    s.last_status = s_scope.last_status;
    s.flags       = s_scope.flags;
    s.layout      = false;
    return s;
}

gui_item_sub_t
gui_item_sub_layout_begin( gui_id_t id, gui_rect_t r )
{
    gui_item_sub_t s = gui_item_sub_begin();
    s.layout         = true;
    id_push( id );                    /* inner labels salt against the outer widget id */
    gui_push_layout_overlay( r );     /* transient frame over the widget's rect; verbs work inside */
    return s;
}

void
gui_item_sub_end( gui_item_sub_t s )
{
    if ( s.layout )
    {
        gui_pop_layout();
        id_pop();
    }
    s_scope.flags       = s.flags;
    s_scope.last_id     = s.last_id;
    s_scope.last_rect   = s.last_rect;
    s_scope.last_status = s.last_status;
}

/*==============================================================================================
    The public door -- the protocol from a LABEL and a caller-derived rect.

    gui_item() is the user-UI behavior seam: run this file's state machine over a rect the
    CALLER derived (a canvas() cut, a split/carve panel, custom math) and report the resolved
    state.  A user widget is built on it: get a rect, ask for behavior, draw your own
    presentation -- and it hovers, press-captures, clicks, and registers for keyboard nav
    exactly like a stock widget, including the modal-while-dragging freeze and the last-item
    queries (is_item_hovered, popup_context_item_begin).  The stock widgets differ only in
    where their rect comes from (the composer) -- not in the protocol.

    The caller's vocabulary lives with its machinery, so these sit here rather than in the
    gesture unit: item_id (core/gui_id.c) resolves the label grammar, item_state above does
    the rest, and there is no translation layer -- item_state's result IS the public record.
==============================================================================================*/

gui_item_state_t
gui_item( const char* id_str, gui_rect_t r )
{
    return item_state( item_id( id_str ), r, ITEM_BUTTON );
}

/* gui_item reduced to its click bit -- the one-liner convenience. */
bool
gui_invisible_button( const char* id_str, gui_rect_t r )
{
    return gui_item( id_str, r ).clicked;
}

// clang-format on
/*============================================================================================*/
