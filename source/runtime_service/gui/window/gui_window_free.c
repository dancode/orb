/*==============================================================================================

    runtime_service/gui/window/gui_window_free.c -- The free-float window: geometry + gesture
    resolution (window_begin_ex) and its public front doors (window_begin, viewport_shell).

    Resolves everything about THIS frame's window geometry before any body widget runs: drag,
    tear-off, edge-resize, autosize, and the resulting chrome rect -- the size-policy helpers
    (window_clamp / window_fit_size / window_apply_resize) it shares with the deferred chrome are
    defined here too.  The docked branch (window_begin_docked) lives in gui_window_docked.c,
    included just before this file; the deferred chrome window_end paints after the body widgets
    run (titlebar, buttons, border, resize grip) lives in gui_window_end.c, included just after --
    both share the s_build.win / s_scope handoff this file commits.  The persistent window record
    and the registry live in gui_window.c; this file only reads and mutates that state through
    window_get and the shared drag/resize state vars -- it declares no long-lived state of its own.

    A window is treated as a large compound widget, so this builds on the shared primitives
    in present/gui_paint_core.c (widget_bg_color, the label grammar) and interact/gui_item.c
    (widget_behavior); the style vocabulary (WIDGET_* / WIN_* / COL_*) resolves in
    foundation/gui_style.c.

    Included by gui.c after the widget family files, so the window record (gui_window.c), the
    shared widget core, and the leaf widgets are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Per-window widget ids -- salted from the window id so a window's chrome controls never
    collide with each other or with a title-hashed widget id inside the same window.
----------------------------------------------------------------------------------------------*/

/* Scrollbar salts (GUI_SCROLLBAR_SALT / GUI_HSCROLLBAR_SALT) and the scrollbar widget
   itself live in widgets/gui_scrollbar.c -- the window body is just a region whose engine
   emits them into its gutters. */

/* Collapse arrow: a distinct stable per-window widget id. */
#define GUI_COLLAPSE_SALT   0xC011A95Eu

/* Detach / reattach button (title-bar right edge): a distinct stable per-window widget id. */
#define GUI_DETACH_SALT     0xDE7AC405u

/* Close button (title-bar right edge, outermost): a distinct stable per-window widget id. */
#define GUI_CLOSE_SALT      0xC105E00Du

/* The native-borderless (GUI_WIN_NATIVE) machinery -- window_is_native / window_native_id, the
   caption-button layout + glyphs, window_sync_native, vp_request_button, and the caption strip
   window_end calls (native_caption_chrome) -- lives in gui_window_native.c, included just
   before this file. */

/* GUI_RESIZE_SALT, the RESIZE_BAND_* grab-band constants, and the record-agnostic resize helpers
   (edge_resize_hit, resize_grab, resize_apply_edges) live in interact/gui_resize.c (the
   GUI_RESIZE_* edge bits in gui_internal.h, the hot-edge paint in present/gui_paint_core.c:
   draw_resize_highlight), ahead of gui_layout.c, so child_begin reuses the same mechanism (the
   dock splitter does not; it has its own drag path in dock/).  Only the
   window's size policy stays below: the min clamp with far-edge pinning (window_apply_resize) and
   the content auto-fit (window_fit_size). */

/*----------------------------------------------------------------------------------------------
    window_begin / window_end
----------------------------------------------------------------------------------------------*/

/* Keep a dragged window reachable: clamp so its top edge stays on-screen and at
   least one title-bar's worth of the window remains within the display bounds.
   Uses the window's own viewport dimensions so dragging on a secondary surface
   clamps against that surface, not the primary.  The top bound is the host's
   native caption band (caption_inset): a window cannot slide its titlebar under
   the OS-owned caption, where the grab would be lost -- mirroring how a child
   stays below a normal OS title bar.  Inset is 0 with no native shell, so the
   default-chrome path keeps the old top-of-surface behavior.
   GUI_WIN_NO_BOUNDARY_CLAMP bypasses this entirely. */
static void
window_clamp( gui_window_t* win )
{
    if ( win->flags & GUI_WIN_NO_BOUNDARY_CLAMP )
        return;

    const gui_viewport_t* vp = &g_ctx->vp.pool[ win->viewport ];
    f32 dw = vp_w( vp );
    f32 dh = vp_h( vp );
    const f32 margin = WIN_TITLE_H;
    const f32 top    = vp->caption_inset;
    const f32 max_x  = dw - margin;
    const f32 max_y  = dh - margin;

    if ( win->x > max_x )           win->x = max_x;
    if ( win->y > max_y )           win->y = max_y;
    if ( win->x < margin - win->w ) win->x = margin - win->w;
    if ( win->y < top )             win->y = top;
}

/* Space left between win's current position and the far edge of its viewport -- the ceiling
   window_fit_size clamps against so an autosized window never grows past the screen its
   position is held fixed. */
static void
window_fit_bounds( const gui_window_t* win, f32* out_max_w, f32* out_max_h )
{
    const gui_viewport_t* vp = &g_ctx->vp.pool[ win->viewport ];
    *out_max_w = vp_w( vp ) - win->x;
    *out_max_h = vp_h( vp ) - win->y;
}

/*----------------------------------------------------------------------------------------------
    Edge resize

    A window may be resized by grabbing a band that straddles any edge or corner -- reaching a
    little inside the border and a little outside it, OS-style, so the grip is easy to land on.
    The hover/grab is resolved up front in window_begin (using last frame's hover_win) so it
    takes priority over the scrollbar and collapse arrow underneath; the resize itself is
    applied at the top of the next window_begin, one frame later, mirroring the title-bar drag.

    Reaching outside the border needs an exception to the normal hover rule: a resizeable
    window nominates an outer-expanded rect for hover_win so the cursor still counts as "over"
    it within the outer band -- otherwise the edge would go cold the instant the cursor crossed
    the border.  The expansion is only the few outer pixels, so occlusion is barely affected.
----------------------------------------------------------------------------------------------*/

/* Smallest width a window may be shrunk to. */
static f32 window_min_w( void ) { return WIN_TITLE_H * 4.0f; }

/* Smallest height: always keeps the title bar fully visible plus one widget row of body, so
   a resize never eats into the title bar vertically.  title_h is 0 for a NOTITLEBAR window. */
static f32 window_min_h( f32 title_h ) { return title_h + WIDGET_H + WIN_BORDER; }

/*----------------------------------------------------------------------------------------------
    Auto-resize

    window_fit_size computes the window geometry that hugs a given content extent.

    Both ALWAYS_AUTOSIZE (every frame) and the CAN_AUTOSIZE grip (double-click) pass
    win->scroll.content_w / content_h -- the rightmost pixel reached by rendered widgets plus the
    scroll bias.  This is stable: a fill widget reports its full cell width so the window tracks its
    current configuration rather than collapsing to widget label widths.

    Height works the same way: pen travel independent of window height, no feedback.
    Never narrower than the title bar or the resize minimum so the chrome stays legible.

    max_w / max_h cap the fit against the viewport (position held fixed, so this is just the space
    remaining from win->x/y to the far edge): a long list should scroll inside the window rather
    than growing it past the screen.  The min clamp runs last so a viewport smaller than the
    minimum size never shrinks the chrome below legibility.
----------------------------------------------------------------------------------------------*/

static void
window_fit_size( const char* title, f32 title_h, f32 mb_h, bool collapsible,
                 f32 content_w, f32 content_h, f32 max_w, f32 max_h, f32* out_w, f32* out_h )
{
    /* The measured content is the full canvas -- items plus the region pads on both ends of each
       axis -- so the body just wraps it: width is the canvas, height adds the chrome above it and
       the bottom border (the only body inset the canvas does not carry). */
    f32 want_w = content_w;
    f32 want_h = title_h + mb_h + content_h + WIN_BORDER;

    /* Stay wide enough for the title bar: the collapse-arrow lead (or the left pad) + the title
       text + a trailing pad.  Keeps the title from being clipped when the body is narrow. */
    if ( title && title_h > 0.0f )
    {
        f32 lead    = collapsible ? title_h : WIDGET_PAD;
        f32 title_w = lead + font_text_w( title ) + WIDGET_PAD;
        if ( want_w < title_w ) want_w = title_w;
    }

    if ( want_w > max_w ) want_w = max_w;
    if ( want_h > max_h ) want_h = max_h;

    f32 min_w = window_min_w();
    if ( want_w < min_w ) want_w = min_w;

    *out_w = want_w;
    *out_h = want_h;
}

/* Apply the in-flight resize to win's geometry, clamped to the minimum size.  The raw edge-drag is
   the shared resize_apply_edges (origin / pin math); the window then layers its own policy -- the
   min clamp with a moving edge stopping against the pinned far edge. */
static void
window_apply_resize( gui_window_t* win, f32 title_h )
{
    const f32 min_w = window_min_w();
    const f32 min_h = window_min_h( title_h );

    gui_rect_t r = { win->x, win->y, win->w, win->h };
    resize_apply_edges( &r, s_resize_edges );
    win->x = r.x;  win->y = r.y;  win->w = r.w;  win->h = r.h;

    /* Clamp to minimum; a moving edge stops against the pinned far edge. */
    if ( win->w < min_w )
    {
        if ( s_resize_edges & GUI_RESIZE_L ) win->x = s_resize_fix_x - min_w;
        win->w = min_w;
    }
    if ( win->h < min_h )
    {
        if ( s_resize_edges & GUI_RESIZE_T ) win->y = s_resize_fix_y - min_h;
        win->h = min_h;
    }
}

/* resize_grab (the press-time anchor record) and draw_collapse_arrow live in gui_paint_core.c,
   shared with child_begin and collapsing_header respectively. */

/* window_begin_docked (the docked branch of window_begin) lives in gui_window_docked.c, included
   just before this file, so window_begin_ex below can call it. */

/*----------------------------------------------------------------------------------------------
    window_begin_ex helpers

    window_begin_ex is one long linear sequence of independent per-frame resolves (native sync,
    drag, tear-off, resize).  Each stage only touches its own inputs and s_build / g_ctx -- pulled
    out here so each is readable and named on its own, the same way window_clamp / window_apply_resize
    / window_fit_size already are above.
----------------------------------------------------------------------------------------------*/

/* window_sync_native (the native geometry pin + frame publish) lives in gui_window_native.c. */

/* Apply an in-progress title-bar drag: this window holds active_id while the button is down.
   On the main surface the panel slides within it (win->x/y).  On a floater the panel fills the
   surface, so the drag instead moves the whole OS window in SCREEN space to follow the cursor --
   the floater client origin tracks (screen cursor - grab offset), so the grabbed title point stays
   pinned under the cursor as it crosses the desktop.  Screen cursor reads stay valid because the
   origin window keeps OS mouse capture for the whole gesture (see the tear-off in
   gui_viewport_update). */
static void
window_apply_drag( gui_window_t* win, gui_id_t id )
{
    if ( !interact_held( id ) )
        return;

    if ( win->viewport == 0 )
    {
        move_track( id, s_io.mouse_x, s_io.mouse_y, &win->x, &win->y );
        window_clamp( win );
    }
    else
    {
        i32 cx = 0, cy = 0;
        f32 nx = 0.0f, ny = 0.0f;
        app()->mouse_position_screen( &cx, &cy );
        move_track( id, (f32)cx, (f32)cy, &nx, &ny );
        app()->window_set_pos( g_ctx->vp.pool[ win->viewport ].win_id, (i32)nx, (i32)ny );
        win->x = 0.0f;
        win->y = 0.0f;
    }
}

/* Seamless tear-off / merge-back gesture (Dear-ImGui style: no release required), plus drag-to-dock
   detection.  While a title-bar drag is live (button still down, this window owns active_id),
   crossing a surface boundary reassigns which surface hosts the window -- and the drag continues
   uninterrupted in the new home.  The request is enqueued for gui_viewport_update, the safe point
   to create/destroy a surface; one request at a time, and NOMOVE windows never tear off.

   Attached (viewport 0): the OS mouse is captured by the main window, so s_io.mouse_x/y stay in its
   client space and read out-of-bounds exactly when the cursor leaves it -- tear off into a floater
   the moment that happens.

   Floating: the floater follows the cursor, so the cursor never leaves IT; instead test the SCREEN
   cursor against the main window's client rect and merge back when it re-enters.  Capture remains
   on the main window throughout, so the screen-cursor read is valid here too.

   Drag-to-dock is mutually exclusive with tear-off: that fires only when the cursor leaves the
   surface, this only when it is inside over a dockspace leaf. */
static void
window_apply_tearoff_gesture( gui_window_t* win, gui_id_t id, const char* title, gui_win_flags_t flags )
{
    /* Drop the merge-back latch once this window's drag ends, so the next grab re-arms from scratch
       (a floater grabbed again while over its parent must not inherit an armed latch). */
    if ( s_vp_drag_id == id && !interact_held( id ) )
        s_vp_drag_id = GUI_ID_NONE;

    /* A live title-bar drag of a movable window -- the gesture both blocks below react to. */
    bool live_drag = interact_held( id ) && s_io.mouse_down[ 0 ] && !( flags & GUI_WIN_NOMOVE );

    if ( live_drag && !( flags & GUI_WIN_NO_DETACH ) && !s_vp_request.active )
    {
        /* Re-arm the merge-back latch on a fresh drag of this window (the previous gesture targeted
           a different window or none).  Until the cursor leaves the main surface, merge-back stays
           disarmed, so a floater grabbed while sitting over its parent is not yanked straight back. */
        if ( s_vp_drag_id != id )
        {
            s_vp_drag_id     = id;
            s_vp_merge_armed = false;
        }

        bool crossed = false;
        if ( win->viewport == 0 )
        {
            const gui_viewport_t* hv = &g_ctx->vp.pool[ 0 ];
            f32 dw = vp_w( hv ); if ( dw < 1.0f ) dw = 1.0f;
            f32 dh = vp_h( hv ); if ( dh < 1.0f ) dh = 1.0f;
            crossed = s_io.mouse_x < 0.0f || s_io.mouse_y < 0.0f
                   || s_io.mouse_x >= dw || s_io.mouse_y >= dh;
        }
        else
        {
            /* Merge back when the screen cursor re-enters the main window's client rect, inset by a
               title-bar margin.  The inset is hysteresis: tear-off fires at the exact main edge, so
               without a dead-band a cursor hovering on the boundary would spawn and destroy a floater
               every frame.  Re-entering only past the inset breaks that oscillation. */
            i32 cx = 0, cy = 0, mx = 0, my = 0;
            app()->mouse_position_screen( &cx, &cy );
            app()->window_get_pos( g_ctx->vp.pool[ 0 ].win_id, &mx, &my );
            const gui_viewport_t* mv = &g_ctx->vp.pool[ 0 ];
            i32 mw = (i32)vp_w( mv );
            i32 mh = (i32)vp_h( mv );
            i32 inset = (i32)WIN_TITLE_H;
            bool inside = cx >= mx + inset && cy >= my + inset
                       && cx < mx + mw - inset && cy < my + mh - inset;

            /* Edge-trigger: arm once the cursor is clear of the main window, then merge only on the
               re-entry.  This distinguishes a real leave->enter crossing from a floater that simply
               started inside (overlapping its parent), which must first be dragged out. */
            if ( !inside )
                s_vp_merge_armed = true;
            crossed = inside && s_vp_merge_armed;
        }

        if ( crossed )
        {
            s_vp_request.active  = true;
            s_vp_request.by_drag = true;   /* place the floater under the cursor, keep it tracking */
            s_vp_request.win_id  = id;
            s_vp_request.from_vp = win->viewport;
            s_vp_request.title   = title;
        }
    }

    /* Drag-to-dock: while this free window is title-dragged over a dockspace on its own surface,
       preview a drop target (per-node 5-way overlay) -- the route seam's drag verb.  Re-tests
       s_vp_request.active because the tear-off block above may have just claimed the gesture. */
    if ( live_drag && !s_vp_request.active )
        window_route_drag( id, win );
}

/* Resolve this frame's edge-resize / autosize-grip hover-and-grab, before any widget can claim the
   press.  The edge protocol (hover gate, grab band, grab on press, directional cursor) is the
   resize_item service (interact/gui_resize.c) -- owner_win is the window's OWN id, since this
   resolves before s_build.win.id is stamped.  Sets s_scope.resize_hot (read by widget_behavior +
   window_end's highlight); a hot autosize grip joins the mask as GUI_RESIZE_GRIP with the R+B
   edge bits promoted alongside it, but never drives the cursor.  Returns the PRE-grip-promotion
   mask -- the debug overlay's outer-band capture wants only the true edge hit. */
static u8
window_resolve_resize_hot( gui_id_t id, gui_window_t* win, gui_win_flags_t flags,
                           gui_rect_t disp_r, bool collapsed, bool resizeable, gui_id_t resize_id )
{
    u8   resize_hot = 0;
    bool dragging   = false;
    if ( resizeable )
        resize_hot = resize_item( id, id, disp_r,
                                  GUI_RESIZE_L | GUI_RESIZE_R | GUI_RESIZE_T | GUI_RESIZE_B,
                                  collapsed, &dragging );
    s_scope.resize_hot = resize_hot;   /* read by widget_behavior + window_end's highlight */

    /* CAN_AUTOSIZE size-grip: reserve the bottom-right corner ahead of the body's scrollbars the
       same way the edge band reserves the borders.  The grip square overlaps the scroll gutter, but
       the scrollbar runs first (in layout_pop_region), so without this it would claim the press and
       the grip -- drawn and grabbed later in window_end -- would sit dead behind it.  Suppressing
       widget hover over the grip rect leaves active_id free for window_end to grab. */
    bool grip_hot = false;
    if ( ( flags & GUI_WIN_CAN_AUTOSIZE ) && !collapsed && s_interaction.hover_win == id
         && ( interact_idle() || interact_held( resize_id ) ) )
    {
        f32          g  = WIDGET_H;   /* grip leg length -- matches window_end's grip rect */
        gui_rect_t gr = { win->x + win->w - g, win->y + disp_r.h - g, g, g };
        grip_hot = rect_hit( gr );
    }

    /* A hot grip joins the resize mask (GRIP suppresses widget hover like an edge; the R+B edge
       bits are promoted with it so the border highlight follows the triangle -- the reverse,
       R+B edges -> triangle, runs in window_end where hot_edges is resolved). */
    if ( grip_hot )
        s_scope.resize_hot |= GUI_RESIZE_GRIP | GUI_RESIZE_R | GUI_RESIZE_B;

    /* The cursor was already driven inside resize_item, from the PRE-grip-promotion mask: the grip
       square alone (no true edge hit) keeps the regular arrow, since the diagonal resize cursor's
       off-center hotspot makes it hard to see exactly where a double-click will land on the small
       triangle.  A true edge-band hit -- including the corner where the band overlaps the grip --
       still shows the resize cursor. */
    (void)dragging;

    return resize_hot;
}

/* window_begin_ex -- the shared body of window_begin, with the window id supplied explicitly and
   the title used only for display + chrome (NULL = no title text).  gui_window_begin hashes the
   title for its id; the popup layer (gui_popup.c) passes a salted popup id and its own title (or
   NULL), so a popup reuses the entire window path -- record, geometry, region, clip, scroll,
   auto-resize, chrome -- with nothing duplicated.  The caller is responsible for any overlay
   save/restore needed when this is begun inside another window (popups detach via gui_overlay_*). */
static bool
window_begin_ex( gui_id_t id, const char* title, f32 x, f32 y, f32 w, f32 h, gui_win_flags_t flags )
{
    if ( title ) DBG_NAME( id, title );

    /* x/y/w/h are the initial geometry; the registry owns position after that. */
    gui_window_t* win = window_get( id, x, y, w, h );
    win->flags            = flags;
    s_build.win.hidden    = false;   /* default; the CLOSEABLE branch below flips it */

    /* Closeable + closed: the window is fully hidden this frame -- no chrome, no body, no hover.
       begin returns false (the caller skips its widgets) and window_end early-outs on win_hidden.
       The record persists with its geometry intact, so window_set_open revives it where it was.
       Tested before the dock lookup so a closed window leaves any node it was tabbed into, too.

       last_frame is deliberately NOT refreshed here: a closed floater must read as "abandoned" so
       viewport_update tears down its OS window (reverting the record to viewport 0).  A
       closed panel lives on viewport 0, which the teardown loop never touches, so freezing its
       last_frame is harmless -- the appearing test simply re-fires once on re-open. */
    if ( ( flags & GUI_WIN_CLOSEABLE ) && win->closed )
    {
        s_build.win.hidden   = true;
        s_build.win.dock_node = NULL;
        return false;
    }

    /* Re-open of a CLOSEABLE floater: closing one let the abandoned-teardown free its OS window and
       revert this record to viewport 0.  Re-spawn it as a floater at its saved screen position via
       the tear-off request -- the title is in hand here, which viewport_update needs to label
       the OS window.  Stay hidden this one frame until the surface exists so it never flashes on the
       main surface at (0,0); last_frame IS stamped so the fresh floater is not read as abandoned. */
    if ( win->reopen.floater && win->viewport == 0 && !s_vp_request.active )
    {
        win->reopen.floater   = false;
        s_vp_request.active   = true;
        s_vp_request.by_drag  = false;
        s_vp_request.win_id   = id;
        s_vp_request.from_vp  = 0;
        s_vp_request.title    = title;
        s_vp_request.has_home = true;   /* spawn reads restore geometry + maximized from the record */

        win->last_frame      = g_ctx->retained.frame;
        s_build.win.hidden   = true;
        s_build.win.dock_node = NULL;
        return false;
    }

    /* Already restored on a surface of its own: clear any stale re-open flag (e.g. the floater was
       re-opened within the teardown grace window, before its viewport reverted). */
    if ( win->viewport != 0 )
        win->reopen.floater = false;

    /* Closed-viewport fallback: if this window's surface was destroyed, revert to primary. */
    if ( win->viewport > 0 && !rhi_handle_valid( g_ctx->vp.pool[ win->viewport ].vb ) )
        win->viewport = 0;

    /* Ask the dock who places this window (the route seam, dock/gui_dock_route.c): any pending
       tab-onto-window group forms now, while the title (its tab name) is in hand, and a window
       tabbed into a dock node is placed + chromed by the node, not free-floated.  The whole
       free-float path below (drag, resize, tear-off, chrome) is bypassed for it. */
    gui_win_route_t route = window_route_resolve( id, title, win );
    if ( route.node )
        return window_begin_docked( win, id, title, flags, &route );

    /* Next-window channel: apply any queued window_set_next_pos / _size before this frame's drag,
       resize, and autosize act on the geometry, so a ONCE / APPEARING seed becomes the incoming
       state the user then interacts with.  `appearing` is the first begin (last_frame 0) or the
       first begin after a frame of absence -- it renews the one-shot APPEARING permission. */
    bool appearing = ( win->last_frame == 0u ) || ( win->last_frame != g_ctx->retained.frame - 1u );

    /* Raise to front on every appearance: first creation and re-opens both get a fresh z
       above all currently-live windows, so no two windows ever share a z on their first
       visible frame.  Re-opened windows surface on top rather than re-using a stale z
       that may sit below windows that were raised while they were closed.

       EXCEPT a popup / tooltip overlay: popup_begin (gui_popup.c) has already put win->z in the
       reserved overlay band above this call.  Stamping a normal counter value here would sink it
       out of that band for exactly the appearing frame, breaking its paint order over the
       windows below.  An overlay's z is the popup layer's to stamp; leave it untouched. */
    if ( appearing && !win->overlay )
        win->z = surface_z_raise( win->z );

    window_apply_next( win, appearing );
    win->last_frame = g_ctx->retained.frame;

    /* NOTITLEBAR removes the bar entirely (title_h 0); content then starts at the top edge.
       Collapsing lives on the title bar, so NOTITLEBAR and NOCOLLAPSE both pin the window
       open -- any stale collapsed state is cleared so it cannot resurface if the flag drops. */
    bool has_titlebar = !( flags & GUI_WIN_NOTITLEBAR );
    f32  title_h      = has_titlebar ? WIN_TITLE_H : 0.0f;

    /* A native window IS its OS window: it cannot collapse (the OS window has no such state) and
       its geometry is owned by the OS surface, not gui -- pin it to the viewport so the gui
       window always exactly covers its host window.  Border/titlebar gestures route to the OS
       (see the resize and move-grab sections below). */
    bool native = window_is_native( win, flags );

    /* Frame-only shell: an explicitly GUI_WIN_NATIVE window stands in for a borderless OS window's
       frame -- it draws the titlebar + border but leaves its body empty and click-through (no
       background fill, hover nominated only over the titlebar, never raised).  That way the windows
       living inside the borderless viewport stay visible and selectable above it.  A detached floater
       is native too (via its owned viewport) but is real content, so it keeps a normal solid body. */
    bool frame_only = ( flags & GUI_WIN_NATIVE ) != 0;

    if ( native )
        window_sync_native( win, flags );

    bool can_collapse = has_titlebar && !( flags & GUI_WIN_NOCOLLAPSE ) && !native;
    if ( !can_collapse ) win->collapsed = false;
    bool collapsed = win->collapsed;

    /* ALWAYS_AUTOSIZE owns its own geometry: it cannot be user-resized and shows no scrollbars
       (the body always fits), and its size is recomputed below from the measured content.  The
       body region is opened with NOSCROLL so it never reserves a gutter -- a clean content_w. */
    bool autosize = ( flags & GUI_WIN_ALWAYS_AUTOSIZE ) != 0;
    gui_win_flags_t body_flags = autosize ? ( flags | GUI_WIN_NOSCROLL ) : flags;

    window_apply_drag( win, id );                             /* in-progress title-bar drag */
    window_apply_tearoff_gesture( win, id, title, flags );    /* tear-off / merge-back / drag-to-dock */

    /* Apply an in-progress edge resize (active_id is the resize-salted window id).  Runs after
       the drag apply -- the two are mutually exclusive, only one can own active_id at a time. */
    if ( interact_held( id_combine( id, GUI_RESIZE_SALT ) ) )
    {
        if ( native && win->viewport != 0 )
        {
            /* Native floater: s_io.mouse_x/y is only valid while the cursor is over THIS window.
               During the drag the cursor can leave the floater, causing mouse_position() to return
               coords in whatever window it crosses into -- which makes window_apply_resize compute
               the wrong size (usually too small).  Derive local coords from the stable screen
               cursor and the floater's fixed screen origin (bottom-right resize pins the top-left)
               so the formula stays correct regardless of which window the cursor is over. */
            win_id_t os = window_native_id( win );
            i32 scx = 0, scy = 0, sox = 0, soy = 0;
            app()->mouse_position_screen( &scx, &scy );
            app()->window_get_pos( os, &sox, &soy );
            f32 local_x = (f32)scx - (f32)sox;
            f32 local_y = (f32)scy - (f32)soy;
            f32 new_w   = local_x - s_resize_off_x;
            f32 new_h   = local_y - s_resize_off_y;
            if ( new_w < window_min_w() )        new_w = window_min_w();
            if ( new_h < window_min_h( title_h ) ) new_h = window_min_h( title_h );
            win->w = new_w;
            win->h = new_h;
            app()->window_resize( os, (i32)new_w, (i32)new_h );
        }
        else
        {
            window_apply_resize( win, title_h );
        }
    }

    /* ALWAYS_AUTOSIZE: hug the content measured last frame (held in win->content_*).  Skipped while
       collapsed (the title-bar-only height is preserved) and on the very first appearance, before
       any content has been measured -- then the caller's initial w/h stands for one frame. */
    f32 fit_mb_h = ( flags & GUI_WIN_MENUBAR ) ? ( WIDGET_H + WIDGET_GAP ) : 0.0f;
    if ( autosize && !collapsed && win->scroll.content_h > 0.0f )
    {
        f32 max_w, max_h;
        window_fit_bounds( win, &max_w, &max_h );
        window_fit_size( title, title_h, fit_mb_h, can_collapse, win->scroll.content_w, win->scroll.content_h,
                         max_w, max_h, &win->w, &win->h );
    }

    /* Collapsed windows shrink to just their title bar, freeing the space below; win->h is
       preserved so reopening restores the previous size.  disp_h is the height actually
       shown this frame and drives the hover rect, clip, and border. */
    f32 disp_h = collapsed ? title_h : win->h;

    /* Edge resize, resolved here so it pre-empts the scrollbar and collapse arrow (resolved in
       window_end) underneath: while the cursor sits on a hot edge, s_scope.resize_hot suppresses
       every widget hover in this window, and a press grabs the resize before any widget can.
       Gated on hover_win (last frame's front-most), so only the top window's edges go hot. */
    gui_rect_t disp_r    = { win->x, win->y, win->w, disp_h };
    gui_id_t   resize_id = id_combine( id, GUI_RESIZE_SALT );
    bool         resizeable = !( flags & GUI_WIN_NORESIZE ) && !autosize && !native;
    u8 resize_hot = window_resolve_resize_hot( id, win, flags, disp_r, collapsed, resizeable, resize_id );

    /* Nominate this window as the one under the cursor (front-most by z wins).  A resizeable
       window expands its nominee rect by the outer grab band (horizontally only when collapsed,
       since its height is pinned) so the cursor still counts as "over" it just outside the
       border -- that is what keeps an edge hot as the cursor crosses to the outside.  The
       winner becomes hover_win next frame; that single fact gates all widget hit-testing. */
    f32 ox = resizeable ? RESIZE_BAND_OUTER : 0.0f;
    f32 oy = ( resizeable && !collapsed ) ? RESIZE_BAND_OUTER : 0.0f;
    if ( !( flags & GUI_WIN_NO_INPUT ) )
    {
        if ( frame_only )
            /* Frame-only shell: only the titlebar (caption band + its buttons) is interactive; the body
               is click-through so windows inside the viewport keep their own hover and selection. */
            surface_hover_nominate( id, ( gui_rect_t ){ win->x, win->y, win->w, title_h },
                                   win->z, win->viewport );
        else
            surface_hover_nominate( id, ( gui_rect_t ){ win->x - ox, win->y - oy,
                                                         win->w + 2.0f * ox, disp_h + 2.0f * oy }, win->z,
                                   win->viewport );
    }

    /* All of this window's geometry is stamped with its z so flush can paint
       windows back-to-front regardless of window_begin call order, and with its
       viewport so flush dispatches it to the surface hosting this window.
       cur_viewport is updated first so DBG_RESIZE below captures to the correct per-viewport list. */
    draw_set_window( id );                  /* stable cache key: all this window's spans share it */
    draw_set_sort_key( win->z );
    draw_set_viewport( win->viewport );
    draw_set_band( ( flags & GUI_WIN_DEBUG_BAND ) ? 1u : 0u );
    s_build.win.viewport = win->viewport;   /* update ambient so new windows created after this inherit it */

    /* Debug overlay: show the outer edge-resize grab band (the catch region just outside the
       border), brightened while an edge is armed.  Only meaningful for a resizeable window. */
    if ( resizeable )
        DBG_RESIZE( ( ( gui_rect_t ){ win->x - ox, win->y - oy,
                                        win->w + 2.0f * ox, disp_h + 2.0f * oy } ), resize_hot );

    /* Clip this window against ITS surface's extent, not the main window's: the window clip pushed
       below intersects the base clip (clip_stack[0]), so seed that base with this viewport's drawable
       size.  Falls back to the main display when the surface size is unset (single-window default).
       window_end restores the main display for subsequent background / low-level draws. */
    {
        const gui_viewport_t* vp = &g_ctx->vp.pool[ win->viewport ];
        draw_set_root_clip( vp_w( vp ), vp_h( vp ) );
    }

    /* Window chrome (background, titlebar, border) is not an item: clear any disabled latch a prior
       window's trailing widget left, so this window paints opaque and its chrome interacts. */
    item_flags_chrome_reset();

    /* Commit window chrome state for the widgets and window_end.  The layout pen, content
       column, scroll, and scrollbars are all owned by the body region opened below -- the
       window no longer resolves any of that itself; it is just the root region plus chrome. */
    s_build.win.id        = id;
    s_scope.win           = id;      /* interaction scope: this window owns the items that follow */
    s_build.win.title     = title;   /* cached for window_end's deferred chrome */
    s_build.win.collapsed = collapsed;
    s_build.win.flags     = flags;   /* window_end reads these for chrome + resize grab */
    s_build.win.title_h   = title_h; /* 0 when NOTITLEBAR */
    s_build.win.rec       = win;     /* collapse write-back target for window_end */
    s_build.win.dock_node = NULL;    /* free-floating: window_end takes the normal chrome path */
    s_build.win.x         = win->x;
    s_build.win.y         = win->y;
    s_build.win.w         = win->w;
    s_build.win.h         = disp_h;  /* displayed height (title bar only when collapsed) */

    /* A collapsed window emits no body: the caller skips its widgets on the false return, so
       no body region is opened and no clip is pushed (window_end mirrors this on win_collapsed).
       The fixed-size chrome window_end draws is wholly inside the app bounds.  A caller that
       ignores the return and emits widgets anyway draws into the empty root layout frame --
       harmless zero-size rects -- rather than into the window. */
    if ( !collapsed )
    {
        /* One clip rect for the whole window: the background, the scrolled content, and the
           titlebar/border chrome (deferred to window_end) all share it, so the window flushes
           as a single draw command.  Content scrolled under the title bar or border is
           overpainted by the chrome window_end draws last; anything past the outer edge is
           clipped here.  The body region reuses this clip (own_clip false) -- it does not push
           a second one; only a child_begin inside the window adds another. */
        draw_push_clip_rect( win->x, win->y, win->w, disp_h );
        s_scope.clip = ( gui_rect_t ){ win->x, win->y, win->w, disp_h };

        /* Window body background.  Skipped for a frame-only shell: its body stays empty so the
           borderless viewport shows the cleared surface (and the windows inside it) through it. */
        if ( !frame_only )
            draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );

        /* Menu-bar strip: when WIN_MENUBAR is set, reserve one row below the title bar for
           menu_bar_begin to fill.  Carved off the top of the body here -- before the scroll
           region opens -- so it sits above the scrolling content and never moves.  The rect is
           stashed in s_build for menu_bar_begin; mb_h is 0 (no reservation) otherwise. */
        f32 mb_h = ( flags & GUI_WIN_MENUBAR ) ? ( WIDGET_H + WIDGET_GAP ) : 0.0f;
        s_build.win.menubar_rect = ( gui_rect_t ){ win->x, win->y + title_h, win->w, mb_h };

        /* Open the body as a scroll region.  Its region id is the window id, so the body
           scrollbar ids stay exactly what the window used before unification.  The region owns
           the pen, content column, wheel, and bars until layout_pop_region in window_end, but
           reuses the window's single clip.  Bias-from-scroll, gutter reservation, and clamping
           all live there now. */
        gui_rect_t body = { win->x, win->y + title_h + mb_h, win->w, win->h - title_h - mb_h };
        layout_push_region( id, body, REGION_PAD_DEFAULT, body_flags, &win->scroll,
                            /* own_clip */ false );
    }
    else
    {
        /* Collapsed: no body region opens and no draw clip is pushed, but window_end still
           hit-tests the collapse arrow through s_scope.clip.  Left unset it would inherit
           whatever clip the previously drawn window left behind -- which need not cover this
           title bar, so the arrow goes intermittently dead (it only "works" when the stale clip
           happens to contain it).  Point it at the shown title-bar rect so the arrow is always
           hittable; the deferred chrome in window_end draws within these bounds without a clip. */
        s_scope.clip = ( gui_rect_t ){ win->x, win->y, win->w, disp_h };
    }

    /* false tells the caller to skip its body widgets (they would do nothing anyway). */
    return !collapsed;
}

bool
gui_window_begin( const char* title, gui_win_flags_t flags )
{
    return window_begin_ex( id_hash( title ), title, 60.0f, 60.0f, 240.0f, 320.0f, flags );
}

/*----------------------------------------------------------------------------------------------
    viewport_shell -- the chrome for a borderless viewport, as one self-contained emit.

    The public front door for GUI_WIN_NATIVE: a frame-only shell window whose titlebar stands in
    for the OS caption and whose border is the sizing frame, with an empty, click-through body.
    Emitted first in the build so the caption band it publishes (caption_inset) is live for
    everything after it -- main_menu_bar, window clamping, and the dock tree all read it.

    No-op returning 0 when the viewport's OS window has its own chrome: the host calls this
    unconditionally and selects the mode solely with APP_WIN_BORDERLESS at window_open.
----------------------------------------------------------------------------------------------*/

f32
gui_viewport_shell( gui_vp_t vp, const char* title, gui_win_flags_t flags )
{
    if ( vp >= g_ctx->vp.max )
        return 0.0f;

    if ( !app()->window_is_borderless( g_ctx->vp.pool[ vp ].win_id ) )
        return 0.0f;    /* OS-chrome window: the OS draws the frame, no shell needed */

    gui_window_set_next_viewport( vp );
    gui_window_begin( title, GUI_WIN_NATIVE | GUI_WIN_NOSCROLL | flags );
    gui_window_end();

    return g_ctx->vp.pool[ vp ].caption_inset;   /* published by the begin above */
}

/* gui_window_end (the deferred chrome: titlebar, buttons, border, resize grip, move grab, and
   drag-to-dock commit) lives in gui_window_end.c, included just after this file -- it reuses
   window_fit_bounds / window_fit_size for the auto-fit grip and the GUI_COLLAPSE/DETACH/CLOSE_SALT
   ids defined above. */

// clang-format on
/*============================================================================================*/
