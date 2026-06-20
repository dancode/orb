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
    ctx_bind( &s_default_context );   /* bind the default context; the host may rebind before emitting */

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

void
imgui_shutdown( void )
{
#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_shutdown();
#endif

    /* Release every live surface's geometry (main + any floaters the host left open). */
    for ( u32 i = 0; i < IMGUI_MAX_VIEWPORTS; ++i )
        viewport_destroy( &g_ctx->viewports[ i ] );
    imgui_render_shutdown();                       /* shared pipeline / sampler / atlas */
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

void
imgui_new_frame( f32 dt )
{
    /* Primary viewport's stored dimensions drive the display clip and input snapshot.
       Both are 0 before the first viewport_open() -- safe: no windows are emitted yet. */
    i32 disp_w = g_ctx->viewport_count > 0 ? g_ctx->viewports[ 0 ].disp_w : 0;
    i32 disp_h = g_ctx->viewport_count > 0 ? g_ctx->viewports[ 0 ].disp_h : 0;

    input_update( disp_w, disp_h, dt );
    draw_reset( disp_w, disp_h );
#ifdef IMGUI_DEBUG_OVERLAY
    imgui_debug_reset();         /* clear the overlay's per-frame geometry */
#endif
    ctx_new_frame();             /* promotes last frame's hover_win */
    popup_close_check();         /* stale-close + click-outside, BEFORE any user open_popup */
    popup_apply_modal();         /* fence interaction behind an open modal (steals hover_win) */
    window_raise_on_press();     /* a press raises the hover window (takes effect this frame) */
    nav_new_frame();             /* commit last frame's nav move + read this frame's nav keys */
}

/* Flush one viewport's geometry partition to GPU.  The host opens a frame on that viewport's rhi
   context, calls render() with the context cmd, then ends the frame -- once per live viewport.
   The viewport's stored disp_w/h drive the GPU viewport and scissor clamping.
   The debug overlay is also painted when vp == 0 (the primary).  IMGUI_VP_INVALID is a no-op. */
void
imgui_render( imgui_vp_t vp, rhi_cmd_t cmd )
{
    if ( vp < 0 || vp >= IMGUI_MAX_VIEWPORTS )
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
    if ( win_id < 0 || win_id >= (i32)IMGUI_MAX_VIEWPORTS )
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
    if ( vp < 0 || vp >= IMGUI_MAX_VIEWPORTS )
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
    if ( vp <= 0 || vp >= IMGUI_MAX_VIEWPORTS )
        return;
    viewport_destroy( &g_ctx->viewports[ vp ] );
    /* Migrate any windows on this surface back to the primary. */
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].viewport == (u32)vp )
            s_windows[ i ].viewport = 0;
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
    if ( win_id < 0 || win_id >= (i32)IMGUI_MAX_VIEWPORTS )
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
            if ( s_vp_request.by_drag )
            {
                win->x = s_io.mouse_x - s_drag_off_x;
                win->y = s_io.mouse_y - s_drag_off_y;
            }
            else
            {
                i32 fx = 0, fy = 0, mx = 0, my = 0;
                if ( fvp < IMGUI_MAX_VIEWPORTS )
                    app()->window_get_pos( g_ctx->viewports[ fvp ].win_id, &fx, &fy );
                app()->window_get_pos( g_ctx->viewports[ 0 ].win_id, &mx, &my );
                win->x = (f32)( fx - mx );
                win->y = (f32)( fy - my );

                /* Snap fully inside the host's client bounds: a floater docked from well clear of the
                   main window would otherwise land at a screen offset outside the visible area (the
                   button path never runs the per-frame window_clamp the drag path relies on).  Clamp
                   so the whole panel sits within the surface when it fits, pinned to a corner if not. */
                const imgui_viewport_t* hv = &g_ctx->viewports[ 0 ];
                f32 dw = hv->disp_w > 0 ? (f32)hv->disp_w : (f32)s_io.display_w;
                f32 dh = hv->disp_h > 0 ? (f32)hv->disp_h : (f32)s_io.display_h;
                f32 max_x = dw - win->w; if ( max_x < 0.0f ) max_x = 0.0f;
                f32 max_y = dh - win->h; if ( max_y < 0.0f ) max_y = 0.0f;
                win->x = win->x < 0.0f ? 0.0f : ( win->x > max_x ? max_x : win->x );
                win->y = win->y < 0.0f ? 0.0f : ( win->y > max_y ? max_y : win->y );
            }

            bool empty = true;
            for ( u32 w = 0; w < s_window_count; ++w )
                if ( s_windows[ w ].viewport == fvp ) { empty = false; break; }
            if ( empty && fvp > 0 && fvp < IMGUI_MAX_VIEWPORTS && g_ctx->viewports[ fvp ].owned )
                viewport_destroy( &g_ctx->viewports[ fvp ] );
        }
    }

    /* (2) Owned floaters whose OS window the user closed (APP_EV_WIN_CLOSE -> pending_close). */
    for ( u32 i = 1; i < g_ctx->viewport_count; ++i )
    {
        imgui_viewport_t* vp = &g_ctx->viewports[ i ];
        if ( !( vp->owned && vp->pending_close ) )
            continue;

        /* Windows assigned to this surface revert to the primary, then free the surface
           (viewport_destroy drains the GPU, frees buffers, destroys the ctx, closes the window). */
        for ( u32 w = 0; w < s_window_count; ++w )
            if ( s_windows[ w ].viewport == i )
                s_windows[ w ].viewport = 0;
        viewport_destroy( vp );
    }
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

        imgui_render_flush( vp, i, cmd, vp->disp_w, vp->disp_h );
        rhi()->frame_end( vp->rhi_ctx );
    }
}

/*==============================================================================================
    Font API
==============================================================================================*/

bool
imgui_load_font( const char* path )
{
    if ( !tt_font_load( path ) )
        return false;

    /* Recompute layout metrics from the font's type size, glyph box, and line advance. */
    layout_compute( (u32)s_font->size, (u32)s_font->char_h, (u32)s_font->line_h );
    return true;
}

void
imgui_set_font( imgui_font_t font )
{
    tt_font_unload();
    bitmap_font_select( font );
    layout_compute( (u32)s_font->size, (u32)s_font->char_h, (u32)s_font->line_h );

    const bitmap_font_def_t* def = s_bitmap_active->def;

    printf( "[imgui] set font '%s : %s' (char_h=%.1f line_h=%.1f)\n",
            s_font->proportional ? "TrueType" : "Bitmap", def->debug_name, s_font->char_h, s_font->line_h );
}

void
imgui_set_bmp_scale( u32 scale )
{
    bitmap_scale_set( scale );
    if ( !s_tt_font.active )
        layout_compute( (u32)s_font->size, (u32)s_font->char_h, (u32)s_font->line_h );
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

// clang-format on
/*============================================================================================*/
