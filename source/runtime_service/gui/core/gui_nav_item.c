/*==============================================================================================

    runtime_service/gui/core/gui_nav_item.c -- The keyboard-nav PER-ITEM seam.

    Keyboard nav is two halves at two layers, and this is the lower one:

      register (here, core)      -- every item of the nav window records itself into the frame's
                                    nav item list as it emits, carrying the structural region /
                                    line stamp the layout engine latched when it placed it.  The
                                    current item also lights its focus ring and takes a pending
                                    activation as a synthesized click.
      resolve (chrome/nav/gui_nav.c) -- reads that list and decides where the cursor goes: Tab
                                    order, directional moves as index math, type-ahead, the menu
                                    bar / F6 chrome lane.  Policy, one frame behind the emit that
                                    built the list, exactly as hover_win lags the cursor.

    The split is a layering fact, not a preference: item_state (core) must call the registration
    on every item, and core cannot call up into chrome.  The state both halves share is
    g_ctx->nav (gui_nav_state_t, core/gui_ctx.h).

    This is the keyboard mirror of the mouse hit-test in item_state: one seam every widget
    already passes through, so a widget needs no per-widget nav code.

    LAYERING NOTE: nav_item_register invokes the present-tier focus ring (draw_nav_ring,
    stock/gui_adornment.c) and the composer's scroll chase (nav_scroll_chase, flow/gui_scroll.c)
    -- behavior decides WHEN a system adornment paints or a region scrolls; the policy lives with
    the skin / the composer.  Both are declared in this server's upward-seams block
    (core/gui_core.h); see the comments at the call sites.

    Included by gui_core.c before core/gui_item.c, its one caller.

==============================================================================================*/
// clang-format off

/* Record one item into the frame's nav list and, for the item under the nav cursor, apply the
   cursor's effects (ring, fill, activation).  Called from item_state for every item that belongs
   to the nav window (s_scope.win == g_ctx->nav.win) and was not opted out (s_scope.nav.skip). */

void
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
    if ( !placed && g_ctx->nav.first_chrome == GUI_ID_NONE )
        g_ctx->nav.first_chrome = id;   /* recovery fallback: a window with only chrome items
                                            (a minimized shelf chip) still needs a landing spot */

    if ( g_ctx->nav.item_count >= GUI_NAV_ITEMS_MAX )
    {
        /* List full: items past the cap never register, so Tab / arrows silently cannot reach
           them.  Warn so a keyboard dead zone in a huge window traces to this cap (and to
           rows_clip as the usual fix) instead of reading as a nav bug. */
        GUI_WARN_ONCE( "nav item list full (%u) -- further items are unreachable by "
                       "keyboard nav this frame. Virtualize rows (rows_clip) or raise "
                       "GUI_NAV_ITEMS_MAX (core/gui_ctx.h).\n", (unsigned)GUI_NAV_ITEMS_MAX );
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

    /* Current item: mark the cursor ring whenever a nav cursor exists (even in mouse mode, so it
       keeps its location), and -- only while the keyboard is the active instrument (nav_highlight)
       -- give it the fill (st->nav, read by col_item_bg / col_frame_bg) and apply a pending
       activation.

       LAYERING NOTE: the ring is marked from here because it is a system adornment that must be
       uniform across every widget -- stock and custom alike -- and no single presentation seam
       exists after behavior that every widget passes through.  Everything else is the skin's:
       ring_mark_nav records, and rings_paint lays the ring down at the end of the window body
       (stock/gui_adornment.c).  Do not add style reads or raw draws to this tier. */

    if ( is_cur && g_ctx->nav.active )
    {
        /* Fresh adoption: scroll the item into view (once).  Only a placed item chases -- its
           region stack is the one open right now; chrome sits outside the scrolling content.
           The walk itself is composition machinery, so it lives with flow (nav_scroll_chase,
           flow/gui_scroll.c) and this tier only picks the MOMENT -- the same split as the ring. */
        if ( g_ctx->nav.scroll_chase )
        {
            g_ctx->nav.scroll_chase = false;
            if ( placed )
                nav_scroll_chase( r );
        }

        bool captured = ( g_ctx->nav.edit_id == id || g_ctx->nav.solo_drag_id == id );
        ring_mark_nav( r, captured );

        if ( g_ctx->nav.highlight )
        {
            st->nav = true;
            if ( g_ctx->nav.activate )
            {
                if ( kind == ITEM_DRAG )
                {
                    /* A value widget (slider, drag box) does not click -- activation captures it
                       for keyboard editing: Left/Right then step the value (st->nav_adjust below)
                       until Enter/Space/Esc or a mouse press releases (gui_nav.c).  A drag box
                       promotes this fresh capture straight into its text-entry mode
                       (gui_widget_slider.c), input-field parity. */
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

// clang-format on
/*============================================================================================*/
