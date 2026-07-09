/*==============================================================================================

    runtime_service/gui/4_window/gui_window_end.c -- Deferred window chrome (gui_window_end).

    The symmetric partner to window_begin_ex / window_begin_docked (gui_widget_window.c /
    gui_window_docked.c): closes the body scroll region, then paints titlebar, collapse arrow,
    close/detach buttons, native caption buttons, border, and the resize grip -- all deferred so
    they overdraw any content that scrolled beneath them while staying inside the window's one
    draw command.  Also resolves the window move-grab and the drag-to-dock commit, both decided
    only after this window's own widgets have run.

    Split out of gui_widget_window.c because chrome painting (this file) is a distinct concern
    from geometry/gesture resolution (window_begin_ex): the two halves meet only through the
    s_build.win / s_scope handoff window_begin_ex or window_begin_docked commits before the
    caller's body widgets run, and the window_min_w/_h / window_fit_bounds / window_fit_size
    helpers gui_window_end reuses for the auto-fit grip -- all defined in gui_widget_window.c,
    included just before this file.

    Included by gui.c right after gui_widget_window.c.

==============================================================================================*/
// clang-format off

void
gui_window_end( void )
{
    /* Closed CLOSEABLE window: window_begin emitted nothing (no region, no clip, no chrome), so
       there is nothing to balance here -- just consume the latch and return. */
    if ( s_build.win.hidden )
    {
        s_build.win.hidden = false;
        return;
    }

    gui_window_t* win = s_build.win.rec;

    /* Docked window: the node owns chrome, not the free-float path below.  An inactive tab opened
       nothing (begin returned false), so there is nothing to close; the active tab closes its body
       region, draws the tab strip + node border, and balances its clip.  Either way, restore the
       ambient draw state and return before the free-float chrome runs. */
    if ( s_build.win.dock_node )
    {
        gui_dock_node_t* node = s_build.win.dock_node;
        s_build.win.dock_node = NULL;   /* consume for the next window */

        if ( s_build.win.dock_active )
        {
            item_flags_chrome_reset();
            layout_pop_region();        /* measure content, draw scrollbars, pop the inner clip */
            dock_window_chrome( node ); /* tab strip + tabs + node border, under the window clip */
            draw_pop_clip_rect();       /* balance the clip pushed in window_begin_docked */
        }

        draw_set_window( 0 );
        draw_set_sort_key( 0 );
        draw_set_viewport( 0 );
        draw_set_band( 0 );
        draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
        return;
    }

    /* Same native test window_begin used (flag or owned floater): a native window's titlebar/border
       is the OS frame, so its collapse arrow, detach button and gui drag-grab are all suppressed. */
    bool native     = window_is_native( win, s_build.win.flags );
    bool frame_only = ( s_build.win.flags & GUI_WIN_NATIVE ) != 0;

    /* Chrome below (scrollbars via layout_pop_region, collapse arrow, border, size grip) is not an
       item.  layout_pop_region resets too, but a collapsed window opens no region and skips it, so
       reset here to cover that case and the deferred chrome either way. */
    item_flags_chrome_reset();

    /* Close the body scroll region (expanded only -- a collapsed window opened none).  This
       measures the content extent, pops the inner content clip, draws the scrollbars, and
       handles the wheel, all via the shared layout engine.  A collapsed window measures
       nothing and keeps its extents from the last expanded frame, so scroll survives collapse.
       Bars are drawn here, before the chrome (so the border frames them) and before the window
       drag-grab below (so a press on a knob claims active_id and the window does not drag). */
    if ( !s_build.win.collapsed )
        layout_pop_region();

    /* Deferred chrome: titlebar, collapse arrow, title text, and border paint last under the
       outer window clip, so they overdraw any content that scrolled beneath them while still
       merging into the one window draw command.  A NOTITLEBAR window (title_h 0) skips the bar
       entirely and keeps only the border. */
    if ( s_build.win.title_h > 0.0f )
    {
        f32 title_h = s_build.win.title_h;
        draw_push_rect_filled( s_build.win.x, s_build.win.y, s_build.win.w, title_h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_TITLE_BG );

        /* Collapse toggle: a triangle in a title-bar-height square at the bar's left edge.  A
           click flips win->collapsed, taking effect next frame like the drag grab.  Claiming
           hover/active here also keeps the title-bar drag grab below from firing on the same
           press.  Omitted (and the title slides left to the padding) when NOCOLLAPSE is set.
           The icon is drawn from this frame's state so it matches the body shown this frame. */
        f32 text_x = s_build.win.x + WIDGET_PAD;
        if ( !( s_build.win.flags & GUI_WIN_NOCOLLAPSE ) && !native )
        {
            gui_rect_t   arrow_r  = { s_build.win.x, s_build.win.y, title_h, title_h };
            gui_id_t     arrow_id = id_combine( s_build.win.id, GUI_COLLAPSE_SALT );
            gui_item_state_t arrow_st = widget_behavior( arrow_id, arrow_r, GUI_WIDGET_KIND_BUTTON );
            if ( arrow_st.clicked )
            {
                win->collapsed = !win->collapsed;
                g_ctx->retained.wants_redraw = true;  /* toggle takes effect next frame; force one more build */
            }
            draw_collapse_arrow( arrow_r, s_build.win.collapsed, arrow_st.hover ? COL_TEXT : COL_TEXT_DIM );
            text_x = s_build.win.x + title_h;   /* title follows the arrow square */

            /* Double-click anywhere on the bar (but not the arrow, which hovers, nor a hot
               resize edge) does the same toggle -- the familiar "double-click titlebar to
               collapse" gesture.  hover_id == NONE excludes the arrow; resize_hot excludes
               the edges; the toggle lands next frame like the arrow click and the drag grab. */
            gui_rect_t bar_r = { s_build.win.x, s_build.win.y, s_build.win.w, title_h };
            if ( s_io.mouse_double[ 0 ] && !s_scope.resize_hot
                 && s_build.win.id == s_interaction.hover_win && s_interaction.hover_id == GUI_ID_NONE
                 && rect_hit( bar_r ) )
            {
                win->collapsed = !win->collapsed;
                g_ctx->retained.wants_redraw = true;
            }
        }

        /* Right-edge title-bar buttons march leftward from the bar's right edge: the close (X)
           first (outermost) when CLOSEABLE, then the detach / reattach box.  btn_x tracks the next
           free slot; right_limit follows it so the title text always stops clear of the buttons.
           Native windows use their OS caption buttons instead (drawn below), so both are skipped. */
        f32 right_limit = s_build.win.x + s_build.win.w - WIDGET_PAD;
        f32 btn_x       = s_build.win.x + s_build.win.w;

        /* Close button: an X that hides the window.  A click sets win->closed, taking effect next
           frame like the collapse toggle; from then on window_begin returns false and the window
           draws nothing until the host re-opens it (window_set_open).  Suppressed for native
           windows, which get the OS close caption button. */
        if ( ( s_build.win.flags & GUI_WIN_CLOSEABLE ) && !native )
        {
            btn_x -= title_h;
            gui_rect_t   cl_r  = { btn_x, s_build.win.y, title_h, title_h };
            gui_id_t     cl_id = id_combine( s_build.win.id, GUI_CLOSE_SALT );
            gui_item_state_t cl_st = widget_behavior( cl_id, cl_r, GUI_WIDGET_KIND_BUTTON );

            /* Hover/press background so the control reads as clickable (the glyph stays square). */
            if ( cl_st.hover || cl_st.active )
            {
                draw_set_rounding( ROUND_WIDGET );
                draw_push_rect_filled( cl_r.x, cl_r.y, cl_r.w, cl_r.h, 0, 0, 1, 1, 0, COL_WIDGET_HOT );
            }
            draw_close_x( cl_r, cl_st.hover ? COL_TEXT : COL_TEXT_DIM );

            if ( cl_st.clicked && win )
            {
                win->closed = true;
                g_ctx->retained.wants_redraw = true;  /* close takes effect next frame; force one more build */
            }

            right_limit = cl_r.x - WIDGET_PAD;
        }

        /* Detach / reattach button: a square at the title bar's right edge that pops the window out
           into its own OS window (when it sits on the main surface) or back into the main surface
           (when it is floating).  Movable windows only -- NOMOVE (popups, modals, fixed panels)
           never show it.  Mirrors the collapse arrow on the left: claiming hover/active here keeps
           the title-bar drag and double-click-collapse from also firing on the same press. */
        if ( !( s_build.win.flags & GUI_WIN_NOMOVE ) && !( s_build.win.flags & GUI_WIN_NO_DETACH )
             && !native )
        {
            btn_x -= title_h;
            gui_rect_t   det_r  = { btn_x, s_build.win.y, title_h, title_h };
            gui_id_t     det_id = id_combine( s_build.win.id, GUI_DETACH_SALT );
            gui_item_state_t det_st = widget_behavior( det_id, det_r, GUI_WIDGET_KIND_BUTTON );
            if ( det_st.clicked )
                vp_request_button( win );   /* 0 = main surface -> tear off; else floater -> merge back */

            /* Icon: an outlined box when docked (click to pop out), a filled box when floating
               (click to dock back in). */
            bool attached = !win || win->viewport == 0;
            u32  icol     = det_st.hover ? COL_TEXT : COL_TEXT_DIM;
            f32  isz      = title_h * 0.42f;
            f32  ix       = det_r.x + ( det_r.w - isz ) * 0.5f;
            f32  iy       = det_r.y + ( det_r.h - isz ) * 0.5f;
            f32 det_save = draw_rounding();
            draw_set_rounding( 0.0f );   /* small box glyph stays square */
            if ( attached )
                draw_push_rect_outline( ix, iy, isz, isz, 1.0f, 0, icol );
            else
                draw_push_rect_filled( ix, iy, isz, isz, 0.0f, 0.0f, 1.0f, 1.0f, 0, icol );
            draw_set_rounding( det_save );

            right_limit = det_r.x - WIDGET_PAD;   /* keep the title text clear of the button */
        }

        /* Native caption buttons (min / max / close / pop-in): a native window's titlebar IS the OS
           caption, so its collapse arrow, detach button and gui drag-grab are all suppressed above
           -- instead it gets OS-window controls (native_caption_chrome, gui_window_native.c). */
        if ( native )
            right_limit = native_caption_chrome( win, title_h, right_limit );

        /* Title text, fitted to the room between the arrow square and the detach button (or the
           bar's right edge) so a narrow (shrunk) window ellipsizes the title instead of bleeding
           it under the button / border. */
        draw_text_fit_n( text_x, text_center_y( s_build.win.y, title_h ), COL_TEXT, s_build.win.title,
                         0xFFFFFFFFu, right_limit - text_x );
    }

    /* Border frames the whole window, with or without a title bar.  Reassert the window radius: a
       caption button above may have left the ambient at the widget radius. */
    gui_rect_t win_r = { s_build.win.x, s_build.win.y, s_build.win.w, s_build.win.h };
    draw_set_rounding( ROUND_WIN );
    draw_push_rect_outline( win_r.x, win_r.y, win_r.w, win_r.h, WIN_BORDER, 0, COL_BORDER );

    /* Debug overlay: trace the window frame; the front-most (hover) window stands out. */
    DBG_WINDOW( win_r, ( s_build.win.id == s_interaction.hover_win ) );

    /* Resize affordance: bold the outline on any hot edge.  While a resize is in flight, the
       grabbed edges stay lit even if the cursor drifts off them; otherwise use resize_hot,
       the hover set computed in window_begin (already NORESIZE- and hover_win-gated).
       hot_edges is declared here (not in a block) so the grip section below can read it for
       the R+B -> triangle reverse direction. */
    u8 hot_edges = ( s_interaction.active_id == id_combine( s_build.win.id, GUI_RESIZE_SALT ) )
                 ? s_resize_edges
                 : s_scope.resize_hot;
    if ( hot_edges )
        draw_resize_highlight( win_r, hot_edges );

    /* CAN_AUTOSIZE: a size-grip triangle hugging the bottom-right corner -- both a resize handle
       and an auto-fit button.  Drag it to resize the window (it grabs the same right+bottom edge
       resize the border band uses, so window_begin applies it next frame); double-click it to snap
       the window to this frame's measured content (win->content_* was just written back by
       layout_pop_region).  Only one of the two fires per press, and the double-click never starts a
       drag.  The grip resizes regardless of NORESIZE -- it is the window's own explicit handle.

       Press-1 is deferred behind the press_defer latch (2_interact/gui_move.c, same service as the
       native title bar below), keyed by resize_id: grabbing the resize immediately on press-1 would
       set active_id and swallow press-2 before mouse_double can be tested, so every double-click
       attempt needed several tries. Only committing once the cursor actually moves keeps the grip's
       hit rect stationary through a stationary click, so press-2 lands on it. */
    if ( ( s_build.win.flags & GUI_WIN_CAN_AUTOSIZE ) && !s_build.win.collapsed && win )
    {
        f32          g         = WIDGET_H;           /* grip leg length */
        gui_rect_t gr        = { s_build.win.x + s_build.win.w - g, s_build.win.y + s_build.win.h - g, g, g };
        gui_id_t   resize_id = id_combine( s_build.win.id, GUI_RESIZE_SALT );
        bool         resizing  = ( s_interaction.active_id == resize_id );
        /* Hot when the cursor is over the grip square, or when the R+B corner edges are already
           highlighted (cursor landed on the edge band just inside the border at the corner). */
        bool         hot       = ( s_build.win.id == s_interaction.hover_win ) && rect_hit( gr );
        bool corner_edges_highlighted = ( hot_edges & ( GUI_RESIZE_R | GUI_RESIZE_B ) ) == ( GUI_RESIZE_R | GUI_RESIZE_B );
        hot = hot || corner_edges_highlighted;

        if ( hot && s_interaction.active_id == GUI_ID_NONE )
        {
            if ( s_io.mouse_double[ 0 ] )
            {
                press_defer_cancel();
                bool collapsible = ( s_build.win.title_h > 0.0f ) && !( s_build.win.flags & GUI_WIN_NOCOLLAPSE );
                f32  grip_mb_h   = ( s_build.win.flags & GUI_WIN_MENUBAR ) ? ( WIDGET_H + WIDGET_GAP ) : 0.0f;
                f32  max_w, max_h;
                window_fit_bounds( win, &max_w, &max_h );
                window_fit_size( s_build.win.title, s_build.win.title_h, grip_mb_h, collapsible,
                                 win->scroll.content_w, win->scroll.content_h, max_w, max_h, &win->w, &win->h );
                /* Native floater: forward the fit size to the OS window. */
                if ( native && win->viewport != 0 )
                    app()->window_resize( window_native_id( win ), (i32)win->w, (i32)win->h );
            }
            else if ( s_io.mouse_pressed[ 0 ] )
            {
                press_defer_arm( resize_id );
            }
        }

        /* Poll the pending grip press outside the hot gate, so the cursor sliding off the small
           grip rect mid-press does not strand the drag -- mirrors the title-bar poll below.
           Released without crossing the threshold: the latch clears silently, it was a (potential
           first) click -- active_id is never touched, leaving the grip in place for press-2. */
        if ( press_defer_crossed( resize_id ) )
        {
            resize_grab( s_build.win.id, ( gui_rect_t ){ win->x, win->y, win->w, win->h },
                         GUI_RESIZE_R | GUI_RESIZE_B );
            resizing = true;
        }

        /* Filled right-angle triangle, lit while hovered or actively resizing. */
        draw_push_triangle( gr.x + g, gr.y, gr.x + g, gr.y + g, gr.x, gr.y + g,
                            0, ( hot || resizing ) ? COL_RESIZE_HOT : COL_TEXT_DIM );
    }

    /* Balance the clip push, which window_begin only made for an expanded window. */
    if ( !s_build.win.collapsed )
        draw_pop_clip_rect();

    /* Subsequent draws (low-level API, the next window) revert to the background key and the
       main surface; the next window_begin re-routes them to its own viewport.  Restore the base
       clip to the main display so background / low-level draws are not bounded by this window's
       (possibly larger or smaller) surface. */
    draw_set_window( 0 );
    draw_set_sort_key( 0 );
    draw_set_viewport( 0 );
    draw_set_band( 0 );
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );

    /* Window move grab.  Decided here, after this window's widgets have run, and pinned off
       entirely by NOMOVE (fixed-position widgets).  This window must be the one under the
       cursor (hover_win) and nothing must already own active_id -- an edge press never reaches
       here, since window_begin grabbed the resize before the widgets ran.

       frame_only (GUI_WIN_NATIVE shell): gui owns the full client surface via HTCLIENT, so
       title-bar clicks land here instead of going to the OS.  Dispatch them: single press starts
       an OS move (Aero Snap follows for free); double-click sends the maximize toggle; right-click
       shows the system menu.  No active_id is set -- the OS modal loop runs in place of gui drag.

       All other windows (panels on main surface, floaters on owned viewports): gui drag grab.
       Left button obeys the global drag mode and only fires on empty space (hover_id == NONE, so a
       widget press drives the widget).  Middle button grabs from anywhere -- no widget consumes it.
       Floaters on owned viewports move their whole OS window each frame (see window_begin drag apply). */
    if ( s_build.win.id == s_interaction.hover_win && s_interaction.active_id == GUI_ID_NONE
         && !( s_build.win.flags & GUI_WIN_NOMOVE ) )
    {
        gui_rect_t title_r = { s_build.win.x, s_build.win.y, s_build.win.w, s_build.win.title_h };
        win_id_t     os      = window_native_id( win );

        if ( frame_only || ( native && win && win->viewport != 0 ) )
        {
            /* Native title bar (frame-shell or floater): defer dispatch behind a drag threshold
               so a fast double-click never triggers window_start_move or sets active_id on
               click-1 -- either would absorb click-2 before mouse_double can be tested.
               Right-click on the frame-shell opens the system menu (no double-click concern). */
            if ( s_interaction.hover_id == GUI_ID_NONE && rect_hit( title_r ) )
            {
                if ( s_io.mouse_double[ 0 ] )
                {
                    press_defer_cancel();
                    app()->window_title_event( os );
                }
                else if ( s_io.mouse_pressed[ 0 ] )
                {
                    press_defer_arm( s_build.win.id );
                }
            }
            if ( frame_only && s_io.mouse_pressed[ 1 ]
                 && s_interaction.hover_id == GUI_ID_NONE && rect_hit( title_r ) )
                app()->window_system_menu( os, ( i32 )s_io.mouse_x, ( i32 )s_io.mouse_y );

            /* Middle button: immediate grab for floaters (middle has no double-click concern). */
            if ( !frame_only && s_io.mouse_pressed[ 2 ] )
                move_grab( s_build.win.id, 2, s_build.win.x, s_build.win.y );
        }
        else
        {
            /* Regular panel: immediate drag grab. */
            bool left_grab = s_io.mouse_pressed[ 0 ] && s_interaction.hover_id == GUI_ID_NONE
                          && s_win_drag_mode != GUI_WIN_DRAG_NONE
                          && ( s_win_drag_mode == GUI_WIN_DRAG_BODY || rect_hit( title_r ) );
            bool mid_grab  = s_io.mouse_pressed[ 2 ];

            if ( left_grab || mid_grab )
                move_grab( s_build.win.id, mid_grab ? 2 : 0, s_build.win.x, s_build.win.y );
        }
    }

    /* Native title-bar poll: commit or clear the pending press outside the hover / active_id
       gate so dragging off the title bar does not stall an in-flight drag.  Keyed by this
       window's id, so only the arming window acts; frame_only picks the same dispatch the arm
       site saw (flags do not change mid-gesture). */
    if ( press_defer_crossed( s_build.win.id ) )
    {
        win_id_t os = window_native_id( win );
        if ( frame_only )
        {
            app()->window_start_move( os );
        }
        else
        {
            /* Floater: commit to gui drag; window_begin applies via window_set_pos.
               If the floater is maximized, restore it first -- dragging a maximized OS
               window while calling window_set_pos leaves it in a bad state and produces
               a stale-chrome white bar across the titlebar.  Grab with a synthetic origin
               so the cursor lands at a natural title-bar position on the restored window
               (the maximized-viewport-local offsets would snap it to screen origin). */
            if ( app()->window_state( os ).maximized )
            {
                app()->window_restore( os );
                move_grab( s_build.win.id, 0, s_io.mouse_x - s_build.win.title_h,
                           s_io.mouse_y - s_build.win.title_h * 0.5f );
            }
            else
            {
                move_grab( s_build.win.id, 0, s_build.win.x, s_build.win.y );
            }
        }
    }

    /* Drag-to-dock release: dock this window if it was released over a valid target (computed by
       dock_drag_detect in window_begin).  Gating lives inside -- s_dock_drag is private to the dock
       unit, included after this one -- so call it unconditionally; it no-ops unless this is the
       dragged window on its release edge.  It renders docked from next frame. */
    dock_drag_commit( s_build.win.id, s_build.win.title );
}

// clang-format on
/*============================================================================================*/
