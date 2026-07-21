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
    in present/gui_paint_core.c (col_item_bg, the label grammar) and interact/gui_item.c
    (item_state); the style vocabulary (WIDGET_* / WIN_* / COL_*) resolves in
    core/gui_style.c.

    Included by gui.c after the widget family files, so the window record (gui_window.c), the
    shared widget core, and the leaf widgets are all in scope.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Per-window widget ids -- salted from the window id so a window's chrome controls never
    collide with each other or with a title-hashed widget id inside the same window.
==============================================================================================*/

/* Scrollbar salts (GUI_SCROLLBAR_SALT / GUI_HSCROLLBAR_SALT) and the scrollbar widget
   itself live in widgets/gui_scrollbar.c -- the window body is just a region whose engine
   emits them into its gutters. */

/* Collapse arrow: a distinct stable per-window widget id. */
#define GUI_COLLAPSE_SALT   0xC011A95Eu

/* Detach / reattach button (title-bar right edge): a distinct stable per-window widget id. */
#define GUI_DETACH_SALT     0xDE7AC405u

/* Close button (title-bar right edge, outermost): a distinct stable per-window widget id. */
#define GUI_CLOSE_SALT      0xC105E00Du

/* Maximize / minimize buttons (between close and detach): distinct stable per-window widget
   ids.  The minimize salt doubles as the shelf chip's restore button id. */
#define GUI_MAXIMIZE_SALT   0xB166E557u
#define GUI_MINIMIZE_SALT   0x53A11E57u

/* The native-borderless (GUI_WIN_NATIVE) machinery -- window_is_native / window_native_id, the
   caption-button layout + glyphs, window_sync_native, native_popin_request, and the caption strip
   window_end calls (native_caption_chrome) -- lives in gui_window_native.c, included just
   before this file. */

/* GUI_RESIZE_SALT, the RESIZE_BAND_* grab-band constants, and the record-agnostic resize helpers
   (resize_edge_hit, resize_grab, resize_apply_edges) live in interact/gui_resize.c (the
   GUI_RESIZE_* edge bits in gui_internal.h, the hot-edge paint in present/gui_paint_core.c:
   draw_resize_highlight), ahead of gui_layout.c, so child_begin reuses the same mechanism (the
   dock splitter does not; it has its own drag path in dock/).  Only the
   window's size policy stays below: the min clamp with far-edge pinning (window_apply_resize) and
   the content auto-fit (window_fit_size). */

/*==============================================================================================
    window_begin / window_end
==============================================================================================*/

/* Top of the viewport work area: the native caption band plus the main-menu-bar band on frames
   the host emits one (one-frame tolerance -- the bar may emit after this window builds, the
   same lag hover_win runs on).  Shared by the maximize pin, window_clamp below, and the
   floating-group clamp (dock/gui_dock_float.c). */
static f32
window_work_top( const gui_viewport_t* vp )
{
    f32 top = vp->caption_inset;
    if ( vp->bar_seen_frame != 0u && vp->bar_seen_frame + 1u >= g_ctx->retained.frame )
        top += vp->bar_inset;
    return top;
}

/* Public query twin of window_work_top (gui_api.h viewport_content_y): where host content
   starts on a viewport -- 0 OS-chrome, + caption band gui-shelled, + menu bar when emitted. */
f32
gui_viewport_content_y( gui_vp_t vp )
{
    if ( !g_ctx || vp >= g_ctx->vp.max )
        return 0.0f;
    return window_work_top( &g_ctx->vp.pool[ vp ] );
}

/* Keep a dragged window reachable within its own viewport's work area.  The geometry is the
   feat kit's clamp mechanism (gui_feat_clamp, interact/gui_feature.c); this wrapper is the
   window's POLICY: which surface bounds apply (the window's own viewport, so dragging on a
   secondary surface clamps against that surface), where the work area starts (window_work_top
   -- a titlebar may not slide under the OS caption band or the main menu bar, where the grab
   would be lost), the title-bar margin, and the GUI_WIN_NO_BOUNDARY_CLAMP opt-out. */
static void
window_clamp( gui_window_t* win )
{
    if ( win->flags & GUI_WIN_NO_BOUNDARY_CLAMP )
        return;

    const gui_viewport_t* vp  = &g_ctx->vp.pool[ win->viewport ];
    const f32             top = window_work_top( vp );

    gui_rect_t r = { win->x, win->y, win->w, win->h };
    gui_feat_clamp( &r, ( gui_rect_t ){ 0.0f, top, vp_w( vp ), vp_h( vp ) - top }, WIN_TITLE_H );
    win->x = r.x;
    win->y = r.y;
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

/*==============================================================================================
    Maximize / minimize (regular floaters)

    Window-record state, resolved every frame in window_begin_ex ahead of the drag / resize
    gestures (both are suppressed while either state holds the geometry).  Maximized pins the
    window to its surface work area -- below the native caption band and the main menu bar -- so
    it tracks live surface resizes; minimized parks it as a title-bar chip on a shelf packing
    left-to-right along the surface's bottom edge, rendering through the collapsed
    (title-bar-only) path.  The buttons, the double-click toggle, and the chip chrome live in
    gui_window_end.c; a native window uses the OS states instead and a docked window never
    offers either (the node owns its chrome).
==============================================================================================*/

/* window_work_top (the caption band + main-menu-bar top bound both the maximize pin and
   window_clamp share) is defined above window_clamp, which needs it first. */

/*==============================================================================================
    State-transition animation

    Maximize / minimize / restore and the collapse height do not snap between states: the rect
    channel rides the feat kit's pin mechanism (feat_pin, interact/gui_feature.c -- the core
    under gui_feat_maximize, generalized to this window's three states: normal / maximized /
    shelf chip) and the body height rides gui_feat_collapse.  Both edge-detect the state bools
    the setters below flip, capture their tween scratch in the keyed state pool, and ease on
    the gui_anim_timer clock (FEAT_ANIM_SECS / feat_ease -- the one window-feel constant, so
    stock chrome and feature-built pane chrome move identically).  The setters are the POLICY
    choke points every button and double-click routes through: they flip the state, order the
    z / shelf bookkeeping, and flag the redraw; the geometry is the mechanisms' from here on.

    Animation is a user choice: s_win_anim (gui_window_anim_enable) gates it globally -- the
    mechanisms read it through gui_window_anim_is_enabled and seed a zero-duration timer when
    off, so the same code path snaps instantly.
==============================================================================================*/

/* Global toggle: window state transitions animate when set (default), snap when clear.  A process-
   wide preference like idle-skip, not per-context -- see gui_window_anim_enable in the API block. */
static bool s_win_anim = true;

/* Toggle collapse.  Flips the logical state and flags the redraw; the height tween is
   gui_feat_collapse's edge detect on the next window_begin, easing from the height it showed
   last frame (so a toggle mid-tween continues seamlessly).  The single choke point the arrow
   click and the double-click both route through. */
static void
window_collapse_set( gui_window_t* win, bool on )
{
    if ( win->collapsed == on )
        return;

    win->collapsed = on;
    g_ctx->retained.wants_redraw = true;
}

/* Public toggle for window state-transition animation (maximize / minimize / restore).  A global
   preference like idle-skip; off snaps instantly through the zero-duration timer path.  An
   in-flight tween is left to finish -- the switch governs transitions started after it. */
void gui_window_anim_enable    ( bool on ) { s_win_anim = on; }
bool gui_window_anim_is_enabled( void )    { return s_win_anim; }

/* Shelf chip width -- wide enough for a legible title, resting on the grid lattice. */
static f32
window_shelf_chip_w( void ) { return lat_ceil( WIN_TITLE_H * 7.0f, s_style.grid_quantum ); }

/* A window occupies a shelf slot only while LIVE: minimized and emitted this frame or last (the
   window_begin recency test `appearing` uses).  A closed chip (its last_frame deliberately
   freezes) or one the host stopped emitting releases its slot while hidden -- otherwise its
   ghost would hold the shelf position and every later chip would pack one slot too far right. */
static bool
window_shelf_occupies( const gui_window_t* o, const gui_window_t* win )
{
    return o != win && o->id != 0 && o->minimized && o->viewport == win->viewport
        && o->last_frame + 1u >= g_ctx->retained.frame;
}

/* Chip paint position on the shelf: the count of live minimized windows on this surface ahead of
   this one -- ordered by the slot key taken at minimize time, compacting as neighbours restore. */
static u32
window_shelf_order( const gui_window_t* win )
{
    u32 order = 0;
    for ( u32 i = 0; i < g_ctx->win.count; ++i )
    {
        const gui_window_t* o = &g_ctx->win.pool[ i ];
        if ( window_shelf_occupies( o, win ) && o->shelf_slot < win->shelf_slot )
            ++order;
    }
    return order;
}

/* Next free shelf slot on win's surface -- one past the highest live occupant. */
static u32
window_shelf_take_slot( const gui_window_t* win )
{
    u32 slot = 0;
    for ( u32 i = 0; i < g_ctx->win.count; ++i )
    {
        const gui_window_t* o = &g_ctx->win.pool[ i ];
        if ( window_shelf_occupies( o, win ) && o->shelf_slot >= slot )
            slot = o->shelf_slot + 1u;
    }
    return slot;
}

/* Toggle maximize.  Entering raises the window so it covers everything on its surface (bodies
   are opaque, so occlusion and hover both follow from z).  The rect ease -- and the norm save
   on the way up -- are feat_pin's edge detect on the next window_begin_ex; a hop to or from
   the minimized state keeps the pin nonzero, so the first save survives (minimized owns the
   restore then, exactly the old rule). */
static void
window_maximize_set( gui_window_t* win, bool on )
{
    if ( win->maximized == on )
        return;

    if ( on )
        win->z = surface_z_raise( win->z );

    win->maximized = on;
    g_ctx->retained.wants_redraw = true;   /* takes effect next frame; force one more build */
}

/* Toggle minimize.  Entering takes the next free shelf slot on this surface; leaving raises the
   window.  As with maximize, the rect ease and the norm save / restore are feat_pin's edge
   detect on the next window_begin_ex. */
static void
window_minimize_set( gui_window_t* win, bool on )
{
    if ( win->minimized == on )
        return;

    if ( on )
        win->shelf_slot = window_shelf_take_slot( win );
    else
        win->z = surface_z_raise( win->z );

    win->minimized = on;
    g_ctx->retained.wants_redraw = true;
}

/*==============================================================================================
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
==============================================================================================*/

/* Smallest width a window may be shrunk to. */
static f32 window_min_w( void ) { return WIN_TITLE_H * 4.0f; }

/* Smallest height: always keeps the title bar fully visible plus one widget row of body, so
   a resize never eats into the title bar vertically.  title_h is 0 for a NOTITLEBAR window. */
static f32 window_min_h( f32 title_h ) { return title_h + WIDGET_H + WIN_BORDER; }

/* Snap a window position or extent onto the grid lattice (nearest), so a window rests only on snap
   thresholds -- its interior content column then always lands on the same lattice the theme metrics
   and resolved tracks do, and widget widths never jump mid-resize as the free-pixel width crosses a
   lattice line.  Movement/resize reads blocky by one quantum step; a future settle-lerp (gui_anim.c)
   would smooth it.  q<=1 or GUI_GRID_LATTICE=0 -> identity (free-pixel windows, the pre-grid feel).
   The per-gesture callers snap the MOVING edge only and hold the pinned far edge fixed, so a snap
   never drags the opposite edge. */
static f32 window_snap( f32 v ) { return lat_round( v, s_style.grid_quantum ); }

/*==============================================================================================
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
==============================================================================================*/

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

    /* Rest the autosized window on the lattice too, so its edges align with the manually-resized
       case.  Ceil (not round) so the hug never crops the content it was measured to hold; the
       content extent is already lattice-ceiled, so this only absorbs the sub-quantum chrome slack
       (the free-pixel border). */
    u32 q = s_style.grid_quantum;
    *out_w = lat_ceil( want_w, q );
    *out_h = lat_ceil( want_h, q );
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

    /* Snap the moving edge onto the lattice, holding the pinned far edge fixed: a right/bottom drag
       snaps the extent out from the fixed origin; a left/top drag snaps the origin and recovers the
       extent against the pinned far edge (s_resize_fix_*, itself on the lattice from the last rest).
       Both edges land on the grid, so the window's content column stays lattice-aligned as it grows. */
    if ( s_resize_edges & GUI_RESIZE_R ) win->w = window_snap( win->w );
    if ( s_resize_edges & GUI_RESIZE_L ) { win->x = window_snap( win->x ); win->w = s_resize_fix_x - win->x; }
    if ( s_resize_edges & GUI_RESIZE_B ) win->h = window_snap( win->h );
    if ( s_resize_edges & GUI_RESIZE_T ) { win->y = window_snap( win->y ); win->h = s_resize_fix_y - win->y; }

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

/*==============================================================================================
    window_begin_ex helpers

    window_begin_ex is one long linear sequence of independent per-frame resolves (native sync,
    drag, tear-off, resize).  Each stage only touches its own inputs and s_build / g_ctx -- pulled
    out here so each is readable and named on its own, the same way window_clamp / window_apply_resize
    / window_fit_size already are above.
==============================================================================================*/

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
        win->x = window_snap( win->x );   /* rest on the lattice: blocky move, size preserved */
        win->y = window_snap( win->y );   /* (both origin coords snap; extent is untouched) */
    }
    else
    {
        i32 cx = 0, cy = 0;
        f32 nx = 0.0f, ny = 0.0f;
        app()->mouse_position_screen( &cx, &cy );
        move_track( id, (f32)cx, (f32)cy, &nx, &ny );
        app()->window_set_pos( g_ctx->vp.pool[ win->viewport ].win_id,
                               (i32)window_snap( nx ), (i32)window_snap( ny ) );
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
   resolves before s_build.win.id is stamped.  Sets s_scope.resize_hot (read by item_state +
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
    s_scope.resize_hot = resize_hot;   /* read by item_state + window_end's highlight */

    /* CAN_AUTOSIZE size-grip: reserve the bottom-right corner ahead of the body's scrollbars the
       same way the edge band reserves the borders.  The grip square overlaps the scroll gutter, but
       the scrollbar runs first (in layout_pop_region), so without this it would claim the press and
       the grip -- drawn and grabbed later in window_end -- would sit dead behind it.  Suppressing
       widget hover over the grip rect leaves active_id free for window_end to grab. */
    bool grip_hot = false;
    bool grip_eligible = ( flags & GUI_WIN_CAN_AUTOSIZE ) && !collapsed
                         && s_interaction.hover_win == id
                         && ( interact_idle() || interact_held( resize_id ) );
    if ( grip_eligible )
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

/* Apply an in-progress edge resize (active_id is the resize-salted window id).  Runs after the drag
   apply -- the two are mutually exclusive, only one can own active_id at a time.  A native floater
   derives local coords from the stable screen cursor and its fixed screen origin (bottom-right
   resize pins the top-left), because s_io.mouse_x/y is only valid while the cursor is over THIS
   window and the drag routinely leaves the floater -- reading mouse_position() then would return
   coords in whatever window the cursor crossed into and compute the wrong (usually too small) size. */
static void
window_apply_resize_gesture( gui_window_t* win, gui_id_t id, bool native, f32 title_h )
{
    if ( !interact_held( id_combine( id, GUI_RESIZE_SALT ) ) )
        return;

    if ( native && win->viewport != 0 )
    {
        win_id_t os = window_native_id( win );
        i32 scx = 0, scy = 0, sox = 0, soy = 0;
        app()->mouse_position_screen( &scx, &scy );
        app()->window_get_pos( os, &sox, &soy );
        f32 local_x = (f32)scx - (f32)sox;
        f32 local_y = (f32)scy - (f32)soy;
        f32 new_w   = window_snap( local_x - s_resize_off_x );   /* rest the OS size on the lattice */
        f32 new_h   = window_snap( local_y - s_resize_off_y );
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

/* Open the window body -- or, when collapsed, just seed the collapse-arrow clip.  Expanded: push the
   one clip rect the whole window shares, fill the body background (skipped for a frame-only shell so
   the borderless viewport shows through), reserve the menu-bar strip, and open the body scroll
   region.  Collapsed: no region opens and no clip is pushed, but s_scope.clip is pointed at the shown
   title bar so window_end's collapse arrow stays hittable instead of inheriting the previous
   window's stale clip (which need not cover this bar, leaving the arrow intermittently dead). */
static void
window_open_body( gui_window_t* win, gui_id_t id, gui_win_flags_t flags, f32 title_h, f32 disp_h,
                  bool collapsed )
{
    bool            frame_only = ( flags & GUI_WIN_NATIVE ) != 0;
    gui_win_flags_t body_flags = ( flags & GUI_WIN_ALWAYS_AUTOSIZE ) ? ( flags | GUI_WIN_NOSCROLL ) : flags;

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
           borderless viewport shows the cleared surface (and the windows inside it) through it.
           A maximized body fills flush to the surface edges: square corners, or the radius would
           open gaps at the surface's bottom corners. */
        if ( !frame_only )
        {
            f32 save_round = draw_rounding();
            if ( win->maximized && !win->minimized )
                draw_set_rounding( 0.0f );
            draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );
            draw_set_rounding( save_round );
        }

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

        /* Text selection (GUI_WIN_TEXT_SELECT): paint the selection bands UNDER the content
           about to emit (opaque, editor-style, from last frame's captured runs); interaction
           and the marquee outline run at window_end. */
        if ( flags & GUI_WIN_TEXT_SELECT )
            select_paint_under();
    }
    else
    {
        s_scope.clip = ( gui_rect_t ){ win->x, win->y, win->w, disp_h };
    }
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
    s_build.win.minimized = false;   /* default; the shelf-chip branch below flips it */

    /* GUI_WIN_MODAL: pin into the overlay z-band above every normal window and register the
       fence so the next ctx_begin makes everything behind inert (window_modal_apply).  Stamped
       here like the popup layer stamps its overlays before this call -- win->overlay is the TYPE
       fact that keeps the appearing-raise (below) and raise-on-press from sinking it out of the
       band.  Cleared implicitly: a modal that stops emitting stops re-stamping seen_frame. */
    if ( flags & GUI_WIN_MODAL )
    {
        win->z       = surface_z_overlay( 0u );
        win->overlay = true;
        g_ctx->modal.win_id     = id;
        g_ctx->modal.seen_frame = g_ctx->retained.frame;
    }

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

    /* Maximize / minimize live on a regular floater's title bar only: a native window has the OS
       states, an overlay (popup / tooltip) has no such chrome.  A window that loses eligibility
       while in either state (flag change, turned native) exits through the setters so its normal
       geometry comes back. */
    if ( !has_titlebar || native || win->overlay )
    {
        window_minimize_set( win, false );
        window_maximize_set( win, false );
    }

    bool can_collapse = has_titlebar && !( flags & GUI_WIN_NOCOLLAPSE ) && !native && !win->maximized;
    if ( !can_collapse ) win->collapsed = false;

    /* Logical collapse: the state the arrow reflects and the autosize refit respects.  The VISUAL
       collapse (title-bar only this frame) is resolved below from disp_h once the height tween has
       run -- during a collapse animation the two differ, the window still showing a shrinking body. */
    bool logical_collapsed = win->collapsed || win->minimized;

    /* ALWAYS_AUTOSIZE owns its own geometry: it cannot be user-resized and shows no scrollbars
       (the body always fits), and its size is recomputed below from the measured content.  The
       body region is opened with NOSCROLL (in window_open_body) so it never reserves a gutter. */
    bool autosize = ( flags & GUI_WIN_ALWAYS_AUTOSIZE ) != 0;

    /* State-pinned geometry, refreshed every frame so both states track live surface resizes.
       Minimized: a title-bar chip on the surface's bottom shelf, packing left-to-right (the body
       renders through the collapsed path via `collapsed` above).  Maximized: fill the work area
       below the caption band + main menu bar.  While pinned, the drag / tear-off gestures below
       are skipped -- a title drag of a maximized window instead restores it first and re-grabs
       (window_end_titlebar_poll, gui_window_end.c). */
    bool pinned = win->minimized || win->maximized;

    /* The rect channel is the feat kit's pin mechanism (feat_pin, interact/gui_feature.c): the
       target is recomputed here every frame (so a settled pin tracks surface resizes and shelf
       reorders), the state ordinal is this window's policy fact, and the mechanism edge-detects
       the setters' flips -- saving / restoring win->norm and easing between the states.  Height
       is held out of the minimized target -- the body renders title-bar-only via `collapsed`,
       so win->h is preserved for an instant restore.  pin_live is true while a transition (or
       the restore) eases -- the gesture gates below suppress on it so the ease is never fought. */
    gui_rect_t pin_target = { 0 };
    u32        pin_state  = 0;
    if ( win->minimized )
    {
        /* A re-appearing chip (re-opened after close, or the host resumed emitting it) re-takes
           the next free slot: its old slot was released while hidden and may be occupied now --
           two chips sharing a slot key would tie in window_shelf_order and paint on top of each
           other. */
        if ( appearing )
            win->shelf_slot = window_shelf_take_slot( win );

        const gui_viewport_t* vp = &g_ctx->vp.pool[ win->viewport ];
        f32 chip_w = window_shelf_chip_w();
        pin_state  = 2u;
        pin_target = ( gui_rect_t ){
            (f32)window_shelf_order( win ) * ( chip_w + WIDGET_GAP ), vp_h( vp ) - title_h, chip_w, win->h
        };
    }
    else if ( win->maximized )
    {
        const gui_viewport_t* vp  = &g_ctx->vp.pool[ win->viewport ];
        f32  top   = window_work_top( vp );
        pin_state  = 1u;
        pin_target = ( gui_rect_t ){ 0.0f, top, vp_w( vp ), vp_h( vp ) - top };
    }

    gui_rect_t pin_r    = { win->x, win->y, win->w, win->h };
    bool       pin_live = feat_pin( id, pin_state, &pin_r, &win->norm, pin_target );
    win->x = pin_r.x;  win->y = pin_r.y;  win->w = pin_r.w;  win->h = pin_r.h;

    /* The drag / resize gestures are suppressed while a transition eases so a half-finished
       ease is never fought (a pinned window already suppresses them). */
    if ( !pinned && !pin_live )
    {
        window_apply_drag( win, id );                           /* in-progress title-bar drag */
        window_apply_tearoff_gesture( win, id, title, flags );  /* tear-off / merge-back / drag-to-dock */
    }

    /* Apply an in-progress edge resize (mutually exclusive with the drag apply above). */
    window_apply_resize_gesture( win, id, native, title_h );

    /* ALWAYS_AUTOSIZE: hug the content measured last frame (held in win->content_*).  Skipped while
       collapsed (the title-bar-only height is preserved) and on the very first appearance, before
       any content has been measured -- then the caller's initial w/h stands for one frame. */
    f32 fit_mb_h = ( flags & GUI_WIN_MENUBAR ) ? ( WIDGET_H + WIDGET_GAP ) : 0.0f;
    if ( autosize && !logical_collapsed && !feat_collapse_live( id ) && !win->maximized && !pin_live
         && win->scroll.content_h > 0.0f )
    {
        f32 max_w, max_h;
        window_fit_bounds( win, &max_w, &max_h );
        window_fit_size( title, title_h, fit_mb_h, can_collapse, win->scroll.content_w, win->scroll.content_h,
                         max_w, max_h, &win->w, &win->h );
    }

    /* disp_h is the body height actually shown this frame -- it drives the hover rect, clip, and
       border.  A minimized chip and a maximized window take their pinned heights directly; a normal
       window runs the collapse height tween (gui_feat_collapse, interact/gui_feature.c), which
       eases between win->h and title_h so a collapse / expand animates instead of snapping.  win->h
       is preserved throughout so an expand restores the previous size.  The mechanism is sampled
       EVERY frame -- pinned states included, discarding the value -- so its edge latch stays in
       step with win->collapsed (a forced re-open while maximized consumes its tween invisibly).
       `collapsed` is then the VISUAL fact -- no body space left this frame -- which balances
       window_open_body's region against window_end and gates the caller's body emit; while the
       tween still shows a sliver of body it stays false so the content clips through. */
    f32 tween_h = gui_feat_collapse( id, !win->collapsed, title_h, win->h );
    f32 disp_h;
    if ( win->minimized )       disp_h = title_h;
    else if ( win->maximized )  disp_h = win->h;
    else                        disp_h = tween_h;

    bool collapsed = disp_h <= title_h + 0.5f;

    /* Edge resize, resolved here so it pre-empts the scrollbar and collapse arrow (resolved in
       window_end) underneath: while the cursor sits on a hot edge, s_scope.resize_hot suppresses
       every widget hover in this window, and a press grabs the resize before any widget can.
       Gated on hover_win (last frame's front-most), so only the top window's edges go hot. */
    gui_rect_t disp_r    = { win->x, win->y, win->w, disp_h };
    gui_id_t   resize_id = id_combine( id, GUI_RESIZE_SALT );
    bool         resizeable = !( flags & GUI_WIN_NORESIZE ) && !autosize && !native && !pinned
                              && !pin_live && !feat_collapse_live( id );
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

    /* The pane open (surface/gui_surface.c): all of this window's geometry is stamped with its
       z so flush can paint windows back-to-front regardless of window_begin call order, and
       with its viewport so flush dispatches it to the hosting surface; the interaction scope
       is committed alongside (this window owns the items that follow).  Ordered before
       DBG_RESIZE so the capture lands on the correct per-viewport list.  A free window IS a
       pane + the persisted record + the chrome policy below. */
    pane_tag( id, win->z, win->viewport, ( flags & GUI_WIN_DEBUG_BAND ) ? 1u : 0u );

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

    /* Commit window chrome state for the widgets and window_end (id + interaction scope were
       committed by pane_tag above).  The layout pen, content column, scroll, and scrollbars
       are all owned by the body region opened below -- the window no longer resolves any of
       that itself; it is just the root region plus chrome. */
    s_build.win.title     = title;   /* cached for window_end's deferred chrome */
    s_build.win.collapsed = collapsed;
    s_build.win.minimized = win->minimized;   /* shelf chip: window_end swaps in the chip chrome */
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
    window_open_body( win, id, flags, title_h, disp_h, collapsed );

    /* false tells the caller to skip its body widgets (they would do nothing anyway). */
    return !collapsed;
}

/* Default spawn cascade: each window that first appears WITHOUT an explicit position lands one
   title-bar step down-right of the previous default spawn, OS-style, so windows opened in
   sequence stagger instead of stacking on one point.  The run starts at a fixed inset below the
   viewport work area (caption band + main menu bar, window_work_top) and wraps back to that
   first slot once the next position would cross half the viewport extent on either axis. */
static void
window_default_spawn( u32 viewport, f32* out_x, f32* out_y )
{
    const gui_viewport_t* vp = &g_ctx->vp.pool[ viewport ];
    const f32 inset = 60.0f;
    const f32 step  = WIN_TITLE_H;
    const f32 top   = window_work_top( vp );

    f32 x = inset + step * (f32)g_ctx->win.cascade;
    f32 y = top + inset + step * (f32)g_ctx->win.cascade;
    if ( x > vp_w( vp ) * 0.5f || y > top + vp_h( vp ) * 0.5f )
    {
        g_ctx->win.cascade = 0;
        x = inset;
        y = top + inset;
    }
    ++g_ctx->win.cascade;

    *out_x = window_snap( x );
    *out_y = window_snap( y );
}

bool
gui_window_begin( const char* title, gui_win_flags_t flags )
{
    gui_id_t id = id_hash( title );

    /* Only a window appearing for the FIRST time with no queued explicit position consumes a
       cascade slot: a re-begin of an existing record ignores the passed geometry (the registry
       owns it), and a window_set_next_pos seed lands where it asked -- neither should advance
       the cascade.  The spawn viewport is the queued one when set, else the ambient the new
       record will inherit. */
    f32 x = 60.0f, y = 60.0f;
    if ( !window_find( id ) && !s_next_win.has_pos && g_ctx->win.count < g_ctx->win.max )
    {
        /* The pool-full guard keeps a scratch-hosted overflow window (window_find never sees it,
           so it reads as appearing EVERY frame) from advancing the cascade and walking across
           the screen; it takes the fixed fallback above instead. */
        u32 vp = s_next_win.has_viewport ? s_next_win.viewport : s_build.win.viewport;
        window_default_spawn( vp, &x, &y );
    }

    return window_begin_ex( id, title, x, y, 240.0f, 320.0f, flags );
}

/*==============================================================================================
    viewport_shell -- the chrome for a borderless viewport, as one self-contained emit.

    The public front door for GUI_WIN_NATIVE: a frame-only shell window whose titlebar stands in
    for the OS caption and whose border is the sizing frame, with an empty, click-through body.
    Emitted first in the build so the caption band it publishes (caption_inset) is live for
    everything after it -- main_menu_bar, window clamping, and the dock tree all read it.

    No-op returning 0 when the viewport's OS window has its own chrome: the host calls this
    unconditionally and selects the mode solely with APP_WIN_BORDERLESS at window_open.
==============================================================================================*/

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
