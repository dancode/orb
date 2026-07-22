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
    record; this file is the service that arbitrates it.  Everything behavior consumes about
    "where is this item emitting" comes from the interaction scope (s_scope, same file): the
    owner window, the interaction clip, the chrome suppression, and the per-item flag/nav stamps,
    all placed there by composition at its seams.  Behavior never reads the composer scratch
    (s_build) -- the scope record IS the composition->behavior contract.

    LAYERING NOTE: nav_item_register invokes the present-tier focus ring (draw_nav_ring,
    element/gui_adornment.c) -- behavior decides WHEN the system adornment paints; the paint
    policy (color, thickness, extent) lives with the skin.  See the comment at that site.

    Part of the core unit (gui_core.c); draw_nav_ring / NAV_RING resolve cross-unit, and
    every consumer (flow/ scrollbars, widgets/, window/ chrome, dock/, table/).

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

/* Keyboard-nav per-item seam.  Called from item_state for every item that belongs to the nav
   window (s_scope.win == g_ctx->nav.win), the keyboard mirror of the hover hit-test above.  It does
   three things: records the item into the frame's nav list (with the structural region/line stamp
   the layout engine latched when it placed it -- gui_nav.c resolves the next move as index math
   over this list), notes whether the nav cursor was seen, and -- for the current nav item --
   lights the focus ring and synthesizes a click from an Enter/Space activation so every widget
   activates from the keyboard with no per-widget code, exactly as a mouse click flows through
   st.clicked. */

/* Bring the nav cursor's rect into view -- the keyboard analogue of the wheel.  Runs once, on the
   frame the cursor was adopted (g_ctx->nav.scroll_chase), for the layout-placed cursor item: walk the
   open region stack innermost-out and nudge each region's scroll so the item is visible, correcting
   the rect level by level so an item deep in a nested child pulls every ancestor into line.  Like
   the wheel, the new offset only reaches the screen next frame (this frame's pen already used the
   old one), so wants_redraw forces the follow-up frame that shows it. */
static void
nav_scroll_chase( gui_rect_t r )
{
    const f32 pad = NAV_RING + 2.0f;   /* breathing room so the ring lands clear of the view edge */

    u32 top = ( s_layout_sp <= GUI_LAYOUT_DEPTH ) ? s_layout_sp : GUI_LAYOUT_DEPTH;
    for ( i32 i = (i32)top - 1; i >= 0; --i )
    {
        layout_frame_t* f = &s_layout_stack[ i ];
        if ( f->flags & GUI_WIN_NOSCROLL ) continue;

        f32 vx0 = f->view.x;   /* the region's resolved view rect: the same gutter-adjusted */
        f32 vy0 = f->view.y;   /* extents the region's own scrollbars are sized against     */

        /* Overshoot per axis: pull the near edge in (top-aligning an item taller than the view),
           else the far edge.  Clamped into the scroll range measured last frame, so a chase can
           never scroll past the content -- the same clamp the wheel applies. */
        f32 dy = 0.0f;
        if ( r.h > f->view.h - 2.0f * pad || r.y < vy0 + pad )
            dy = r.y - ( vy0 + pad );
        else if ( r.y + r.h > vy0 + f->view.h - pad )
            dy = ( r.y + r.h ) - ( vy0 + f->view.h - pad );

        f32 max_y  = f->scroll->content_h - f->view.h;
        if ( max_y < 0.0f ) max_y = 0.0f;
        f32 want_y = clampf( f->scroll->scroll_y + dy, 0.0f, max_y );
        dy = want_y - f->scroll->scroll_y;

        f32 dx = 0.0f;
        if ( r.w > f->view.w - 2.0f * pad || r.x < vx0 + pad )
            dx = r.x - ( vx0 + pad );
        else if ( r.x + r.w > vx0 + f->view.w - pad )
            dx = ( r.x + r.w ) - ( vx0 + f->view.w - pad );

        f32 max_x  = f->scroll->content_w - f->view.w;
        if ( max_x < 0.0f ) max_x = 0.0f;
        f32 want_x = clampf( f->scroll->scroll_x + dx, 0.0f, max_x );
        dx = want_x - f->scroll->scroll_x;

        if ( dy != 0.0f || dx != 0.0f )
        {
            f->scroll->scroll_y = want_y;
            f->scroll->scroll_x = want_x;
            r.y -= dy;   /* where the item lands next frame -- the ancestors check that position */
            r.x -= dx;
            g_ctx->retained.wants_redraw = true;
        }
    }
}

static void
nav_item_register( gui_id_t id, gui_rect_t r, gui_item_state_t* st, gui_item_kind_t kind )
{
    /* Dormant gate: while the keyboard is fully disengaged (nav_finish cleared reg_all -- no
       cursor, not active, no menu bar, no value edit) the list append below is bookkeeping no
       resolver will read, so skip it.  Everything below is inert anyway then (no cursor means
       is_cur can never hit).  Type-ahead candidates still enter via nav_item_stamp_label, and
       the first Tab/arrow sets nav.active so the NEXT emission registers fully and the
       first-focus recovery lands the cursor -- one frame, the lag nav already runs on. */
    if ( !g_ctx->nav.reg_all )
        return;

    bool is_cur = ( id == g_ctx->nav.id );
    if ( is_cur )
        g_ctx->nav.id_seen = true;

    /* Append to the nav item list (emission order == Tab order).  A layout-placed item carries
       the region/line coordinate cell_next_w latched; anything interacting without a
       layout cell -- title-bar buttons, dock tabs -- lists as chrome, the F6 lane: Tab and the
       body arrows skip it, F6 hops onto the strip and Left/Right walk it (gui_nav.c).
       Scrollbars and drag strips never reach here at all (s_scope.nav.skip). */

    bool placed = s_scope.nav.placed;
    if ( placed && g_ctx->nav.first_item == GUI_ID_NONE )
        g_ctx->nav.first_item = id;   /* first-focus / recovery landing spot */

    if ( g_ctx->nav.item_count >= GUI_NAV_ITEMS_MAX )
    {
        /* List full: items past the cap never register, so Tab / arrows silently cannot reach
           them.  Warn so a keyboard dead zone in a huge window traces to this cap (and to
           rows_clip as the usual fix) instead of reading as a nav bug. */
        GUI_WARN_ONCE( "nav item list full (%u) -- further items are unreachable by "
                       "keyboard nav this frame. Virtualize rows (rows_clip) or raise "
                       "GUI_NAV_ITEMS_MAX (gui_internal.h).\n", (unsigned)GUI_NAV_ITEMS_MAX );
    }
    else
    {
        gui_nav_item_t* it = &g_ctx->nav.items[ g_ctx->nav.item_count++ ];
        it->id       = id;
        it->rect     = r;
        it->region   = placed ? s_scope.nav.region : 0;
        it->line     = placed ? s_scope.nav.line   : 0;
        it->chrome    = !placed;
        it->drag_kind = ( kind == ITEM_DRAG );
        it->label[0]  = 0;   /* type-ahead opt-in: nav_item_stamp_label fills it in, if called */
    }

    /* Current item: draw the outline ring whenever a nav cursor exists (even in mouse mode, so it
       keeps its location), and -- only while the keyboard is the active instrument (nav_highlight)
       -- give it the fill (st->nav, read by col_item_bg / col_frame_bg) and apply a pending
       activation.  The ring is drawn before the widget's own background (item_state runs
       first), inset outward by NAV_RING so the fill leaves the border visible.

       LAYERING NOTE: the ring is invoked from here because it is a system adornment that must
       be uniform across every widget -- stock and custom alike -- and must paint beneath the
       item's own fill, and no single presentation seam exists after behavior that every widget
       passes through.  The paint itself (color, thickness, extent) is draw_nav_ring in
       element/gui_adornment.c; behavior only picks the moment.  Do not add style reads or
       raw draws to this tier. */

    if ( is_cur && g_ctx->nav.active )
    {
        /* Fresh adoption: scroll the item into view (once).  Only a placed item chases -- its
           region stack is the one open right now; chrome sits outside the scrolling content. */
        if ( g_ctx->nav.scroll_chase )
        {
            g_ctx->nav.scroll_chase = false;
            if ( placed )
                nav_scroll_chase( r );
        }

        bool captured = ( g_ctx->nav.edit_id == id || g_ctx->nav.solo_drag_id == id );
        draw_nav_ring( r, captured );

        if ( g_ctx->nav.highlight )
        {
            st->nav = true;
            if ( g_ctx->nav.activate )
            {
                if ( kind == ITEM_DRAG )
                {
                    /* A value widget (slider, drag box) does not click -- activation captures it
                       for keyboard editing: Left/Right then step the value (st->nav_adjust below)
                       until Enter/Space/Esc or a mouse press releases (gui_nav.c). */
                    g_ctx->nav.edit_id = id;
                }
                else
                {
                    st->pressed = st->clicked = true;
                    if ( kind == ITEM_FOCUSABLE )
                        s_interaction.focused_id = id;  /* Enter on an input box -> enter text capture */
                }

                /* Consume the activating keys + any text so the item just focused does not also see
                   this frame's Enter (instant blur) or type the activating Space. */

                g_ctx->nav.activate = false;
                key_claim( APP_KEY_ENTER );
                key_claim( APP_KEY_SPACE );
                s_io.text[ 0 ] = '\0';
            }
        }

        /* Captured for value edit -- explicitly (edit_id, via Enter/Space) or implicitly (solo_drag_id,
           a lone DRAG widget on its row -- see gui_nav.c): keep the fill on (even if a mouse move
           dropped nav_highlight) and hand the widget this frame's arrow step to apply to its value.
           st->focused mirrors the FOCUSABLE meaning ("owns keyboard input right now") onto DRAG
           widgets, so slider/drag presentation can reuse the same focused-border convention as
           text/numeric fields (gui_input.c, gui_widget_numeric.c) instead of inventing a second
           visual language. */
        if ( captured )
        {
            st->nav        = true;
            st->nav_adjust = g_ctx->nav.edit_dir;
            st->focused    = true;
        }
    }
}

/* Type-ahead opt-in: called right after item_state by a list-y widget (gui_selectable) to
   stamp its label onto the nav item entry that call just registered, so gui_nav.c's type-ahead
   resolver can prefix-match it.  A no-op if the item did not register this frame (wrong nav
   window, nav_skip) or GUI_ITEM_NO_TYPEAHEAD opted it out -- its label stays "" (nav_item_register
   already cleared it), which the resolver skips. */
void
nav_item_stamp_label( gui_id_t id, const char* label )
{
    if ( s_scope.flags & GUI_ITEM_NO_TYPEAHEAD ) return;

    /* Dormant-frame append: nav_item_register skipped this item (keyboard disengaged), but a
       LABELED item is a type-ahead candidate and type-ahead must be able to engage nav from
       cold -- so the labeled subset always registers.  Mirror the item_state early-outs that
       would have prevented the registration (disabled / deaf / replay / wrong nav window). */
    if ( !g_ctx->nav.reg_all )
    {
        if ( s_scope.win != g_ctx->nav.win ) return;
        if ( ( s_scope.flags & GUI_ITEM_DISABLED ) || !g_ctx->listening || s_replay_mode ) return;
        if ( g_ctx->nav.item_count >= GUI_NAV_ITEMS_MAX ) return;

        gui_nav_item_t* nit = &g_ctx->nav.items[ g_ctx->nav.item_count++ ];
        nit->id        = id;
        nit->rect      = s_scope.last_rect;
        nit->region    = s_scope.nav.placed ? s_scope.nav.region : 0;
        nit->line      = s_scope.nav.placed ? s_scope.nav.line   : 0;
        nit->chrome    = !s_scope.nav.placed;
        nit->drag_kind = false;   /* a labeled selectable is never an ITEM_DRAG widget */
        nit->label[ 0 ] = 0;      /* filled below */
    }

    if ( g_ctx->nav.item_count == 0 ) return;

    gui_nav_item_t* it = &g_ctx->nav.items[ g_ctx->nav.item_count - 1 ];
    if ( it->id != id ) return;

    u32 n = 0;
    for ( ; label[ n ] && n < sizeof( it->label ) - 1; ++n )
        it->label[ n ] = ( label[ n ] >= 'A' && label[ n ] <= 'Z' ) ? (char)( label[ n ] + 32 ) : label[ n ];
    it->label[ n ] = 0;
}

/* Programmatic focus request (public: gui()->set_keyboard_focus).  Latched until the next
   focusable widget passes through item_state, which takes keyboard focus as if clicked.
   Persisting across frames is deliberate: a request queued after this frame's field has
   already emitted lands on that field next frame. */

static bool s_focus_request = false;

void
gui_set_keyboard_focus( void )
{
    s_focus_request = true;
}

/* Focus confinement -- the CONFINE half of the exclusive input mode (see gui_modal_scope_live in
   core/gui_ctx.c).  While a GUI_WIN_MODAL window is live, only its own widgets may take keyboard
   focus: no background window can steal it, exactly as a held active_id denies interaction to
   every other item during a drag.  This is what lets an exclusive window (the dev console) seat
   focus once, as an event, instead of re-stealing it every frame.  The HOLD half (sticky focus,
   "cannot select nothing") lives in interaction_frame_reset. */

static bool
focus_allowed( gui_id_t win )
{
    return !gui_modal_scope_live() || win == g_ctx->modal.win_id;
}

/* Drop keyboard/text focus entirely (Enter commit, Escape revert).  The arbitration verb for
   ending a focus capture -- widgets call this instead of writing the interaction record raw;
   the record itself stays owned by core/gui_ctx.c.

   Exclusive input mode -- the HOLD rule's second half.  Focus can fall to nothing two ways: a
   press on dead space (guarded in interaction_frame_reset) and a widget releasing its own capture
   here (Enter submit / Escape).  A game menu honors neither: it always keeps a selection.  So
   while the live exclusive mode owns the focused widget, this release is a no-op -- the console
   input keeps its caret after every command instead of going dead until the next click.  A focus
   MOVE (clicking / tabbing to another field) never comes through here; it overwrites focused_id
   directly in item_state, so navigation within the mode still works. */
void
item_focus_release( void )
{
    if ( gui_modal_scope_live() && s_interaction.focused_win == g_ctx->modal.win_id )
        return;
    s_interaction.focused_id = GUI_ID_NONE;
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
       widget emitted after it -- the keyboard twin of click-to-focus below.  Skipped for a
       window the exclusive fence denies, so the request stays latched and lands on the modal
       window's own input rather than a background field emitted earlier in the frame. */
    if ( s_focus_request && kind == ITEM_FOCUSABLE && focus_allowed( s_scope.win ) )
    {
        s_focus_request          = false;
        s_interaction.focused_id = id;
    }

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_interaction.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_interaction.active_id = id;
        st.pressed      = true;
        if ( kind == ITEM_FOCUSABLE && focus_allowed( s_scope.win ) )
            s_interaction.focused_id = id;

        /* Keep the nav ring synced to the last interacted item: a click moves the cursor here, so
           resuming the keyboard later continues from what was clicked (only once a ring exists). */

        if ( g_ctx->nav.active )
            g_ctx->nav.id = id;
    }

    st.hover   = ( s_interaction.hover_id == id );
    st.active  = ( s_interaction.active_id == id );
    st.focused = ( s_interaction.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_interaction.hover_id == id && s_interaction.active_id == id;

    /* Record which window owns the keyboard focus, for the exclusive-scope focus lock (a modal
       input mode holds its focus sticky; see interaction_frame_reset).  Refreshed every frame the
       focused widget re-emits, so it always names that widget's current window. */
    if ( st.focused )
        s_interaction.focused_win = s_scope.win;

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
    {
        if ( eligible ) {
            gui_rect_t vis = rect_intersect( r, s_scope.clip );
            if ( vis.w > 0.0f && vis.h > 0.0f )
                 DBG_WIDGET( id, vis, st.hover, st.active );
        }
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
    Compound-widget bracket -- see the contract comment in gui_internal.h.

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
    Gate predicates -- the interaction questions every gesture gate asks, named once.

    These are the read half of the arbitration state above: pure queries, no writes, so any
    tier may call them (declared in gui_internal.h for the files included before this one).
    They exist so a compound gesture gate reads as a sentence at the call site instead of a
    chain of raw field comparisons.
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

/* Claim the pointer capture for `id` (held by `button`) -- the write half of the pair above.
   The one sanctioned door for a HIGHER tier to start a press-drag gesture: core/ and
   interact/ are the only raw writers of the arbitration fields, so chrome-side gestures
   (window text selection's fallback press) claim through this verb instead of poking
   s_interaction directly.  Release is global, as for every gesture: the frame reset drops
   active_id when `button` lifts. */
void
interact_claim( gui_id_t id, u8 button )
{
    s_interaction.active_id     = id;
    s_interaction.active_button = button;
}

/* The cursor is over window `win_id`'s bare surface: it is the front-most window under the
   cursor AND no widget sits under the cursor -- the gate for chrome gestures (title-bar drag,
   double-click collapse, context-menu press) that must yield to any widget above them. */
bool
interact_hover_bare( gui_id_t win_id )
{
    return s_interaction.hover_win == win_id && s_interaction.hover_id == GUI_ID_NONE;
}

/* Point the hover-window arbitration at `owner`, making every OTHER window inert for the rest
   of this frame: item_state gates all hover on s_build.win.id == hover_win, so redirecting
   hover_win freezes everything behind the owner with no per-widget code -- the window-scale
   analogue of active_id drag-modality.  The verb behind the popup modal fence
   (popup_apply_modal); exists so this tier stays the only writer of s_interaction. */
void
interact_hover_fence( gui_id_t owner )
{
    s_interaction.hover_win = owner;
}


// clang-format on
/*============================================================================================*/
