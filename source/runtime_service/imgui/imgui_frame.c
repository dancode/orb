/*==============================================================================================

    runtime_service/imgui/imgui_frame.c -- Frame lifecycle, viewport, font, and clip helpers.

    Implements the public functions that bracket a frame: init/shutdown, new_frame, render,
    viewport open/resize/close, font loading/selection, bitmap scale, and clip rect push/pop.
    Included by imgui.c before imgui_api.c so the vtable can reference these by name.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Init / Shutdown
==============================================================================================*/

bool
imgui_init( void )
{
    ctx_pool_init();   /* wire default context's static backing arrays; sets g_ctx */

    if ( !imgui_render_init() )       /* shared pipeline / sampler / atlas */
        return false;

    /* No viewports created here -- the host calls viewport_open() after init() for each OS window.
       Viewports own their own geometry buffers and are opened explicitly before any frames. */

#ifdef IMGUI_DEBUG_OVERLAY
    /* Debug overlay GPU buffers.  Non-fatal: a failure just leaves the overlay dark. */
    if ( !imgui_debug_init() )
        printf( "[imgui] WARNING: debug overlay buffers failed; overlay disabled\n" );
#endif
    return true;
}

/*============================================================================================*/

void
imgui_shutdown( void )
{
    #ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_shutdown();
    #endif

    /* Destroy GPU surfaces for every context before releasing memory blocks.
       viewport_destroy is g_ctx-agnostic (takes a pointer), so no rebind is needed.
       Primary context viewports (including any imgui-owned floaters) are destroyed first. */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        imgui_context_t* ctx = s_ctx_pool[ i ];
        if ( !ctx ) continue;
        for ( u32 v = 0; v < ctx->max_viewports; ++v )
            viewport_destroy( &ctx->viewports[ v ] );
    }
    imgui_render_shutdown();    /* shared pipeline / sampler / atlas */

    /* Free all context blocks. */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        if ( !s_ctx_pool[ i ] ) continue;
        free( s_ctx_pool[ i ]->_alloc );
        s_ctx_pool[ i ] = NULL;
    }
    g_ctx = NULL;
}

/*==============================================================================================
    Memory Stats
==============================================================================================*/

imgui_mem_stats_t
imgui_mem_stats( void )
{
    return imgui_render_memory();
}

void
imgui_print_mem_stats( void )
{
    imgui_render_print_memory();
}

/*==============================================================================================
    Frame API
==============================================================================================*/

/* Global frame phase: input poll + draw-list reset.  Always reads display dimensions from the
   PRIMARY context (slot 0): the OS window and its viewports belong to the default context
   regardless of which context is active for input this frame.  Called by imgui_new_frame (the
   compat wrapper) and directly by hosts using the multi-context frame contract. */
void
imgui_frame_begin( f32 dt )
{
    imgui_context_t* primary = s_ctx_pool[ 0 ];   /* default ctx always owns the OS window */
    i32 disp_w = primary->viewport_count > 0 ? primary->viewports[ 0 ].disp_w : 0;
    i32 disp_h = primary->viewport_count > 0 ? primary->viewports[ 0 ].disp_h : 0;

    /* caption_inset is NOT cleared here.  Native shell windows republish it during the build, so
       the field always reflects the last frame the shell was active.  Leaving it sticky means
       update_platform_windows gets the correct top bound regardless of whether it is called before
       or after the build this frame (both are valid host patterns for tear-off handling).  If the
       native shell permanently disappears the stale inset is conservative -- windows stay clamped
       below where the caption used to be -- and the viewport is destroyed shortly after anyway. */

    /* Push last frame's cursor request to the OS BEFORE interaction_frame_reset promotes the new
       hover_win and input_update overwrites mouse_viewport -- cursor_flush reads both as the
       previous frame left them, keeping the requested shape and its target window coherent. */
    cursor_flush();

    /* Promote last frame's render-stat accumulator to the published value BEFORE draw_reset, so a
       build that reads render_stats() this frame sees the previous frame's completed totals. */
    imgui_render_stats_publish();

    input_update( disp_w, disp_h, dt );
    draw_reset( disp_w, disp_h );
    imgui_render_frame_reset();   /* drop last frame's tessellation cache; rebuilt on first flush */

    /* Push any icons registered since last frame to the GPU once, before the build emits draws. */
    icon_atlas_flush_upload();

    /* Reset global interaction state exactly once per app frame.  hover_win promotion and
       active_id release happen here -- NOT in ctx_new_frame -- so subsequent ctx_begin calls
       for additional contexts do not clobber hover nominations from earlier contexts. */
    interaction_frame_reset();

#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_reset();         /* clear the overlay's per-frame geometry */
#endif
}

/* Per-context frame phase: bind `ctx_handle` and run a full frame init for it.  Every context
   gets the full init (nav, popup check, per-frame scratch reset) regardless of its `listening`
   flag; the flag only gates widget interaction (hover nomination and widget hit-tests). */
void
imgui_ctx_begin( imgui_ctx_t ctx_handle )
{
    if ( ctx_handle < 0 || ctx_handle >= (i32)s_ctx_pool_count || !s_ctx_pool[ ctx_handle ] )
        ctx_handle = IMGUI_CTX_DEFAULT;

    imgui_context_t* c = s_ctx_pool[ ctx_handle ];
    ctx_bind( c );

    s_retained.wants_redraw = false;   /* cleared before the build; set again by any animating widget */
    ctx_new_frame();             /* per-context scratch reset + frame clock bump (no global interaction touch) */
    popup_close_check();         /* stale-close + click-outside, BEFORE any user open_popup */
    popup_apply_modal();         /* fence interaction behind an open modal (steals hover_win) */
    window_raise_on_press();     /* a press raises the hover window (takes effect this frame) */
    nav_new_frame();             /* commit last frame's nav move + read this frame's nav keys */
}

/* Single-context backwards-compat wrapper: frame_begin + ctx_begin(DEFAULT). */
void
imgui_new_frame( f32 dt )
{
    imgui_frame_begin( dt );
    imgui_ctx_begin( IMGUI_CTX_DEFAULT );
}

/* Flush one viewport's geometry partition to GPU.  The host opens a frame on that viewport's rhi
   context, calls render() with the context cmd, then ends the frame -- once per live viewport.
   The viewport's stored disp_w/h drive the GPU viewport and scissor clamping.
   The debug overlay is also painted when vp == 0 (the primary).  IMGUI_VP_INVALID is a no-op. */
void
imgui_render( imgui_vp_t vp, rhi_cmd_t cmd )
{
    if ( vp < 0 || vp >= (i32)g_ctx->max_viewports )
        return;
    imgui_viewport_t* v = &g_ctx->viewports[ vp ];
    imgui_render_flush( v, (u32)vp, cmd, v->disp_w, v->disp_h );
#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_flush( vp, cmd, v->disp_w, v->disp_h );   /* each viewport flushes its own rects */
#endif
}

/*==============================================================================================
    Viewport API

    Open a viewport: claim the slot at win_id (slot index == win_id), create its GPU geometry
    buffers, and record the OS window and initial drawable size.

    Slot alignment: win_id 0 = primary swapchain, win_id 1..N = secondary surfaces.  Since each
    viewport requires a live OS window, the window pool guarantees the matching slot is free.
    RHI_SWAPCHAIN_COLOR resolves per-context at flush time -- which cmd you pass render() selects
    the swapchain.
    Returns the handle to pass to render / viewport_resize / viewport_close / set_next_window_viewport,
    or IMGUI_VP_INVALID on bad win_id or GPU buffer failure.
    Must be called after init() and before new_frame().

==============================================================================================*/

imgui_vp_t
imgui_viewport_open( i32 win_id, i32 w, i32 h )
{
    /* Slot index == win_id; an open window guarantees the slot is free. */
    if ( win_id < 0 || win_id >= (i32)g_ctx->max_viewports )
        return IMGUI_VP_INVALID;

    imgui_viewport_t* vp = &g_ctx->viewports[ win_id ];
    ORB_ASSERT( !rhi_handle_valid( vp->vb ) );   /* slot must be free */

    if ( !viewport_create( vp, ( rhi_texture_t ){ .id = RHI_SWAPCHAIN_COLOR }, win_id ) )
        return IMGUI_VP_INVALID;

    vp->disp_w = w;
    vp->disp_h = h;

    if ( (u32)win_id + 1u > g_ctx->viewport_count )
        g_ctx->viewport_count = (u32)win_id + 1u;
    return (imgui_vp_t)win_id;
}

/* Update a viewport's drawable size.  Call on OS resize BEFORE new_frame.
   Works identically for the primary (0) and secondary viewports.  IMGUI_VP_INVALID is a no-op. */
void
imgui_viewport_resize( imgui_vp_t vp, i32 w, i32 h )
{
    if ( vp < 0 || vp >= (i32)g_ctx->max_viewports )
        return;
    g_ctx->viewports[ vp ].disp_w = w;
    g_ctx->viewports[ vp ].disp_h = h;
}

/* Close a non-primary viewport and release its GPU geometry buffers.  Windows that were assigned
   to this surface automatically revert to the primary (index 0).  The primary (index 0) is managed
   by init/shutdown and may not be closed here.  The host owns the OS window and rhi context. */
void
imgui_viewport_close( imgui_vp_t vp )
{
    if ( vp <= 0 || vp >= (i32)g_ctx->max_viewports )
        return;
    viewport_destroy( &g_ctx->viewports[ vp ] );
    /* Migrate any windows on this surface back to the primary. */
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].viewport == (u32)vp )
            s_windows[ i ].viewport = 0;
    /* Trim the high-water viewport count when the closed slot was at the top. */
    while ( g_ctx->viewport_count > 0
            && !rhi_handle_valid( g_ctx->viewports[ g_ctx->viewport_count - 1 ].vb ) )
        --g_ctx->viewport_count;
}

/*==============================================================================================
    Owned-floater lifecycle (imgui-owned surfaces)

    Unlike viewport_open (the host hands imgui a window + context to flush into), these own the OS
    window + rhi context end to end: imgui creates them on spawn and destroys them on close.  The
    tear-off gesture (Phase 3) drives spawn/close; a host/sandbox may also call imgui_viewport_spawn
    directly to place a panel in its own OS window.

    viewport_spawn is defined here (not render.c) because it picks a slot from g_ctx->viewports and
    bumps viewport_count -- and g_ctx lives in imgui_ctx.c, included after render.c.  render.c's
    viewport_create / viewport_destroy stay g_ctx-agnostic (they take a vp pointer) for that reason.
==============================================================================================*/

/* Create a NEW imgui-owned floater surface: OS window + its rhi context (swapchain) + per-surface
   geometry buffers.  The window's win_id doubles as the viewport slot index (APP_WIN_MAX ==
   IMGUI_MAX_VIEWPORTS, so the id is always a valid slot, and the window pool guarantees it is
   free) -- preserving the slot == win_id invariant the input router relies on.  Returns the
   viewport index, or IMGUI_VP_INVALID on any failure (each step unwinds the previous). */
static imgui_vp_t
viewport_spawn( const char* title, i32 x, i32 y, i32 w, i32 h, bool no_activate )
{
    /* OS window first -- its win_id is the viewport slot index.  no_activate (set for a mid-drag
       tear-off) opens the floater with APP_WIN_NOFOCUS so it does NOT steal foreground from the
       origin window -- on Windows, activating another top-level window releases that window's
       mouse capture, which would sever the in-flight drag the moment the floater appeared. */
    /* Owned floaters are native-borderless: a detached panel owns its OS window and acts as that
       window's frame (begin_window treats any window on an owned viewport as IMGUI_WIN_NATIVE), so
       the OS drives its move / resize / snap.  no_activate (mid-drag tear-off) adds APP_WIN_NOFOCUS
       so spawning does not steal foreground and sever the origin window's mouse capture. */
    u32 open_flags = APP_WIN_BORDERLESS | ( no_activate ? APP_WIN_NOFOCUS : 0u );
    i32 win_id = app()->window_open( title, x, y, w, h, open_flags );
    if ( win_id == APP_WIN_INVALID )
        return IMGUI_VP_INVALID;
    if ( win_id < 0 || win_id >= (i32)g_ctx->max_viewports )
    {
        app()->window_close( win_id );    /* no viewport slot for this id */
        return IMGUI_VP_INVALID;
    }

    imgui_viewport_t* vp = &g_ctx->viewports[ win_id ];
    ORB_ASSERT( !rhi_handle_valid( vp->vb ) );   /* slot must be free (slot == win_id) */

    /* This window's own render context (swapchain). */
    i32 ctx = rhi()->context_create( win_id, app()->window_handle( win_id ), w, h );
    if ( ctx == RHI_CTX_INVALID )
    {
        app()->window_close( win_id );
        return IMGUI_VP_INVALID;
    }

    /* Per-surface geometry buffers; RHI_SWAPCHAIN_COLOR resolves to this ctx's image at flush. */
    if ( !viewport_create( vp, ( rhi_texture_t ){ .id = RHI_SWAPCHAIN_COLOR }, win_id ) )
    {
        rhi()->context_destroy( ctx );
        app()->window_close( win_id );
        return IMGUI_VP_INVALID;
    }

    vp->rhi_ctx = ctx;
    vp->owned   = true;    /* imgui created the window + context -> imgui destroys them */
    vp->disp_w  = w;
    vp->disp_h  = h;

    if ( (u32)win_id + 1u > g_ctx->viewport_count )
        g_ctx->viewport_count = (u32)win_id + 1u;
    return (imgui_vp_t)win_id;
}

/* Public spawn: open an imgui-owned floater hosting its own OS window at (x,y) sized w x h.
   Returns the viewport handle to assign windows to (set_next_window_viewport), or
   IMGUI_VP_INVALID.  Must be called between frames (it creates an OS window + rhi context). */
imgui_vp_t
imgui_viewport_spawn( const char* title, i32 x, i32 y, i32 w, i32 h )
{
    return viewport_spawn( title, x, y, w, h, false );
}

/* Service an OS resize/close event for an imgui-owned floater (delegated from imgui_event, which
   cannot see the viewport pool from input.c).  Resize updates the context + drawable size now;
   close defers teardown to update_platform_windows (a safe point).  Returns true -- consuming the
   event -- only when win_id matches an owned surface, so a host window's events fall through. */
static bool
imgui_owned_window_event( const app_event_t* ev )
{
    for ( u32 i = 1; i < g_ctx->viewport_count; ++i )
    {
        imgui_viewport_t* vp = &g_ctx->viewports[ i ];
        if ( !vp->owned || vp->win_id != ev->win_id )
            continue;

        if ( ev->type == APP_EV_WIN_RESIZE )
        {
            i32 w = ev->data.win_resize.w, h = ev->data.win_resize.h;
            rhi()->context_resize( vp->rhi_ctx, w, h );
            vp->disp_w = w;
            vp->disp_h = h;
        }
        else /* APP_EV_WIN_CLOSE */
        {
            vp->pending_close = true;   /* torn down at the next update_platform_windows */
        }
        return true;   /* imgui owns this window -> event consumed */
    }
    return false;      /* not an imgui window -> host handles it */
}

/* Reconcile imgui-owned floater surfaces with their OS windows.  Call once per frame after the UI
   build and BEFORE rendering: it is the safe point to tear surfaces down, since no in-flight draw
   list references one being freed.  Today it destroys surfaces the user closed (pending_close);
   Phase 3 will also service tear-off / merge-back requests enqueued during the build. */
void
imgui_update_platform_windows( void )
{
    /* (1) Tear-off / merge-back: a window whose title was dragged off its host surface (enqueued by
       window_begin_ex) changes which surface hosts it. */
    if ( s_vp_request.active )
    {
        s_vp_request.active     = false;
        imgui_window_t* win     = window_find( s_vp_request.win_id );
        if ( win && s_vp_request.from_vp == 0 )
        {
            /* Tear the window off the main surface into its own floater.  Placement depends on how
               the tear-off was requested:

               by_drag -- a live title-bar drag (button still down).  Place the floater so the grab
               point stays exactly beneath the cursor: client origin = screen cursor - the grab offset
               recorded when the drag began.  Spawned no-activate so the origin window keeps its OS
               mouse capture (activating would release it and sever the drag); the per-frame
               floater-follow in begin_window then keeps the panel tracking the cursor.

               else -- a detach-button click, no drag in flight.  Keep the panel at its EXACT screen
               position (main client origin + the panel's position within it), so it pops out in place
               rather than jumping to the cursor.  Activate normally; there is no capture to preserve. */
            i32 sx, sy;
            if ( s_vp_request.by_drag )
            {
                i32 cx = 0, cy = 0;
                app()->mouse_position_screen( &cx, &cy );
                sx = cx - (i32)s_drag_off_x;
                sy = cy - (i32)s_drag_off_y;
            }
            else
            {
                i32 mx = 0, my = 0;
                app()->window_get_pos( g_ctx->viewports[ 0 ].win_id, &mx, &my );
                sx = mx + (i32)win->x;
                sy = my + (i32)win->y;
            }

            imgui_vp_t vp = viewport_spawn( s_vp_request.title ? s_vp_request.title : "panel",
                                            sx, sy, (i32)win->w, (i32)win->h, s_vp_request.by_drag );
            if ( vp != IMGUI_VP_INVALID )
            {
                /* window_open positions the FRAME; set_pos lands the CLIENT corner on (sx,sy). */
                app()->window_set_pos( g_ctx->viewports[ vp ].win_id, sx, sy );
                win->viewport = (u32)vp;
                win->x        = 0.0f;
                win->y        = 0.0f;
            }
        }
        else if ( win )
        {
            /* Merge back into the main surface.  Placement mirrors tear-off:

               by_drag -- capture is held by the main window for the whole drag, so s_io.mouse_x/y are
               already main-client coords; the panel lands at cursor - grab offset, continuous with the
               in-flight drag, which the attached drag-apply in begin_window then carries on.

               else -- a button click; keep the panel at the screen location the floater occupied (its
               client origin minus the main client origin), so it docks in place rather than jumping. */
            u32 fvp = s_vp_request.from_vp;

            win->viewport = 0;

            /* Clamp the window to fit inside the host surface on any pop-in path.  Size first so
               that position clamping below always has a non-negative travel range.  A panel that was
               fullscreened while floating must not land with resize handles off-screen.
               Skipped for IMGUI_WIN_NO_BOUNDARY_CLAMP -- placement is externally managed. */
            if ( !( win->flags & IMGUI_WIN_NO_BOUNDARY_CLAMP ) )
            {
                const imgui_viewport_t* hv = &g_ctx->viewports[ 0 ];
                f32 dw    = vp_w( hv );
                f32 dh    = vp_h( hv );
                f32 top   = hv->caption_inset;
                f32 max_h = dh - top; if ( max_h < 0.0f ) max_h = 0.0f;
                if ( win->w > dw )    win->w = dw;
                if ( win->h > max_h ) win->h = max_h;
            }

            if ( s_vp_request.by_drag )
            {
                win->x = s_io.mouse_x - s_drag_off_x;
                win->y = s_io.mouse_y - s_drag_off_y;
            }
            else
            {
                i32 fx = 0, fy = 0, mx = 0, my = 0;
                if ( fvp < g_ctx->max_viewports )
                    app()->window_get_pos( g_ctx->viewports[ fvp ].win_id, &fx, &fy );
                app()->window_get_pos( g_ctx->viewports[ 0 ].win_id, &mx, &my );
                win->x = (f32)( fx - mx );
                win->y = (f32)( fy - my );

                /* Snap fully inside the host's client bounds: a floater merged from well clear of the
                   main window would otherwise land at a screen offset outside the visible area (the
                   button path never runs the per-frame window_clamp the drag path relies on).
                   Skipped for IMGUI_WIN_NO_BOUNDARY_CLAMP -- caller is responsible for placement. */
                if ( !( win->flags & IMGUI_WIN_NO_BOUNDARY_CLAMP ) )
                {
                    const imgui_viewport_t* hv = &g_ctx->viewports[ 0 ];
                    f32 dw  = vp_w( hv );
                    f32 dh  = vp_h( hv );
                    f32 top = hv->caption_inset;
                    f32 max_x = dw - win->w;
                    f32 max_y = dh - win->h; if ( max_y < top ) max_y = top;
                    win->x = win->x < 0.0f ? 0.0f : ( win->x > max_x ? max_x : win->x );
                    win->y = win->y < top  ? top  : ( win->y > max_y ? max_y : win->y );
                }
            }

            bool empty = true;
            for ( u32 w = 0; w < s_window_count; ++w )
                if ( s_windows[ w ].viewport == fvp ) { empty = false; break; }
            if ( empty && fvp > 0 && fvp < g_ctx->max_viewports && g_ctx->viewports[ fvp ].owned )
                viewport_destroy( &g_ctx->viewports[ fvp ] );
        }
    }

    /* (2) Tear down owned floaters for either reason:

         pending_close -- the user closed the OS window (APP_EV_WIN_CLOSE).

         abandoned     -- the window(s) the floater hosts stopped being emitted.  A floater is just a
                          surface for the panel that was torn into it; if the caller hides that panel
                          or quits drawing it, its begin_window stops running and last_frame freezes,
                          leaving a hovering OS window with no UI and no controls.  Detect it by the
                          same staleness rule popups use (popup_close_check): the freshest assigned
                          window has missed a full frame (max last_frame + 1 < frame), or no window is
                          bound at all.  One frame of grace tolerates a transient single-frame hide.

       This runs after step (1), so a window just torn off / merged this frame already carries
       last_frame == s_retained.frame on its new surface and never reads as abandoned. */
    for ( u32 i = 1; i < g_ctx->viewport_count; ++i )
    {
        imgui_viewport_t* vp = &g_ctx->viewports[ i ];
        if ( !vp->owned )
            continue;

        bool abandoned = false;
        if ( !vp->pending_close )
        {
            /* Freshest emit among windows bound to this surface; no bound window stays abandoned. */
            u32  max_lf = 0u;
            bool any    = false;
            for ( u32 w = 0; w < s_window_count; ++w )
                if ( s_windows[ w ].viewport == i )
                {
                    any = true;
                    if ( s_windows[ w ].last_frame > max_lf )
                        max_lf = s_windows[ w ].last_frame;
                }
            abandoned = !any || ( max_lf + 1u < s_retained.frame );
        }

        if ( !( vp->pending_close || abandoned ) )
            continue;

        /* Windows assigned to this surface revert to the primary, then free the surface
           (viewport_destroy drains the GPU, frees buffers, destroys the ctx, closes the window).
           Reverting lets a panel re-emitted later reappear in the main window. */
        for ( u32 w = 0; w < s_window_count; ++w )
            if ( s_windows[ w ].viewport == i )
                s_windows[ w ].viewport = 0;
        viewport_destroy( vp );
    }

    /* Compact the high-water viewport count after any teardowns. */
    while ( g_ctx->viewport_count > 0
            && !rhi_handle_valid( g_ctx->viewports[ g_ctx->viewport_count - 1 ].vb ) )
        --g_ctx->viewport_count;
}

/* Present every imgui-owned floater surface from the shared draw list: open a frame on the
   floater's own rhi context, clear, replay that viewport's partition, end.  The main surface
   (index 0, host-owned) is presented by the host via render(); this loop handles only the surfaces
   imgui spawned, so a single-window host stays a single-window present loop and tear-off "just
   works".  A minimized floater is skipped (its frame_begin would hand back an invalid cmd). */
void
imgui_render_floaters( void )
{
    for ( u32 i = 1; i < g_ctx->viewport_count; ++i )
    {
        imgui_viewport_t* vp = &g_ctx->viewports[ i ];
        if ( !vp->owned || vp->rhi_ctx == RHI_CTX_INVALID )
            continue;
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
            .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
        }, 1, NULL );
        rhi()->cmd_end_rendering( cmd );

        imgui_render( (imgui_vp_t)i, cmd );
        rhi()->frame_end( vp->rhi_ctx );
    }
}

/*==============================================================================================
    Font API
==============================================================================================*/

/* The font metrics live in the render backend unit; this UI-unit API reads them through the
   font_em / font_char_h / font_line_h accessors (imgui_backend.h) and feeds layout_compute
   (this unit) -- the font -> layout bridge.  font_is_tt / font_print_active keep the remaining
   font internals (s_tt_font, s_bitmap_active) on the backend side. */

bool
imgui_load_font( const char* path )
{
    if ( !tt_font_load( path ) )
        return false;

    /* Recompute layout metrics from the font's type size, glyph box, and line advance. */
    layout_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h() );
    return true;
}

void
imgui_set_font( imgui_font_t font )
{
    tt_font_unload();
    bitmap_font_select( font );
    layout_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h() );
    font_print_active();
}

void
imgui_set_bmp_scale( u32 scale )
{
    bitmap_scale_set( scale );
    if ( !font_is_tt() )
        layout_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h() );
}

/*==============================================================================================
    Clip API
==============================================================================================*/

void
imgui_push_clip( f32 x, f32 y, f32 w, f32 h )
{
    draw_push_clip_rect( x, y, w, h );
}

void
imgui_pop_clip( void )
{
    draw_pop_clip_rect();
}

/*==============================================================================================
    Animation state query
==============================================================================================*/

/* True when at least one imgui_anim_f32 channel is still transitioning this frame.  The host
   loop checks this after the build to decide whether to skip the editor-sleep wait: as long as
   any value is mid-animation the host must keep pumping frames, otherwise the transition freezes. */
bool
imgui_wants_redraw( void )
{
    return s_retained.wants_redraw;
}

/*==============================================================================================
    Multi-context API
==============================================================================================*/

/* Set whether a context listens for hover/click/nav input.  Call between frames.
   Multiple contexts may listen simultaneously; a deaf context renders but returns inert
   widget state.  The default context starts listening; secondary contexts start deaf. */
void
imgui_ctx_set_listening( imgui_ctx_t ctx, bool listen )
{
    if ( ctx >= 0 && ctx < IMGUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        s_ctx_pool[ ctx ]->listening = listen;
}

/* Allocate a fresh secondary context sized to `cfg` (NULL = editor defaults).
   Each gets a unique id_salt so same-named widgets do not alias across contexts.
   Returns IMGUI_CTX_INVALID when the pool is full.  Call between frames. */
imgui_ctx_t
imgui_ctx_create( const imgui_ctx_config_t* cfg )
{
    /* Resolve config: NULL or zero fields fall back to editor defaults.
       max_dock_nodes == 0 is valid (disables docking); do not default it. */
    imgui_ctx_config_t c = cfg ? *cfg : IMGUI_CTX_CONFIG_EDITOR;
    if ( !c.max_windows   ) c.max_windows   = IMGUI_DEFAULT_MAX_WINDOWS;
    if ( !c.state_slots   ) c.state_slots   = IMGUI_DEFAULT_STATE_SLOTS;
    if ( !c.popup_depth   ) c.popup_depth   = IMGUI_DEFAULT_POPUP_DEPTH;
    if ( !c.max_viewports ) c.max_viewports = IMGUI_DEFAULT_MAX_VIEWPORTS;

    /* state_slots must be a power of two for the hash mask to work. */
    u32 slots = c.state_slots;
    if ( slots < 16 ) slots = 16;
    u32 p = 1;
    while ( p < slots ) p <<= 1;
    slots = p;

    /* Find a free pool slot (1..IMGUI_CTX_POOL_MAX-1). */
    i32 slot = -1;
    for ( i32 i = 1; i < IMGUI_CTX_POOL_MAX; ++i )
        if ( !s_ctx_pool[ i ] ) { slot = i; break; }
    if ( slot < 0 ) return IMGUI_CTX_INVALID;

    imgui_context_t* ctx = ctx_alloc_slot( &c, slots, slot );
    if ( !ctx ) return IMGUI_CTX_INVALID;
    ctx->listening = false;   /* secondary contexts start deaf; caller opts in */

    s_ctx_pool[ slot ] = ctx;
    if ( (u32)slot >= s_ctx_pool_count ) s_ctx_pool_count = (u32)slot + 1u;
    return (imgui_ctx_t)slot;
}

/* Free a secondary context; rebinds the default if this was current.  Never destroys slot 0. */
void
imgui_ctx_destroy( imgui_ctx_t ctx )
{
    if ( ctx <= 0 || ctx >= IMGUI_CTX_POOL_MAX || !s_ctx_pool[ ctx ] )
        return;
    imgui_context_t* c = s_ctx_pool[ ctx ];
    if ( g_ctx == c ) ctx_bind( NULL );
    /* Destroy any GPU surfaces the context opened before releasing its memory block. */
    for ( u32 i = 0; i < c->max_viewports; ++i )
        viewport_destroy( &c->viewports[ i ] );
    if ( c->_alloc ) free( c->_alloc );
    s_ctx_pool[ ctx ] = NULL;
}

/* Make ctx the current context.  IMGUI_CTX_DEFAULT (0) or an invalid handle rebinds the default. */
void
imgui_ctx_bind( imgui_ctx_t ctx )
{
    if ( ctx >= 0 && ctx < IMGUI_CTX_POOL_MAX && s_ctx_pool[ ctx ] )
        ctx_bind( s_ctx_pool[ ctx ] );
    else
        ctx_bind( NULL );
}

// clang-format on
/*============================================================================================*/
