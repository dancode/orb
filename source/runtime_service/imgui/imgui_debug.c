/*==============================================================================================

    runtime_service/imgui/imgui_debug.c -- Bolt-on debug overlay.

    A second, independent draw list that is emitted from inside the regular imgui code (via the
    DBG_* capture macros in imgui.c) and flushed LAST, on top of the finished UI.  It visualizes
    things the normal UI hides: every widget's interaction rect, the window edge-resize grab
    bands, window frames (with the hover window highlighted), and the clip/scissor stack.

    It is deliberately NOT intertwined with the main draw list (s_draw):
      - its own verts/idx/cmds with their own (small) caps, so it can never starve the UI
        budget and make real widgets overflow;
      - its own GPU buffers, reusing only the shared imgui pipeline + font sampler;
      - no z-sort and no per-window clip on flush -- overlay order is emit order, always topmost;
      - the whole subsystem (state, buffers, capture, flush) is compiled out unless
        IMGUI_DEBUG_OVERLAY is defined (Debug builds), leaving only the two no-op public setters.

    Active layers are chosen at runtime with imgui()->debug_set_layers( imgui_dbg_layer_t mask ).

    Included by imgui.c after imgui_render.c so s_render (pipeline, sampler), render_ortho,
    imgui_push_t, the IMGUI_*_REGION_BYTES sizes, and the font_* atlas helpers are all in scope.

==============================================================================================*/
// clang-format off

#ifdef IMGUI_DEBUG_OVERLAY

/*----------------------------------------------------------------------------------------------
    Caps + colors

    Caps are a fraction of the UI list -- a debug frame is a handful of outlines and labels,
    not a dense UI.  Indices are u16 (vert base must stay under 64K), comfortably true here.
----------------------------------------------------------------------------------------------*/

#define IMGUI_DBG_MAX_VERTS ( 8 * 1024 )
#define IMGUI_DBG_MAX_IDX   ( IMGUI_DBG_MAX_VERTS * 3 )

#define IMGUI_DBG_VB_REGION_BYTES ( IMGUI_DBG_MAX_VERTS * sizeof( imgui_draw_vert_t ) )
#define IMGUI_DBG_IB_REGION_BYTES ( IMGUI_DBG_MAX_IDX   * sizeof( u16 ) )

/* Palette (IMGUI_COLOR packs R,G,B,A bytes).  Bright, saturated, semi-transparent so the
   overlay reads clearly over the UI without fully hiding it. */
#define DBG_COL_WIDGET      IMGUI_COLOR( 0x30, 0xE0, 0x30, 0xC0 )   /* widget rect        */
#define DBG_COL_HOVER       IMGUI_COLOR( 0xF0, 0xE0, 0x20, 0xFF )   /* hovered widget     */
#define DBG_COL_ACTIVE      IMGUI_COLOR( 0xF0, 0x50, 0x20, 0xFF )   /* active widget      */
#define DBG_COL_WIN         IMGUI_COLOR( 0xE0, 0x30, 0xE0, 0xA0 )   /* window frame       */
#define DBG_COL_WIN_HOVER   IMGUI_COLOR( 0xF0, 0x80, 0xF0, 0xFF )   /* hover window frame */
#define DBG_COL_RESIZE      IMGUI_COLOR( 0x20, 0xC0, 0xF0, 0xA0 )   /* resize grab band   */
#define DBG_COL_RESIZE_HOT  IMGUI_COLOR( 0x40, 0xF0, 0xFF, 0xFF )   /* armed resize band  */

#define DBG_CLIP_FILL_A     0x18u                                   /* clip fill alpha    */

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

static struct
{
    u32             layers;     // active IMGUI_DBG_* bitmask; 0 = overlay off (no capture, no flush)

    imgui_draw_vert_t verts  [ IMGUI_DBG_MAX_VERTS ];
    u16               indices[ IMGUI_DBG_MAX_IDX ];

    u32             vert_count; // verts accumulated this frame
    u32             idx_count;  // indices accumulated this frame
    bool            overflow;   // a push was dropped this frame (cleared each frame)

    rhi_buffer_t    vb;         // CPU_TO_GPU vertex buffer, one region per frame-in-flight
    rhi_buffer_t    ib;         // CPU_TO_GPU index buffer, one region per frame-in-flight

} s_dbg;

/*----------------------------------------------------------------------------------------------
    Geometry append -- all overlay geometry routes through the font atlas white texel, so the
    entire list is one texture and flushes as a single draw call.  Mirrors draw_push_rect_filled
    but targets s_dbg and applies no clip (the overlay is always full-screen, always on top).
----------------------------------------------------------------------------------------------*/

static void
dbg_quad( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    if ( s_dbg.vert_count + 4 > IMGUI_DBG_MAX_VERTS || s_dbg.idx_count + 6 > IMGUI_DBG_MAX_IDX )
    {
        s_dbg.overflow = true;
        return;
    }

    /* Pixel-grid snap (same rationale as the UI list: keep 1px outlines crisp). */
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );

    f32 wu, wv;
    font_white_uv( &wu, &wv );

    u16 base = (u16)s_dbg.vert_count;

    imgui_draw_vert_t* v = &s_dbg.verts[ s_dbg.vert_count ];
    v[ 0 ] = ( imgui_draw_vert_t ){ x,     y,     wu, wv, abgr };
    v[ 1 ] = ( imgui_draw_vert_t ){ x + w, y,     wu, wv, abgr };
    v[ 2 ] = ( imgui_draw_vert_t ){ x + w, y + h, wu, wv, abgr };
    v[ 3 ] = ( imgui_draw_vert_t ){ x,     y + h, wu, wv, abgr };
    s_dbg.vert_count += 4;

    u16* idx = &s_dbg.indices[ s_dbg.idx_count ];
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base + 0; idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
    s_dbg.idx_count += 6;
}

/* Hollow rectangle as four edge quads (corners not double-blended, matching draw_push_rect_outline). */
static void
dbg_rect_outline( imgui_rect_t r, f32 t, u32 abgr )
{
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    if ( t > r.h * 0.5f ) t = r.h * 0.5f;   /* keep edges from overlapping on a thin rect */

    dbg_quad( r.x,             r.y,             r.w,           t,             abgr );   /* top    */
    dbg_quad( r.x,             r.y + r.h - t,   r.w,           t,             abgr );   /* bottom */
    dbg_quad( r.x,             r.y + t,         t,             r.h - 2.0f * t, abgr );  /* left   */
    dbg_quad( r.x + r.w - t,   r.y + t,         t,             r.h - 2.0f * t, abgr );  /* right  */
}

/* Translucent fill (clip-rect interiors). */
static void
dbg_rect_fill( imgui_rect_t r, u32 abgr )
{
    if ( r.w > 0.0f && r.h > 0.0f )
        dbg_quad( r.x, r.y, r.w, r.h, abgr );
}

/*----------------------------------------------------------------------------------------------
    Capture entry points -- called via the DBG_* macros in imgui.c.  Each early-outs instantly
    when its layer is off, so with the overlay armed-but-layer-disabled the cost is one branch.
----------------------------------------------------------------------------------------------*/

static void
dbg_capture_widget( imgui_id_t id, imgui_rect_t r, bool hover, bool active )
{
    (void)id;
    if ( !( s_dbg.layers & IMGUI_DBG_INTERACT ) ) return;
    u32 c = active ? DBG_COL_ACTIVE : ( hover ? DBG_COL_HOVER : DBG_COL_WIDGET );
    dbg_rect_outline( r, 1.0f, c );
}

/* Color the clip rect by its stack depth so nested clips are distinguishable. */
static void
dbg_capture_clip( imgui_rect_t r, u32 depth )
{
    if ( !( s_dbg.layers & IMGUI_DBG_CLIP ) ) return;

    static const u32 depth_rgb[ 4 ] = {
        IMGUI_COLOR( 0x40, 0xC0, 0xF0, 0xFF ),   /* depth 1: cyan   */
        IMGUI_COLOR( 0xF0, 0xC0, 0x40, 0xFF ),   /* depth 2: amber  */
        IMGUI_COLOR( 0xC0, 0x60, 0xF0, 0xFF ),   /* depth 3: violet */
        IMGUI_COLOR( 0x60, 0xF0, 0x90, 0xFF ),   /* depth 4+: mint  */
    };
    u32 c = depth_rgb[ ( depth ? depth - 1u : 0u ) & 3u ];

    dbg_rect_fill( r, ( c & 0x00FFFFFFu ) | ( DBG_CLIP_FILL_A << 24 ) );   /* faint interior */
    dbg_rect_outline( r, 1.0f, c );                                        /* crisp border   */
}

static void
dbg_capture_window( imgui_rect_t r, bool is_hover )
{
    if ( !( s_dbg.layers & IMGUI_DBG_WINDOW ) ) return;
    dbg_rect_outline( r, is_hover ? 2.0f : 1.0f, is_hover ? DBG_COL_WIN_HOVER : DBG_COL_WIN );
}

static void
dbg_capture_resize( imgui_rect_t band, u8 hot_edges )
{
    if ( !( s_dbg.layers & IMGUI_DBG_RESIZE ) ) return;
    dbg_rect_outline( band, hot_edges ? 2.0f : 1.0f, hot_edges ? DBG_COL_RESIZE_HOT : DBG_COL_RESIZE );
}

/*----------------------------------------------------------------------------------------------
    Lifecycle
----------------------------------------------------------------------------------------------*/

/* Allocate the overlay's own VB/IB (one region per frame-in-flight).  Called from imgui_init()
   after imgui_render_init(); the shared pipeline + font sampler are reused, so nothing else is
   created here.  A failure here is non-fatal -- the overlay simply stays dark. */
static bool
imgui_debug_init( void )
{
    s_dbg.vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_DBG_VB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_dbg_vb",
    } );
    if ( !rhi_handle_valid( s_dbg.vb ) )
        return false;

    s_dbg.ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_DBG_IB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_INDEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_dbg_ib",
    } );
    if ( !rhi_handle_valid( s_dbg.ib ) )
    {
        rhi()->buffer_destroy( s_dbg.vb );
        s_dbg.vb = ( rhi_buffer_t ){ 0 };
        return false;
    }

    return true;
}

static void
imgui_debug_shutdown( void )
{
    if ( rhi_handle_valid( s_dbg.ib ) ) rhi()->buffer_destroy( s_dbg.ib );
    if ( rhi_handle_valid( s_dbg.vb ) ) rhi()->buffer_destroy( s_dbg.vb );
    memset( &s_dbg, 0, sizeof( s_dbg ) );   /* clears handles AND layers (overlay off after shutdown) */
}

/* Clear the per-frame geometry; layers + GPU handles persist.  Called from imgui_new_frame(). */
static void
imgui_debug_reset( void )
{
    s_dbg.vert_count = 0;
    s_dbg.idx_count  = 0;
    s_dbg.overflow   = false;
}

/* Flush the overlay: a second LOAD pass over the swapchain, reusing the imgui pipeline, with a
   single full-window scissor and one indexed draw (the whole list shares the atlas texture).
   No z-sort -- geometry is replayed in emit order, so the overlay sits on top of the UI pass.
   Called from imgui_render() after imgui_render_flush(). */
static void
imgui_debug_flush( rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( s_dbg.idx_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    if ( s_dbg.overflow )
    {
        static bool warned = false;
        if ( !warned )
        {
            printf( "[imgui] WARNING: debug overlay draw list overflow -- geometry dropped "
                    "(verts cap %u, idx cap %u).\n", IMGUI_DBG_MAX_VERTS, IMGUI_DBG_MAX_IDX );
            warned = true;
        }
    }

    /* This frame's region, so the upload never clobbers data a prior in-flight frame still reads. */
    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = frame * (u32)IMGUI_DBG_VB_REGION_BYTES;
    u32 ib_off = frame * (u32)IMGUI_DBG_IB_REGION_BYTES;

    rhi()->buffer_write( s_dbg.vb, s_dbg.verts,   s_dbg.vert_count * sizeof( imgui_draw_vert_t ), vb_off );
    rhi()->buffer_write( s_dbg.ib, s_dbg.indices, s_dbg.idx_count  * sizeof( u16 ),               ib_off );

    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_LOAD,    /* preserve the UI just rendered */
        .store_op = RHI_STORE_OP_STORE,
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f, .width = (f32)win_w, .height = (f32)win_h,
        .min_depth = 0.0f, .max_depth = 1.0f,
    } );

    rhi()->cmd_bind_pipeline( cmd, s_render.pipeline );   /* shared imgui pipeline */
    rhi()->cmd_bind_bindless( cmd );
    rhi()->cmd_bind_vertex_buffer( cmd, s_dbg.vb, vb_off );
    rhi()->cmd_bind_index_buffer( cmd, s_dbg.ib, ib_off, RHI_INDEX_TYPE_UINT16 );

    /* Full-window scissor: the overlay is never clipped to any window. */
    rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){ .x = 0, .y = 0, .width = win_w, .height = win_h } );

    imgui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );
    push.samp_idx = s_render.font_sampler_idx;
    push.tex_idx  = font_atlas_idx();   /* everything routes through the atlas white texel */
    rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

    rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
        .index_count    = s_dbg.idx_count,
        .instance_count = 1,
        .first_index    = 0,
        .vertex_offset  = 0,
        .first_instance = 0,
    } );

    rhi()->cmd_end_rendering( cmd );
}

/*----------------------------------------------------------------------------------------------
    Public layer control (also defined -- as no-ops -- in the disabled build below)
----------------------------------------------------------------------------------------------*/

void imgui_debug_set_layers( u32 layers ) { s_dbg.layers = layers; }
u32  imgui_debug_get_layers( void )       { return s_dbg.layers; }

#else  /* !IMGUI_DEBUG_OVERLAY -- overlay compiled out; only the public setters remain (no-ops) */

void imgui_debug_set_layers( u32 layers ) { (void)layers; }
u32  imgui_debug_get_layers( void )       { return 0u; }

#endif /* IMGUI_DEBUG_OVERLAY */

// clang-format on
/*============================================================================================*/
