/*==============================================================================================

    runtime_service/gui/frame/gui_viewport.c -- Viewport lifecycle and gui-owned floater surfaces.

    Open a viewport: claim the slot at win_id (slot index == win_id), create its GPU geometry
    buffers, and record the OS window and initial drawable size.

    Slot alignment: win_id 0 = primary swapchain, win_id 1..N = secondary surfaces.
    Since each viewport requires a live OS window, the window pool guarantees the matching slot is free.
    RHI_SWAPCHAIN_COLOR resolves per-context at flush time -- which cmd you pass render() selects
    the swapchain.

    Returns the handle to pass to render / viewport_resize / viewport_close / window_set_next_viewport,
    or GUI_VP_INVALID on bad win_id or GPU buffer failure.
    Must be called after init() and before frame_begin().

    Also owns the gui-owned floater lifecycle (viewport_spawn / update / render_floaters):
    unlike viewport_open (the host hands gui a window + context to flush into), a floater owns
    the OS window + rhi context end to end -- gui creates it on spawn and destroys it on close.
    The tear-off gesture drives spawn/close; a host/sandbox may also call gui_viewport_spawn
    directly to place a panel in its own OS window.

    Included by the gui_frame.c unit root after gui_frame_loop.c: gui_viewport_render_floaters
    calls gui_render(), defined there.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Surface record lifecycle (the pane-handoff completion)

    The render server mints and frees only the GPU geometry ring (surface_geo_create/destroy,
    render/gui_render.h); every other field of the surface record (gui_viewport_t,
    core/gui_ctx.h) is orchestration -- OS window, rhi context, routing, dock policy -- and
    the orchestrator owns it here.  Both take a slot INDEX into the one global s_vp_pool, not a
    context: a viewport is a real OS window / RHI context, not context-owned state, so there is
    no context to be agnostic of anymore.
==============================================================================================*/

bool
viewport_create( gui_vp_t vp, rhi_texture_t target, i32 win_id )
{
    gui_viewport_t* v = &s_vp_pool[ vp ];
    v->target          = target;
    v->win_id          = win_id;           // OS window hosting this surface; -1 = unassociated
    v->rhi_ctx         = RHI_CTX_INVALID;  // set only by viewport_spawn for an gui-owned floater
    v->owned           = false;            // host-provided unless viewport_spawn flips it
    v->pending_close   = false;            // owned floater close request; serviced by viewport_update
    v->disp_w          = 0;                // drawable size set by the host before build; 0 = fall back to main
    v->disp_h          = 0;
    v->caption_inset   = 0.0f;             // no native shell band until one publishes it during the build
    v->dock_inset      = 0.0f;             // no host menu/toolbar band until one publishes it
    v->dock_root       = GUI_DOCK_REF_NONE; // free-float until docking assigns a tree
    v->dock_seen_frame = 0;                 // never emitted; frame clock starts at 1 so 0 = dormant

    return surface_geo_create( &v->vb, &v->ib );
}

void
viewport_destroy( gui_vp_t vp )
{
    gui_viewport_t* v = &s_vp_pool[ vp ];

    /* Owned floater: destroy the rhi context FIRST.  context_destroy idles the GPU (waits the device)
       before tearing down its swapchain/sync, so the geometry-buffer frees below are then safe --
       matching the host's own shutdown order (drain, free buffers, close window).  A host-provided
       surface (owned == false) leaves its context to the host; gui frees only the GPU buffers it
       created via viewport_create. */
    if ( v->owned && v->rhi_ctx != RHI_CTX_INVALID )
        rhi()->context_destroy( v->rhi_ctx );

    surface_geo_destroy( &v->vb, &v->ib );

    // Owned floater: close the OS window gui opened, only after the context (and its swapchain) is gone.
    if ( v->owned && v->win_id >= 0 )
        app()->window_close( v->win_id );

    v->win_id        = -1;            // slot freed -> no window matches it for input routing
    v->rhi_ctx       = RHI_CTX_INVALID;
    v->owned         = false;
    v->pending_close = false;
}

/*==============================================================================================
    Viewport API
==============================================================================================*/

gui_vp_t
gui_viewport_open( i32 win_id )
{
    GUI_CONTRACT( s_gui_ready, "viewport_open() before a successful init() -- gui has no GPU "
                               "resources to build a surface from yet.\n" );
    if ( !s_gui_ready )
        return GUI_VP_INVALID;

    /* Slot index == win_id; an open window guarantees the slot is free. */
    GUI_CONTRACT( win_id >= 0 && win_id < (i32)GUI_MAX_VIEWPORTS,
                  "viewport_open( %d ): win_id outside [0, %u).\n", win_id, GUI_MAX_VIEWPORTS );
    if ( win_id < 0 || win_id >= (i32)GUI_MAX_VIEWPORTS )
        return GUI_VP_INVALID;

    gui_viewport_t* vp = &s_vp_pool[ win_id ];

    /* A second open on a live slot would strand the first surface's GPU buffers with no handle
       left to reach them -- refuse instead of leaking. */
    bool slot_free = !rhi_handle_valid( vp->vb );
    GUI_CONTRACT( slot_free, "viewport_open( %d ): that slot is already open.\n", win_id );
    if ( !slot_free )
        return GUI_VP_INVALID;

    /* The window's rhi context must exist first: gui flushes into ITS swapchain, and the slot
       convention is index == win_id == rhi context id.  Without it every render() on this
       viewport is a silent no-op.  The query doubles as the size read below. */
    i32  w = 0, h = 0;
    bool ctx_live = rhi()->context_size( win_id, &w, &h );
    GUI_CONTRACT( ctx_live, "viewport_open( %d ): no live rhi context for that window -- call "
                            "rhi()->context_open( win ) before attaching a viewport to it.\n",
                            win_id );
    if ( !ctx_live )
        return GUI_VP_INVALID;

    if ( !viewport_create( (gui_vp_t)win_id, ( rhi_texture_t ){ .id = RHI_SWAPCHAIN_COLOR }, win_id ) )
        return GUI_VP_INVALID;

    /* Size from the rhi context, not app(): the swapchain extent IS what gui flushes into, so
       the two start the frame in agreement (the render-time size check reads the same pair). */
    vp->disp_w = w;
    vp->disp_h = h;

    // Update the high-water viewport count so the host can enumerate live viewports.
    if ( (u32)win_id + 1u > s_vp_count )
        s_vp_count = (u32)win_id + 1u;

    redraw_request();   /* a fresh surface has no cached geometry to replay -- the first build
                           after it opens must actually run, whatever the input did. */
    return (gui_vp_t)win_id;
}

/* The caption band height (px) a chrome shell published on this viewport -- 0 for an OS-chrome
   window or before the shell's first emit.  Hosts stack their own pinned strips (menu bar,
   toolbar) below it; the built-in main_menu_bar, window clamping, and the dock tree already
   inset themselves.  Sticky across frames (see gui_viewport_t.caption_inset). */
f32
gui_viewport_caption_h( gui_vp_t vp )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return 0.0f;
    return s_vp_pool[ vp ].caption_inset;
}

/* A viewport's current drawable size (disp_w/disp_h) -- the query twin of viewport_resize.
   Either out pointer may be NULL; an invalid viewport reports 0 x 0. */
void
gui_viewport_size( gui_vp_t vp, i32* out_w, i32* out_h )
{
    i32 w = 0, h = 0;
    if ( vp < GUI_MAX_VIEWPORTS )
    {
        w = s_vp_pool[ vp ].disp_w;
        h = s_vp_pool[ vp ].disp_h;
    }
    if ( out_w ) *out_w = w;
    if ( out_h ) *out_h = h;
}

/* Update a viewport's drawable size.  Call on OS resize BEFORE frame_begin.
   Works identically for the primary (0) and secondary viewports.  GUI_VP_INVALID is a no-op. */
void
gui_viewport_resize( gui_vp_t vp, i32 w, i32 h )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return;
    if ( s_vp_pool[ vp ].disp_w == w && s_vp_pool[ vp ].disp_h == h )
        return;                 /* hosts republish size every frame -- raise only on a real change */

    s_vp_pool[ vp ].disp_w = w;
    s_vp_pool[ vp ].disp_h = h;
    redraw_request();           /* every layout resolves against the surface size; a programmatic
                                   resize carries no input edge of its own to ride in on */
}

/* Close a viewport and release its GPU geometry buffers.  Works for the primary (0) and secondary
   viewports alike.  Windows assigned to the closed viewport revert to the primary.  The host owns
   the OS window and rhi context; gui owns only the geometry buffers.

   Windows are per-context, but a real viewport is shared -- every live context's window pool must
   be migrated back to the primary, not just the bound one, or a secondary context's windows would
   keep pointing at a slot that just gave up its GPU buffers. */
void
gui_viewport_close( gui_vp_t vp )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return;
    viewport_destroy( vp );

    for ( u32 c = 0; c < s_ctx_pool_count; ++c )
    {
        gui_context_t* ctx = s_ctx_pool[ c ];
        if ( !ctx ) continue;
        for ( u32 i = 0; i < ctx->win.count; ++i )
            if ( ctx->win.pool[ i ].viewport == vp )
                ctx->win.pool[ i ].viewport = 0;
    }

    /* Trim the high-water viewport count when the closed slot was at the top. */
    while ( s_vp_count > 0 && !rhi_handle_valid( s_vp_pool[ s_vp_count - 1 ].vb ) )
    {
        --s_vp_count;
    }

    redraw_request();   /* the migrated windows must be re-emitted against the primary surface */
}

/*==============================================================================================
    Owned-floater lifecycle (gui-owned surfaces)

    Unlike viewport_open (the host hands gui a window + context to flush into), these own the OS
    window + rhi context end to end: gui creates them on spawn and destroys them on close.  The
    tear-off gesture drives spawn/close; a host/sandbox may also call gui_viewport_spawn
    directly to place a panel in its own OS window.

    viewport_spawn is defined here (not render.c) because it picks a slot from the global s_vp_pool
    and bumps s_vp_count; render.c's viewport_create / viewport_destroy stay context-agnostic (they
    take a slot index, not a pointer or a context) for the same reason -- a viewport is a real OS
    window / RHI context, not context-owned state.
==============================================================================================*/

/* Create a NEW gui-owned floater surface: OS window + its rhi context (swapchain) + per-surface
   geometry buffers.  The window's win_id doubles as the viewport slot index (APP_WIN_MAX ==
   GUI_MAX_VIEWPORTS, so the id is always a valid slot, and the window pool guarantees it is
   free) -- preserving the slot == win_id invariant the input router relies on.  Returns the
   viewport index, or GUI_VP_INVALID on any failure (each step unwinds the previous). */

static gui_vp_t
viewport_spawn( const char* title, i32 x, i32 y, i32 w, i32 h, bool no_activate )
{
    /* OS window first -- its win_id is the viewport slot index.  no_activate (set for a mid-drag
       tear-off) opens the floater with APP_WIN_NOFOCUS so it does NOT steal foreground from the
       origin window -- on Windows, activating another top-level window releases that window's
       mouse capture, which would sever the in-flight drag the moment the floater appeared. */
    /* Owned floaters are native-borderless: a detached panel owns its OS window and acts as that
       window's frame (window_begin treats any window on an owned viewport as GUI_WIN_NATIVE), so
       the OS drives its move / resize / snap.  no_activate (mid-drag tear-off) adds APP_WIN_NOFOCUS
       so spawning does not steal foreground and sever the origin window's mouse capture. */
    u32 open_flags = APP_WIN_BORDERLESS | ( no_activate ? APP_WIN_NOFOCUS : 0u );
    i32 win_id = app()->window_open( title, x, y, w, h, open_flags );
    if ( win_id == APP_WIN_INVALID )
        return GUI_VP_INVALID;
    if ( win_id < 0 || win_id >= (i32)GUI_MAX_VIEWPORTS )
    {
        app()->window_close( win_id );    /* no viewport slot for this id */
        return GUI_VP_INVALID;
    }

    gui_viewport_t* vp = &s_vp_pool[ win_id ];
    ORB_ASSERT( !rhi_handle_valid( vp->vb ) );   /* slot must be free (slot == win_id) */

    /* This window's own render context (swapchain) -- context_open queries handle+size from app(). */
    i32 ctx = rhi()->context_open( win_id );
    if ( ctx == RHI_CTX_INVALID )
    {
        app()->window_close( win_id );
        return GUI_VP_INVALID;
    }

    /* Per-surface geometry buffers; RHI_SWAPCHAIN_COLOR resolves to this ctx's image at flush. */
    if ( !viewport_create( (gui_vp_t)win_id, ( rhi_texture_t ){ .id = RHI_SWAPCHAIN_COLOR }, win_id ) )
    {
        rhi()->context_destroy( ctx );
        app()->window_close( win_id );
        return GUI_VP_INVALID;
    }

    vp->rhi_ctx = ctx;
    vp->owned   = true;    /* gui created the window + context -> gui destroys them */

    /* Size from the context, not the requested w/h: the swapchain extent is what this surface
       actually flushes into, and the OS is free to clamp a spawn (min size, work area, DPI).
       Same rule as viewport_open -- layout and surface start the frame in agreement. */
    i32 cw = 0, ch = 0;
    if ( !rhi()->context_size( ctx, &cw, &ch ) || cw <= 0 || ch <= 0 ) { cw = w; ch = h; }
    vp->disp_w  = cw;
    vp->disp_h  = ch;

    if ( (u32)win_id + 1u > s_vp_count )
        s_vp_count = (u32)win_id + 1u;
    return (gui_vp_t)win_id;
}

/* Public spawn: open an gui-owned floater hosting its own OS window at (x,y) sized w x h.
   Returns the viewport handle to assign windows to (window_set_next_viewport), or
   GUI_VP_INVALID.  Must be called between frames (it creates an OS window + rhi context). */

gui_vp_t
gui_viewport_spawn( const char* title, i32 x, i32 y, i32 w, i32 h )
{
    return viewport_spawn( title, x, y, w, h, false );
}

/* Service an OS resize/close event for any gui-known viewport (delegated from gui_event, which
   cannot see the viewport pool from input.c).

   For WIN_RESIZE: updates the matching viewport's drawable size.  rhi()->event() handles the
   swapchain rebuild -- gui no longer calls rhi()->context_resize() here.
   For WIN_CLOSE:  marks an owned floater for teardown at the next viewport_update.

   Returns true (consumed) only when win_id is an gui-owned floater, so the host's close-to-quit
   path and rhi()->event() still fire for the primary (host-owned) window. */

bool                            /* non-static: gui_event (core/gui_io.c) delegates across the TU seam */
gui_owned_window_event( const app_event_t* ev )
{
    /* Walk all live viewports (index 0 = primary, 1+ = secondary/owned). */
    for ( u32 i = 0; i < s_vp_count; ++i )
    {
        gui_viewport_t* vp = &s_vp_pool[ i ];
        if ( vp->win_id != ev->win_id )
            continue;
        if ( !rhi_handle_valid( vp->vb ) )
            continue;   /* slot not live */

        if ( ev->type == APP_EV_WIN_RESIZE )
        {
            vp->disp_w       = ev->data.win_resize.w;
            vp->disp_h       = ev->data.win_resize.h;
            s_viewport_dirty = true;   /* layout must recompute for the new surface size */
            /* Owned floaters: gui owns the window+context, so consume the event.
               Primary viewport: return false -- rhi()->event() also needs to rebuild the swapchain
               and the host may want to track the size for other purposes. */
            return vp->owned;
        }
        else if ( ev->type == APP_EV_WIN_CLOSE && vp->owned )
        {
            vp->pending_close = true;   /* torn down at the next viewport_update */
            return true;               /* consumed: gui owns this window's close lifecycle */
        }
        break;   /* found the viewport; primary close falls through to host */
    }
    return false;
}

/* Tear a window off the main surface into its own floater.  Placement depends on how the tear-off
   was requested: by_drag keeps the grab point under the cursor (spawned no-activate so the origin
   window keeps its OS mouse capture -- activating would release it and sever the drag); a re-open
   (has_home) lands at the saved restore position and size; a plain detach-button click keeps the
   panel at its exact current screen position. */
static void
viewport_service_tearoff( gui_window_t* win, bool has_home )
{
    i32 sx, sy;
    if ( s_vp_request.by_drag )
    {
        i32 cx = 0, cy = 0;
        f32 gox = 0.0f, goy = 0.0f;
        app()->mouse_position_screen( &cx, &cy );
        move_grab_offset( &gox, &goy );
        sx = cx - (i32)gox;
        sy = cy - (i32)goy;
    }
    else if ( has_home )
    {
        /* Re-opening a closed floater: land at the saved RESTORE (normal) position. */
        sx = win->reopen.home_x;
        sy = win->reopen.home_y;
    }
    else
    {
        i32 mx = 0, my = 0;
        app()->window_get_pos( s_vp_pool[ 0 ].win_id, &mx, &my );
        sx = mx + (i32)win->x;
        sy = my + (i32)win->y;
    }

    /* Re-open spawns at the saved restore size so the OS restore rect is the previous normal
       size; a plain tear-off spawns at the window's current size. */
    i32 sw = has_home ? (i32)win->reopen.w : (i32)win->w;
    i32 sh = has_home ? (i32)win->reopen.h : (i32)win->h;

    gui_vp_t vp = viewport_spawn( s_vp_request.title ? s_vp_request.title : "panel",
                                    sx, sy, sw, sh, s_vp_request.by_drag );
    if ( vp != GUI_VP_INVALID )
    {
        /* window_open positions the FRAME; set_pos lands the CLIENT corner on (sx,sy). */
        app()->window_set_pos( s_vp_pool[ vp ].win_id, sx, sy );
        win->viewport = vp;
        win->x        = 0.0f;
        win->y        = 0.0f;

        /* Re-maximize a floater that was closed maximized: spawned at the restore rect first
           (above), so the OS restore target becomes the previous normal size. */
        if ( has_home && win->reopen.maximized )
            app()->window_maximize( s_vp_pool[ vp ].win_id );
    }
}

/* Merge a floater back into the main surface.  Placement mirrors tear-off: by_drag lands the panel
   at cursor - grab offset (capture is held by the main window, so mouse coords are already
   main-client), continuous with the in-flight drag; a button click keeps it at the screen location
   the floater occupied.  The window is size- then position-clamped to fit the host surface (unless
   NO_BOUNDARY_CLAMP), and the vacated owned surface is destroyed once no window is left on it. */
static void
viewport_service_mergeback( gui_window_t* win )
{
    u32 fvp = s_vp_request.from_vp;

    win->viewport = 0;

    /* Clamp the window to fit inside the host surface on any pop-in path.  Size first so
       that position clamping below always has a non-negative travel range.  A panel that was
       fullscreened while floating must not land with resize handles off-screen.
       Skipped for GUI_WIN_NO_BOUNDARY_CLAMP -- placement is externally managed. */
    if ( !( win->flags & GUI_WIN_NO_BOUNDARY_CLAMP ) )
    {
        f32 dw    = vp_w( 0 );
        f32 dh    = vp_h( 0 );
        f32 top   = s_vp_pool[ 0 ].caption_inset;
        f32 max_h = dh - top; if ( max_h < 0.0f ) max_h = 0.0f;
        if ( win->w > dw )    win->w = dw;
        if ( win->h > max_h ) win->h = max_h;
    }

    if ( s_vp_request.by_drag )
    {
        f32 gox = 0.0f, goy = 0.0f;
        move_grab_offset( &gox, &goy );
        win->x = s_io.mouse_x - gox;
        win->y = s_io.mouse_y - goy;
    }
    else
    {
        i32 fx = 0, fy = 0, mx = 0, my = 0;
        if ( fvp < GUI_MAX_VIEWPORTS )
            app()->window_get_pos( s_vp_pool[ fvp ].win_id, &fx, &fy );
        app()->window_get_pos( s_vp_pool[ 0 ].win_id, &mx, &my );
        win->x = (f32)( fx - mx );
        win->y = (f32)( fy - my );

        /* Snap fully inside the host's client bounds: a floater merged from well clear of the
           main window would otherwise land at a screen offset outside the visible area (the
           button path never runs the per-frame window_clamp the drag path relies on).
           Skipped for GUI_WIN_NO_BOUNDARY_CLAMP -- caller is responsible for placement. */
        if ( !( win->flags & GUI_WIN_NO_BOUNDARY_CLAMP ) )
        {
            f32 dw  = vp_w( 0 );
            f32 dh  = vp_h( 0 );
            f32 top = s_vp_pool[ 0 ].caption_inset;
            f32 max_x = dw - win->w;
            f32 max_y = dh - win->h; if ( max_y < top ) max_y = top;
            win->x = win->x < 0.0f ? 0.0f : ( win->x > max_x ? max_x : win->x );
            win->y = win->y < top  ? top  : ( win->y > max_y ? max_y : win->y );
        }
    }

    /* A real viewport is shared across contexts, so "is anything still assigned to fvp" must
       check every live context's window pool, not just the caller's -- otherwise a secondary
       context's window could be left pointing at a surface this just destroyed out from under it. */
    bool empty = true;
    for ( u32 c = 0; empty && c < s_ctx_pool_count; ++c )
    {
        gui_context_t* ctx = s_ctx_pool[ c ];
        if ( !ctx ) continue;
        for ( u32 w = 0; w < ctx->win.count; ++w )
            if ( ctx->win.pool[ w ].viewport == fvp ) { empty = false; break; }
    }
    if ( empty && fvp > 0 && fvp < GUI_MAX_VIEWPORTS && s_vp_pool[ fvp ].owned )
        viewport_destroy( fvp );
}

/* Tear down owned floaters for either reason:

     pending_close -- the user closed the OS window (APP_EV_WIN_CLOSE).

     abandoned     -- the window(s) the floater hosts stopped being emitted.  A floater is just a
                      surface for the panel that was torn into it; if the caller hides that panel or
                      quits drawing it, its window_begin stops running and last_frame freezes, leaving
                      a hovering OS window with no UI.  Detect it by the same staleness rule popups
                      use (popup_close_check): the freshest assigned window has missed a full frame
                      (max last_frame + 1 < frame), or no window is bound at all.  One frame of grace
                      tolerates a transient single-frame hide.

   Runs after the tear-off / merge-back step, so a window just moved this frame already carries
   last_frame == gui_frame_index() on its new surface and never reads as abandoned.

   A viewport is shared by every context, so "abandoned" must be judged across ALL live contexts'
   window pools -- checking only one (the bound context, in practice always the primary) would
   destroy a floater a secondary context is still actively using the moment the primary itself
   has nothing assigned there. */
static void
viewport_teardown_owned( void )
{
    for ( u32 i = 1; i < s_vp_count; ++i )
    {
        gui_viewport_t* vp = &s_vp_pool[ i ];
        if ( !vp->owned )
            continue;

        bool abandoned = false;
        if ( !vp->pending_close )
        {
            /* Freshest emit among windows bound to this surface, across every context; no bound
               window anywhere stays abandoned. */
            u32  max_lf = 0u;
            bool any    = false;
            for ( u32 c = 0; c < s_ctx_pool_count; ++c )
            {
                gui_context_t* ctx = s_ctx_pool[ c ];
                if ( !ctx ) continue;
                for ( u32 w = 0; w < ctx->win.count; ++w )
                    if ( ctx->win.pool[ w ].viewport == i )
                    {
                        any = true;
                        if ( ctx->win.pool[ w ].last_frame > max_lf )
                            max_lf = ctx->win.pool[ w ].last_frame;
                    }
            }
            abandoned = !any || ( max_lf + 1u < gui_frame_index() );
        }

        if ( !( vp->pending_close || abandoned ) )
            continue;

        /* Windows assigned to this surface revert to the primary, then free the surface
           (viewport_destroy drains the GPU, frees buffers, destroys the ctx, closes the window).
           Reverting lets a panel re-emitted later reappear in the main window.  Every context's
           pool is migrated, not just the bound one -- same reasoning as the abandon scan above. */
        for ( u32 c = 0; c < s_ctx_pool_count; ++c )
        {
            gui_context_t* ctx = s_ctx_pool[ c ];
            if ( !ctx ) continue;
            for ( u32 w = 0; w < ctx->win.count; ++w )
                if ( ctx->win.pool[ w ].viewport == i )
                    ctx->win.pool[ w ].viewport = 0;
        }
        viewport_destroy( i );
    }

    /* Compact the high-water viewport count after any teardowns. */
    while ( s_vp_count > 0
            && !rhi_handle_valid( s_vp_pool[ s_vp_count - 1 ].vb ) )
        --s_vp_count;
}

/* Reconcile gui-owned floater surfaces with their OS windows.  Call once per frame after the UI
   build and BEFORE rendering: it is the safe point to tear surfaces down, since no in-flight draw
   list references one being freed.  Runs in two steps: (1) service tear-off / merge-back requests
   enqueued during the build (a window dragged off its host surface, or back onto it), then (2)
   tear down owned surfaces the user closed (pending_close) or abandoned (their panel stopped
   emitting). */

void
gui_viewport_update( void )
{
    /* Freeing a surface mid-build would pull it out from under a draw list still being written;
       after render it is already too late (the frame drew into a surface marked for teardown).
       The one safe window is between frame_end and the first render -- name it when missed. */
    GUI_CONTRACT( s_frame_phase != GUI_PHASE_BUILD,
                  "viewport_update() inside the build -- it frees surfaces, so it must run "
                  "after frame_end() and before render().\n" );

    /* (1) Tear-off / merge-back: a window whose title was dragged off its host surface (enqueued by
       window_begin_ex) changes which surface hosts it.  Resolved against the REQUEST's owner
       context (stamped at enqueue time), not whichever context happens to be bound now -- by this
       point in the frame every ctx_begin/ctx_end has closed and g_ctx has typically rebound to the
       primary, which would silently miss (or worse, mis-hit) a secondary context's window. */
    if ( s_vp_request.active )
    {
        s_vp_request.active     = false;
        gui_window_t* win     = window_find_in( s_vp_request.owner, s_vp_request.win_id );
        bool            has_home = s_vp_request.has_home;
        s_vp_request.has_home   = false;   /* one-shot: never leak into a later drag tear-off */
        if ( win && s_vp_request.from_vp == 0 )
            viewport_service_tearoff( win, has_home );
        else if ( win )
            viewport_service_mergeback( win );
    }

    /* (2) Tear down owned surfaces the user closed or abandoned, then compact the count. */
    viewport_teardown_owned();

    /* Surfaces reconciled: render may now flush this frame's geometry. */
    if ( s_frame_phase == GUI_PHASE_SEALED )
        s_frame_phase = GUI_PHASE_SYNCED;
}

/* Present every gui-owned floater surface from the shared draw list: open a frame on the
   floater's own rhi context, clear, replay that viewport's partition, end.  The main surface
   (index 0, host-owned) is presented by the host via render(); this loop handles only the surfaces
   gui spawned, so a single-window host stays a single-window present loop and tear-off "just
   works".  A minimized floater is skipped (its frame_begin would hand back an invalid cmd). */

void
gui_viewport_render_floaters( void )
{
    for ( u32 viewport_id = 1; viewport_id < s_vp_count; ++viewport_id )
    {
        gui_viewport_t* vp = &s_vp_pool[ viewport_id ];

        // Skip any slot that is not an gui owned floater (or closed without a valid rhi context)
        if ( !vp->owned || vp->rhi_ctx == RHI_CTX_INVALID )
            continue;

        // We don't render minimized windows.
        if ( app()->window_is_minimized( vp->win_id ) )
            continue;

        rhi_cmd_t cmd = rhi()->frame_begin( vp->rhi_ctx );
        if ( !rhi_cmd_valid( cmd ) )
            continue;

        /* Clear to the window background so the panel composites over a fresh surface (a floater
           is just a UI surface; without the clear, dragging within it smears -- hall-of-mirrors). */
        rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
            .texture  = { .id = RHI_SWAPCHAIN_COLOR },   /* resolves to this ctx's swapchain image */
            .load_op  = RHI_LOAD_OP_CLEAR,
            .store_op = RHI_STORE_OP_STORE,
            .clear    = { RHI_CLEAR_DEFAULT_R, RHI_CLEAR_DEFAULT_G,
                          RHI_CLEAR_DEFAULT_B, RHI_CLEAR_DEFAULT_A },
        }, 1, NULL );
        rhi()->cmd_end_rendering( cmd );

        gui_render( viewport_id, cmd );
        rhi()->frame_end( vp->rhi_ctx );
    }
}

// clang-format on
/*============================================================================================*/
