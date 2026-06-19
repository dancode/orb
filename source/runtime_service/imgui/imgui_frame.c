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
