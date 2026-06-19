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

    Capture stores compact rect commands tagged with a viewport index.  At flush time for
    viewport V, commands are filtered and expanded inline into scratch VB/IB, uploaded once,
    and drawn -- no per-viewport pre-allocated vertex arrays.

    Active layers are chosen at runtime with imgui()->debug_set_layers( imgui_dbg_layer_t mask ).

    Included by imgui.c after imgui_ctx.c so s_build and s_render, render_ortho, imgui_push_t,
    and the font_* atlas helpers are all in scope.

==============================================================================================*/
// clang-format off

#ifdef IMGUI_DEBUG_OVERLAY

/*==============================================================================================
    Debug Caps
==============================================================================================*/

/* Max rect commands accumulated per frame across all viewports. */
#define IMGUI_DBG_MAX_CMDS  2048

/* Scratch geometry caps for one viewport flush.  Worst case: every command is an outline
   (4 edge quads) = 16 verts / 24 idx each.  u16 indices are safe up to 65535 verts. */
#define IMGUI_DBG_FLUSH_MAX_VERTS ( IMGUI_DBG_MAX_CMDS * 16 )
#define IMGUI_DBG_FLUSH_MAX_IDX   ( IMGUI_DBG_MAX_CMDS * 24 )

/* GPU buffer regions: one per viewport per frame-in-flight so concurrent viewport flushes
   (same frame index) do not overwrite each other. */
#define IMGUI_DBG_VB_REGION_BYTES ( IMGUI_DBG_FLUSH_MAX_VERTS * sizeof( imgui_draw_vert_t ) )
#define IMGUI_DBG_IB_REGION_BYTES ( IMGUI_DBG_FLUSH_MAX_IDX   * sizeof( u16 ) )

/*==============================================================================================
    Debug Colors
==============================================================================================*/

/* Palette (IMGUI_COLOR packs R,G,B,A bytes). */
#define DBG_COL_WIDGET      IMGUI_COLOR( 0x30, 0xE0, 0x30, 0xC0 )
#define DBG_COL_HOVER       IMGUI_COLOR( 0xF0, 0xE0, 0x20, 0xFF )
#define DBG_COL_ACTIVE      IMGUI_COLOR( 0xF0, 0x50, 0x20, 0xFF )
#define DBG_COL_WIN         IMGUI_COLOR( 0xE0, 0x30, 0xE0, 0xA0 )
#define DBG_COL_WIN_HOVER   IMGUI_COLOR( 0xF0, 0x80, 0xF0, 0xFF )
#define DBG_COL_RESIZE      IMGUI_COLOR( 0x20, 0xC0, 0xF0, 0xA0 )
#define DBG_COL_RESIZE_HOT  IMGUI_COLOR( 0x40, 0xF0, 0xFF, 0xFF )
#define DBG_CLIP_FILL_A     0x18u

/*==============================================================================================
    Debug Rect Command -- one entry per captured rect, tagged with its target viewport.
    thickness == 0 means filled quad; thickness > 0 means hollow outline.
==============================================================================================*/

typedef struct
{
    imgui_rect_t r;             /* rect geometry in pixels (x0,y0 = top-left) */
    u32          abgr;          /* rect color, packed like IMGUI_COLOR macro */
    f32          thickness;     /* outline thickness in pixels; 0.0f = filled */
    u8           vp;            /* target viewport index (IMGUI_MAX_VIEWPORTS <= 255) */
} dbg_cmd_t;

/*==============================================================================================
    Debug State
==============================================================================================*/

static struct
{
    u32        layers;      /* active IMGUI_DBG_* bitmask */
    u32        cmd_count;   /* count of valid entries in cmds[] this frame; reset to 0 at frame begin */
    bool       overflow;    /* a push was dropped this frame */

    dbg_cmd_t  cmds[ IMGUI_DBG_MAX_CMDS ];
    
    /* Scratch buffers: filled per-viewport at flush time, discarded after upload. */

    imgui_draw_vert_t scratch_verts [ IMGUI_DBG_FLUSH_MAX_VERTS ];
    u16               scratch_idx   [ IMGUI_DBG_FLUSH_MAX_IDX   ];

    rhi_buffer_t vb;        /* one region per viewport per frame-in-flight */
    rhi_buffer_t ib;

} s_dbg;

/*----------------------------------------------------------------------------------------------
    Command push helpers
----------------------------------------------------------------------------------------------*/

static void
dbg_push_fill( u32 vp, imgui_rect_t r, u32 abgr )
{
    if ( s_dbg.cmd_count >= IMGUI_DBG_MAX_CMDS ) { s_dbg.overflow = true; return; }
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    s_dbg.cmds[ s_dbg.cmd_count++ ] = ( dbg_cmd_t ){ r, abgr, 0.0f, (u8)vp };
}

static void
dbg_push_outline( u32 vp, imgui_rect_t r, f32 thickness, u32 abgr )
{
    if ( s_dbg.cmd_count >= IMGUI_DBG_MAX_CMDS ) { s_dbg.overflow = true; return; }
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    s_dbg.cmds[ s_dbg.cmd_count++ ] = ( dbg_cmd_t ){ r, abgr, thickness, (u8)vp };
}

/*----------------------------------------------------------------------------------------------
    Capture entry points -- called via the DBG_* macros in imgui.c.
    s_build.cur_viewport routes each command to the correct viewport.
----------------------------------------------------------------------------------------------*/

static void
dbg_capture_widget( imgui_id_t id, imgui_rect_t r, bool hover, bool active )
{
    (void)id;
    if ( !( s_dbg.layers & IMGUI_DBG_INTERACT ) ) return;
    u32 c = active ? DBG_COL_ACTIVE : ( hover ? DBG_COL_HOVER : DBG_COL_WIDGET );
    dbg_push_outline( s_build.cur_viewport, r, 1.0f, c );
}

static void
dbg_capture_clip( imgui_rect_t r, u32 depth )
{
    if ( !( s_dbg.layers & IMGUI_DBG_CLIP ) ) return;

    static const u32 depth_rgb[ 4 ] = {
        IMGUI_COLOR( 0x40, 0xC0, 0xF0, 0xFF ),
        IMGUI_COLOR( 0xF0, 0xC0, 0x40, 0xFF ),
        IMGUI_COLOR( 0xC0, 0x60, 0xF0, 0xFF ),
        IMGUI_COLOR( 0x60, 0xF0, 0x90, 0xFF ),
    };
    u32 vp = s_build.cur_viewport;
    u32 c  = depth_rgb[ ( depth ? depth - 1u : 0u ) & 3u ];
    dbg_push_fill   ( vp, r, ( c & 0x00FFFFFFu ) | ( DBG_CLIP_FILL_A << 24 ) );
    dbg_push_outline( vp, r, 1.0f, c );
}

static void
dbg_capture_window( imgui_rect_t r, bool is_hover )
{
    if ( !( s_dbg.layers & IMGUI_DBG_WINDOW ) ) return;
    dbg_push_outline( s_build.cur_viewport, r,
                      is_hover ? 2.0f : 1.0f,
                      is_hover ? DBG_COL_WIN_HOVER : DBG_COL_WIN );
}

static void
dbg_capture_resize( imgui_rect_t band, u8 hot_edges )
{
    if ( !( s_dbg.layers & IMGUI_DBG_RESIZE ) ) return;
    dbg_push_outline( s_build.cur_viewport, band,
                      hot_edges ? 2.0f : 1.0f,
                      hot_edges ? DBG_COL_RESIZE_HOT : DBG_COL_RESIZE );
}

/*----------------------------------------------------------------------------------------------
    Lifecycle
----------------------------------------------------------------------------------------------*/

static bool
imgui_debug_init( void )
{
    s_dbg.vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_MAX_VIEWPORTS * IMGUI_DBG_VB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "imgui_dbg_vb",
    } );
    if ( !rhi_handle_valid( s_dbg.vb ) ) return false;

    s_dbg.ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * IMGUI_MAX_VIEWPORTS * IMGUI_DBG_IB_REGION_BYTES,
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
    memset( &s_dbg, 0, sizeof( s_dbg ) );
}

static void
imgui_debug_reset( void )
{
    s_dbg.cmd_count = 0;
    s_dbg.overflow  = false;
}

/*----------------------------------------------------------------------------------------------
    Flush -- expand commands for viewport vp into scratch geometry, upload, draw.
    Scratch memory is treated as write-once per call: overwritten next flush, never read back.
----------------------------------------------------------------------------------------------*/

/* Emit one quad into the scratch buffers.  Caller guarantees capacity. */
static void
dbg_expand_quad( f32 wu, f32 wv, f32 x, f32 y, f32 w, f32 h, u32 abgr,
                 u32* vc, u32* ic )
{
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );

    u16 base = (u16)*vc;
    imgui_draw_vert_t* v = &s_dbg.scratch_verts[ *vc ];
    v[ 0 ] = ( imgui_draw_vert_t ){ x,     y,     wu, wv, abgr };
    v[ 1 ] = ( imgui_draw_vert_t ){ x + w, y,     wu, wv, abgr };
    v[ 2 ] = ( imgui_draw_vert_t ){ x + w, y + h, wu, wv, abgr };
    v[ 3 ] = ( imgui_draw_vert_t ){ x,     y + h, wu, wv, abgr };
    *vc += 4;

    u16* idx = &s_dbg.scratch_idx[ *ic ];
    idx[ 0 ] = base;     idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base;     idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
    *ic += 6;
}

static void
imgui_debug_flush( imgui_vp_t vp, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( vp < 0 || vp >= (imgui_vp_t)IMGUI_MAX_VIEWPORTS ) return;
    if ( s_dbg.cmd_count == 0 || !rhi_cmd_valid( cmd ) ) return;

    u8  v  = (u8)vp;
    u32 vc = 0, ic = 0;

    f32 wu, wv_uv;
    font_white_uv( &wu, &wv_uv );

    /* Filter and expand commands for this viewport into scratch geometry. */
    for ( u32 i = 0; i < s_dbg.cmd_count; ++i )
    {
        const dbg_cmd_t* c = &s_dbg.cmds[ i ];
        if ( c->vp != v ) continue;

        if ( c->thickness <= 0.0f )
        {
            /* Filled quad -- 4 verts, 6 idx. */
            if ( vc + 4 > IMGUI_DBG_FLUSH_MAX_VERTS || ic + 6 > IMGUI_DBG_FLUSH_MAX_IDX ) break;
            dbg_expand_quad( wu, wv_uv, c->r.x, c->r.y, c->r.w, c->r.h, c->abgr, &vc, &ic );
        }
        else
        {
            /* Hollow outline -- 4 edge quads = 16 verts, 24 idx. */
            if ( vc + 16 > IMGUI_DBG_FLUSH_MAX_VERTS || ic + 24 > IMGUI_DBG_FLUSH_MAX_IDX ) break;
            f32 t  = c->thickness;
            f32 rw = c->r.w, rh = c->r.h;
            f32 rx = c->r.x, ry = c->r.y;
            if ( t > rh * 0.5f ) t = rh * 0.5f;
            dbg_expand_quad( wu, wv_uv, rx,          ry,              rw,        t,              c->abgr, &vc, &ic );  /* top    */
            dbg_expand_quad( wu, wv_uv, rx,          ry + rh - t,     rw,        t,              c->abgr, &vc, &ic );  /* bottom */
            dbg_expand_quad( wu, wv_uv, rx,          ry + t,          t,         rh - 2.0f * t,  c->abgr, &vc, &ic );  /* left   */
            dbg_expand_quad( wu, wv_uv, rx + rw - t, ry + t,          t,         rh - 2.0f * t,  c->abgr, &vc, &ic );  /* right  */
        }
    }

    if ( ic == 0 ) return;

    if ( s_dbg.overflow )
    {
        static bool warned = false;
        if ( !warned )
        {
            printf( "[imgui] WARNING: debug overlay command list overflow -- some rects dropped "
                    "(cap %u).\n", IMGUI_DBG_MAX_CMDS );
            warned = true;
        }
    }

    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = ( frame * IMGUI_MAX_VIEWPORTS + (u32)vp ) * (u32)IMGUI_DBG_VB_REGION_BYTES;
    u32 ib_off = ( frame * IMGUI_MAX_VIEWPORTS + (u32)vp ) * (u32)IMGUI_DBG_IB_REGION_BYTES;

    rhi()->buffer_write( s_dbg.vb, s_dbg.scratch_verts, vc * sizeof( imgui_draw_vert_t ), vb_off );
    rhi()->buffer_write( s_dbg.ib, s_dbg.scratch_idx,   ic * sizeof( u16 ),               ib_off );

    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_LOAD,
        .store_op = RHI_STORE_OP_STORE,
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f, .width = (f32)win_w, .height = (f32)win_h,
        .min_depth = 0.0f, .max_depth = 1.0f,
    } );
    rhi()->cmd_bind_pipeline     ( cmd, s_render.pipeline );
    rhi()->cmd_bind_bindless     ( cmd );
    rhi()->cmd_bind_vertex_buffer( cmd, s_dbg.vb, vb_off );
    rhi()->cmd_bind_index_buffer ( cmd, s_dbg.ib, ib_off, RHI_INDEX_TYPE_UINT16 );
    rhi()->cmd_set_scissor       ( cmd, &( rhi_rect_t ){ .x = 0, .y = 0, .width = win_w, .height = win_h } );

    imgui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );
    push.samp_idx = s_render.font_sampler_idx;
    push.tex_idx  = font_atlas_idx();
    rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

    rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
        .index_count    = ic,
        .instance_count = 1,
        .first_index    = 0,
        .vertex_offset  = 0,
        .first_instance = 0,
    } );

    rhi()->cmd_end_rendering( cmd );
}

/*----------------------------------------------------------------------------------------------
    Public layer control
----------------------------------------------------------------------------------------------*/

void imgui_debug_set_layers( u32 layers ) { s_dbg.layers = layers; }
u32  imgui_debug_get_layers( void )       { return s_dbg.layers; }

#else  /* !IMGUI_DEBUG_OVERLAY */

void imgui_debug_set_layers( u32 layers ) { (void)layers; }
u32  imgui_debug_get_layers( void )       { return 0u; }

#endif /* IMGUI_DEBUG_OVERLAY */

// clang-format on
/*============================================================================================*/
