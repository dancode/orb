/*==============================================================================================

    runtime_service/gui/interact/gui_item.c -- The standard item protocol.

    widget_behavior: the default COMPOSITION of the interaction services, run once per item
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
    record; this file is the service that arbitrates it.

    LAYERING NOTE: the nav focus ring drawn in nav_item_register (WIN_BORDER / COL_NAV) is the
    one sanctioned presentation act inside the interaction tier -- see the comment at that site.

    Included by gui.c after core/gui_widget_core.c (WIN_BORDER / COL_NAV macros for the ring)
    and before every consumer (compose/ scrollbars, widgets/, window/ chrome, dock/, table/).

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
widget_repeat_tick( bool pressed )
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

/* Keyboard-nav per-item seam.  Called from widget_behavior for every item that belongs to the nav
   window (s_build.win_id == s_nav.win), the keyboard mirror of the hover hit-test above.  It does
   three things: records the item into the frame's nav list (with the structural region/line stamp
   the layout engine latched when it placed it -- gui_nav.c resolves the next move as index math
   over this list), notes whether the nav cursor was seen, and -- for the current nav item --
   lights the focus ring and synthesizes a click from an Enter/Space activation so every widget
   activates from the keyboard with no per-widget code, exactly as a mouse click flows through
   st.clicked. */

#define NAV_RING 2.0f    /* focus-ring inset outside the item rect so the item's own fill spares it */

/* Bring the nav cursor's rect into view -- the keyboard analogue of the wheel.  Runs once, on the
   frame the cursor was adopted (s_nav.scroll_chase), for the layout-placed cursor item: walk the
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

        f32 vx0 = f->outer.x + WIN_BORDER;   /* view box: the same gutter-adjusted extents the */
        f32 vy0 = f->outer.y;                /* region's own scrollbars are sized against      */

        /* Overshoot per axis: pull the near edge in (top-aligning an item taller than the view),
           else the far edge.  Clamped into the scroll range measured last frame, so a chase can
           never scroll past the content -- the same clamp the wheel applies. */
        f32 dy = 0.0f;
        if ( r.h > f->view_h - 2.0f * pad || r.y < vy0 + pad )
            dy = r.y - ( vy0 + pad );
        else if ( r.y + r.h > vy0 + f->view_h - pad )
            dy = ( r.y + r.h ) - ( vy0 + f->view_h - pad );

        f32 max_y  = f->scroll->content_h - f->view_h;
        if ( max_y < 0.0f ) max_y = 0.0f;
        f32 want_y = clampf( f->scroll->scroll_y + dy, 0.0f, max_y );
        dy = want_y - f->scroll->scroll_y;

        f32 dx = 0.0f;
        if ( r.w > f->view_w - 2.0f * pad || r.x < vx0 + pad )
            dx = r.x - ( vx0 + pad );
        else if ( r.x + r.w > vx0 + f->view_w - pad )
            dx = ( r.x + r.w ) - ( vx0 + f->view_w - pad );

        f32 max_x  = f->scroll->content_w - f->view_w;
        if ( max_x < 0.0f ) max_x = 0.0f;
        f32 want_x = clampf( f->scroll->scroll_x + dx, 0.0f, max_x );
        dx = want_x - f->scroll->scroll_x;

        if ( dy != 0.0f || dx != 0.0f )
        {
            f->scroll->scroll_y = want_y;
            f->scroll->scroll_x = want_x;
            r.y -= dy;   /* where the item lands next frame -- the ancestors check that position */
            r.x -= dx;
            s_retained.wants_redraw = true;
        }
    }
}

static void
nav_item_register( gui_id_t id, gui_rect_t r, widget_state_t* st, widget_kind_t kind )
{
    bool is_cur = ( id == s_nav.id );
    if ( is_cur )
        s_nav.id_seen = true;

    /* Append to the nav item list (emission order == Tab order).  A layout-placed item carries
       the region/line coordinate widget_next_rect_w latched; anything interacting without a
       layout cell -- title-bar buttons, dock tabs -- lists as chrome, the F6 lane: Tab and the
       body arrows skip it, F6 hops onto the strip and Left/Right walk it (gui_nav.c).
       Scrollbars and drag strips never reach here at all (s_build.nav_skip). */

    bool placed = s_build.nav_item_placed;
    if ( placed && s_nav.first_item == GUI_ID_NONE )
        s_nav.first_item = id;   /* first-focus / recovery landing spot */

    if ( s_nav.item_count < GUI_NAV_ITEMS_MAX )
    {
        gui_nav_item_t* it = &s_nav.items[ s_nav.item_count++ ];
        it->id     = id;
        it->rect   = r;
        it->region = placed ? s_build.nav_item_region : 0;
        it->line   = placed ? s_build.nav_item_line   : 0;
        it->chrome = !placed;
    }

    /* Current item: draw the outline ring whenever a nav cursor exists (even in mouse mode, so it
       keeps its location), and -- only while the keyboard is the active instrument (nav_highlight)
       -- give it the fill (st->nav, read by widget_bg_color / frame_bg_color) and apply a pending
       activation.  The ring is drawn before the widget's own background (widget_behavior runs
       first), inset outward by NAV_RING so the fill leaves the border visible.

       LAYERING NOTE: this ring draw (WIN_BORDER / COL_NAV) is the one sanctioned presentation act
       inside the behavior tier.  Behavior otherwise consumes finished rects and never reads a
       style value; the ring stays here because it is a system adornment that must be uniform
       across every widget -- stock and custom alike -- and must paint beneath the item's own
       fill, and no single presentation seam exists after behavior that every widget passes
       through.  Do not add further style reads to this tier. */

    if ( is_cur && s_nav.active )
    {
        /* Fresh adoption: scroll the item into view (once).  Only a placed item chases -- its
           region stack is the one open right now; chrome sits outside the scrolling content. */
        if ( s_nav.scroll_chase )
        {
            s_nav.scroll_chase = false;
            if ( placed )
                nav_scroll_chase( r );
        }

        draw_push_rect_outline( r.x - NAV_RING, r.y - NAV_RING,
                                r.w + 2.0f * NAV_RING, r.h + 2.0f * NAV_RING,
                                WIN_BORDER, 0, COL_NAV );

        if ( s_nav.highlight )
        {
            st->nav = true;
            if ( s_nav.activate )
            {
                if ( kind == WIDGET_KIND_DRAG )
                {
                    /* A value widget (slider, drag box) does not click -- activation captures it
                       for keyboard editing: Left/Right then step the value (st->nav_adjust below)
                       until Enter/Space/Esc or a mouse press releases (gui_nav.c). */
                    s_nav.edit_id = id;
                }
                else
                {
                    st->pressed = st->clicked = true;
                    if ( kind == WIDGET_KIND_FOCUSABLE )
                        s_interaction.focused_id = id;  /* Enter on an input box -> enter text capture */
                }

                /* Consume the activating keys + any text so the item just focused does not also see
                   this frame's Enter (instant blur) or type the activating Space. */

                s_nav.activate = false;
                s_io.keys_pressed[ APP_KEY_ENTER ] = false;
                s_io.keys_pressed[ APP_KEY_SPACE ] = false;
                s_io.text[ 0 ] = '\0';
            }
        }

        /* Captured for value edit: keep the fill on (even if a mouse move dropped nav_highlight)
           and hand the widget this frame's arrow step to apply to its value. */
        if ( s_nav.edit_id == id )
        {
            st->nav        = true;
            st->nav_adjust = s_nav.edit_dir;
        }
    }
}

/* Programmatic focus request (public: gui()->set_keyboard_focus).  Latched until the next
   focusable widget passes through widget_behavior, which takes keyboard focus as if clicked.
   Persisting across frames is deliberate: a request queued after this frame's field has
   already emitted lands on that field next frame. */

static bool s_focus_request = false;

void
gui_set_keyboard_focus( void )
{
    s_focus_request = true;
}

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

static widget_state_t
widget_behavior( gui_id_t id, gui_rect_t r, widget_kind_t kind )
{
    widget_state_t st = { 0 };

    /* Latch the most recent item id for context menus / tooltips (popup_context_item_begin,
       set_item_tooltip).  Done before the disabled early-out so a disabled widget still counts
       as the last item -- the anchor is "what was just emitted", regardless of its state. */

    s_build.last_item_id   = id;
    s_build.last_item_rect = r;             /* item-query getters read this for "the widget just emitted" */

    /* Consume the one-shot nav opt-out here, before any early-out below can leak it onto the
       next widget.  A flagged item (scrollbar, drag strip) still interacts normally with the
       mouse; it just never registers as a keyboard target. */
    bool nav_skip    = s_build.nav_skip;
    s_build.nav_skip = false;

    /* Disabled item: inert this frame -- no hover, active, focus, or click.  Returning the zeroed
       state here is the one place that suppresses interaction for every widget, the behavioral half
       of GUI_ITEM_DISABLED (the visual dim is the draw list's global alpha, set at resolve).  The
       flags were latched by widget_next_rect_w just before this call. */
    if ( s_build.cur_item_flags & GUI_ITEM_DISABLED )
    {
        s_build.last_item_status = st;      /* a disabled item is still the last item, reported inert */
        return st;
    }

    /* Deaf context: not listening this frame -- render but return inert state.
       last_item_id/rect are latched above so item-query calls still work. */

    if ( !g_ctx->listening )
    {
        s_build.last_item_status = st;
        return st;
    }

    /* Volatile-callback replay -- full feature in widgets/gui_volatile.c and
       backend/pipeline/gui_build_volatile.c; s_replay_mode itself is declared in gui_ctx.c.  Report the
       ambient hover/active/focused state as-is, but never touch it and never hit-test -- s_build
       (win_id, cur_item_flags, win_resize_hot, ...) is only meaningful between a real
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

    bool can_hover = ( s_interaction.active_id == GUI_ID_NONE || s_interaction.active_id == id );
    bool win_hover = ( s_build.win_id == s_interaction.hover_win );
    bool eligible  = can_hover && win_hover && !s_build.win_resize_hot && !s_build.win_grip_hot;

    /* While the keyboard is the active nav instrument (nav_highlight), the mouse does not set hover:
       the fill is mutually exclusive, so a mouse-hovered item never fills alongside the nav item
       (the nav ring still shows its location).  A mouse move or click drops nav_highlight
       (gui_nav.c), re-enabling hover that same frame. */

    if ( eligible && !s_nav.highlight && rect_hit( s_build.clip_rect ) && rect_hit( r ) )
         s_interaction.hover_id = id;

    /* Programmatic focus: a queued set_keyboard_focus request lands on the first focusable
       widget emitted after it -- the keyboard twin of click-to-focus below. */
    if ( s_focus_request && kind == WIDGET_KIND_FOCUSABLE )
    {
        s_focus_request          = false;
        s_interaction.focused_id = id;
    }

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_interaction.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_interaction.active_id = id;
        st.pressed      = true;
        if ( kind == WIDGET_KIND_FOCUSABLE )
            s_interaction.focused_id = id;

        /* Keep the nav ring synced to the last interacted item: a click moves the cursor here, so
           resuming the keyboard later continues from what was clicked (only once a ring exists). */

        if ( s_nav.active )
            s_nav.id = id;
    }

    st.hover   = ( s_interaction.hover_id == id );
    st.active  = ( s_interaction.active_id == id );
    st.focused = ( s_interaction.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_interaction.hover_id == id && s_interaction.active_id == id;

    /* Keyboard nav: an item in the nav window registers as a candidate and, if it is the nav
       cursor, takes a synthesized click from an Enter/Space activation -- the keyboard mirror of
       the mouse hit-test above, through the same one seam every widget already passes through. */

    if ( s_build.win_id == s_nav.win && !nav_skip )
        nav_item_register( id, r, &st, kind );

    /* Auto-repeat (GUI_ITEM_BUTTON_REPEAT): while held with the cursor still over it, fire on the
       press then repeatedly on the timed cadence -- replacing the release-click for this widget.
       Gated on the cursor being over it so sliding off pauses the repeat, like a real spin button. */

    if ( ( s_build.cur_item_flags & GUI_ITEM_BUTTON_REPEAT ) && st.active && s_interaction.hover_id == id )
        st.clicked = widget_repeat_tick( st.pressed );

    /* Debug overlay: every interactive widget passes through here, so this one site captures
       the hit rects -- tinted by hover/active so the live interaction is visible.  Capture the
       *visible* rect (the widget clipped to the active region clip): a row scrolled fully
       outside its child box has an empty intersection and is not hit-testable, so it is dropped
       from the overlay too, rather than drawing an interaction rect outside the clip box. */

#ifdef GUI_DEBUG_OVERLAY
    {
        if ( eligible ) {
            gui_rect_t vis = rect_intersect( r, s_build.clip_rect );
            if ( vis.w > 0.0f && vis.h > 0.0f )
                 DBG_WIDGET( id, vis, st.hover, st.active );
        }
    }
#endif

    s_build.last_item_status = st;   /* publish the resolved state for the item-query readers */
    return st;
}


// clang-format on
/*============================================================================================*/
