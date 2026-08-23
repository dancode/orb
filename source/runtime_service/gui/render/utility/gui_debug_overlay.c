/*==============================================================================================

    runtime_service/gui/render/gui_debug_overlay.c -- Bolt-on debug overlay.

    A second, independent draw list that is emitted from inside the regular gui code (via the
    DBG_* capture macros in debug/gui_debug.h) and flushed LAST, on top of the finished UI.  It visualizes
    things the normal UI hides: every widget's interaction rect, the window edge-resize grab
    bands, window frames (with the hover window highlighted), and the clip/scissor stack.

    It is deliberately NOT intertwined with the main draw list (s_draw):
      - its own quad records with their own (small) caps, so it can never starve the UI
        budget and make real widgets overflow;
      - its own quad storage buffer, reusing only the shared gui pipeline + samplers;
      - no z-sort and no per-window clip on flush -- overlay order is emit order, always topmost;
      - the whole subsystem (state, buffers, capture, flush) is compiled out unless
        GUI_DEBUG_OVERLAY is defined (Debug builds), leaving only the two no-op public setters.

    Capture stores compact rect commands tagged with a viewport index.  At flush time for
    viewport V, commands are filtered and expanded inline into scratch quad records, uploaded
    once, and drawn bufferless through the shared quad pipeline.

    Active layers are chosen at runtime with gui()->debug_set_layers( gui_dbg_layer_t mask ).

    Included by gui_render.c after the whole pipeline -- it needs s_render, render_ortho, and
    gui_push_t from pipeline/gui_render_submit.c in scope.  The ambient build viewport it tags rects with
    lives in the core unit (s_build, core/gui_ctx.c), reached across the seam via dbg_build_viewport().

==============================================================================================*/
// clang-format off

#ifdef GUI_DEBUG_OVERLAY

/*==============================================================================================
    Debug Caps
==============================================================================================*/

/* Max rect commands accumulated per frame across all viewports. */
#define GUI_DBG_MAX_CMDS  1024

/* Scratch quad cap for one viewport flush.  Worst case: every command is an outline (4 edge
   quads). */
#define GUI_DBG_FLUSH_MAX_QUADS ( GUI_DBG_MAX_CMDS * 4 )

/* Quad-table regions: one per viewport per frame-in-flight so concurrent viewport flushes
   (same frame index) do not overwrite each other. */
#define GUI_DBG_QUAD_REGION_BYTES ( GUI_DBG_FLUSH_MAX_QUADS * sizeof( gui_quad_t ) )

/*==============================================================================================
    Debug Colors
==============================================================================================*/

/* Palette (GUI_COLOR packs R,G,B,A bytes). */
#define DBG_COL_WIDGET      GUI_COLOR( 0x30, 0xE0, 0x30, 0xC0 )
#define DBG_COL_HOVER       GUI_COLOR( 0xF0, 0xE0, 0x20, 0xFF )
#define DBG_COL_ACTIVE      GUI_COLOR( 0xF0, 0x50, 0x20, 0xFF )
#define DBG_COL_WIN         GUI_COLOR( 0xE0, 0x30, 0xE0, 0xA0 )
#define DBG_COL_WIN_HOVER   GUI_COLOR( 0xF0, 0x80, 0xF0, 0xFF )
#define DBG_COL_RESIZE      GUI_COLOR( 0x20, 0xC0, 0xF0, 0xA0 )
#define DBG_COL_RESIZE_HOT  GUI_COLOR( 0x40, 0xF0, 0xFF, 0xFF )
#define DBG_COL_VIEW        GUI_COLOR( 0x20, 0xF0, 0xC0, 0xFF )   /* region view outline (teal) */
#define DBG_COL_GUTTER      GUI_COLOR( 0xF0, 0x90, 0x20, 0x60 )   /* reserved gutter fill (orange) */
#define DBG_COL_HITCLIP     GUI_COLOR( 0xFF, 0xFF, 0xFF, 0x18 )   /* body interaction clip tint */
#define DBG_CLIP_FILL_A     0x24u   /* base tint alpha for the outermost clip rect */
#define DBG_CLIP_FILL_STEP  0x1Cu   /* added per nesting level so a child clip reads bolder */
#define DBG_CLIP_FILL_MAX   0xA0u   /* cap so deep nesting stays translucent, not opaque */

/*==============================================================================================
    Debug Rect Command -- one entry per captured rect, tagged with its target viewport.
    thickness == 0 means filled quad; thickness > 0 means hollow outline.
==============================================================================================*/

typedef struct
{
    gui_rect_t  r;             /* rect geometry in pixels (x0,y0 = top-left) */
    u32          abgr;         /* rect color, packed like GUI_COLOR macro */
    f32          thickness;    /* outline thickness in pixels; 0.0f = filled */
    u8           vp;           /* target viewport index (GUI_MAX_VIEWPORTS <= 255) */
} dbg_cmd_t;

/*==============================================================================================
    Debug State
==============================================================================================*/

static struct
{
    u32        layers;      /* active GUI_DBG_* bitmask */
    u32        cmd_count;   /* count of valid entries in cmds[] this frame; reset to 0 at frame begin */
    bool       overflow;    /* a push was dropped this frame */

    dbg_cmd_t  cmds[ GUI_DBG_MAX_CMDS ];

    /* Scratch quads: filled per-viewport at flush time, discarded after upload. */

    gui_quad_t scratch_quads[ GUI_DBG_FLUSH_MAX_QUADS ];

    rhi_buffer_t quads;      /* storage buffer: one region per viewport per frame-in-flight */
    u32          quads_idx;  /* bindless buffer slot (0 = init failed) */

} s_dbg;

/*==============================================================================================
    Debug Name Registry -- id -> source string, so the state overlay can show a readable label
    instead of a hash.  Populated every frame at the DBG_NAME( id, str ) call sites (item_id,
    window_begin_ex, region/child/table id mint points); read back by gui_debug_name(), which the
    state overlay calls from overlay_state().  Open-addressed like gui_state_get (core/gui_state.c):
    linear probe, home-bucket overwrite when full -- a rare degradation, not an overflow, and fine
    for a debug tool.  No staleness tracking; a name simply goes stale (but harmless) once its id
    stops being emitted.
==============================================================================================*/

#define GUI_DBG_NAME_CAP   24                           /* bytes per entry, incl NUL              */
#define GUI_DBG_NAME_SLOTS 256                          /* power of two                           */
#define GUI_DBG_NAME_MASK  ( GUI_DBG_NAME_SLOTS - 1 )

typedef struct
{
    gui_id_t id;                                        /* 0 = empty slot */
    char     name[ GUI_DBG_NAME_CAP ];                  /* source string that minted the id */

} gui_dbg_name_entry_t;

static gui_dbg_name_entry_t s_dbg_names[ GUI_DBG_NAME_SLOTS ];
static bool                 s_dbg_names_wanted = false;   /* armed by the first gui_debug_name read */

void
dbg_name_register( gui_id_t id, const char* str )
{
    /* Demand-driven: every labeled widget mints through here every frame, so the probe + copy is
       real per-item emit cost.  Nothing registers until a reader (gui_debug_name below) arms the
       registry -- the first frame a debug view opens shows raw hashes, full names from the next
       mint on.  Sticky once armed: the views re-read every frame they are open. */
    if ( !s_dbg_names_wanted ) return;
    if ( id == GUI_ID_NONE || !str ) return;

    u32 bucket = id & GUI_DBG_NAME_MASK;
    for ( u32 i = 0; i < GUI_DBG_NAME_SLOTS; ++i )
    {
        u32 slot = ( bucket + i ) & GUI_DBG_NAME_MASK;
        gui_dbg_name_entry_t* e = &s_dbg_names[ slot ];
        if ( e->id == id || e->id == GUI_ID_NONE )
        {
            e->id = id;
            size_t n = strlen( str );
            if ( n >= GUI_DBG_NAME_CAP ) n = GUI_DBG_NAME_CAP - 1;
            memcpy( e->name, str, n );
            e->name[ n ] = '\0';
            return;
        }
    }
    /* Table full of distinct live ids (256 named things in one frame) -- overwrite the home
       bucket rather than growing; a rare degradation, not a crash. */
    gui_dbg_name_entry_t* home = &s_dbg_names[ bucket ];
    home->id = id;
    strncpy( home->name, str, GUI_DBG_NAME_CAP - 1 );
    home->name[ GUI_DBG_NAME_CAP - 1 ] = '\0';
}

const char*
gui_debug_name( gui_id_t id )
{
    s_dbg_names_wanted = true;   /* a reader exists -- arm the registry (see dbg_name_register) */
    if ( id == GUI_ID_NONE ) return NULL;
    u32 bucket = id & GUI_DBG_NAME_MASK;
    for ( u32 i = 0; i < GUI_DBG_NAME_SLOTS; ++i )
    {
        gui_dbg_name_entry_t* e = &s_dbg_names[ ( bucket + i ) & GUI_DBG_NAME_MASK ];
        if ( e->id == id           ) return e->name;
        if ( e->id == GUI_ID_NONE  ) return NULL;   /* empty ends the chain */
    }
    return NULL;
}

/*==============================================================================================
    Command push helpers
==============================================================================================*/

static void
dbg_push_fill( i32 vp, gui_rect_t r, u32 abgr )
{
    if ( s_dbg.cmd_count >= GUI_DBG_MAX_CMDS ) { s_dbg.overflow = true; return; }
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    s_dbg.cmds[ s_dbg.cmd_count++ ] = ( dbg_cmd_t ){ r, abgr, 0.0f, (u8)vp };
}

static void
dbg_push_outline( i32 vp, gui_rect_t r, f32 thickness, u32 abgr )
{
    if ( s_dbg.cmd_count >= GUI_DBG_MAX_CMDS ) { s_dbg.overflow = true; return; }
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    s_dbg.cmds[ s_dbg.cmd_count++ ] = ( dbg_cmd_t ){ r, abgr, thickness, (u8)vp };
}

/*==============================================================================================
    Capture entry points -- called via the DBG_* macros in debug/gui_debug.h.
    dbg_build_viewport() routes each command to the correct viewport.
==============================================================================================*/

void
dbg_capture_widget( gui_id_t id, gui_rect_t r, bool hover, bool active )
{
    (void)id;
    if ( !( s_dbg.layers & GUI_DBG_INTERACT ) ) return;
    u32 c = active ? DBG_COL_ACTIVE : ( hover ? DBG_COL_HOVER : DBG_COL_WIDGET );
    dbg_push_outline( dbg_build_viewport(), r, 1.0f, c );
}

void
dbg_capture_layout( gui_rect_t r )
{
    if ( !( s_dbg.layers & GUI_DBG_LAYOUT ) ) return;
    u32 c = GUI_COLOR( 0xFF, 0x00, 0xFF, 0x80 ); // Magenta outline for layout bounds
    dbg_push_outline( dbg_build_viewport(), r, 1.0f, c );
}

void
dbg_capture_clip( gui_rect_t r, u32 depth )
{
    if ( !( s_dbg.layers & GUI_DBG_CLIP ) ) return;

    static const u32 depth_rgb[ 4 ] = {
        GUI_COLOR( 0x40, 0xC0, 0xF0, 0xFF ),
        GUI_COLOR( 0xF0, 0xC0, 0x40, 0xFF ),
        GUI_COLOR( 0xC0, 0x60, 0xF0, 0xFF ),
        GUI_COLOR( 0x60, 0xF0, 0x90, 0xFF ),
    };
    i32 vp = dbg_build_viewport();
    u32 lvl  = depth ? depth - 1u : 0u;       /* 0 = outermost (root/window) clip */
    u32 c    = depth_rgb[ lvl & 3u ];

    /* Nested clips read as progressively bolder: each level deeper adds tint alpha and an extra
       pixel of outline, so a child's scissor stands out clearly on top of its parent's instead of
       washing out into the faint base tint. */
    u32 fill_a = DBG_CLIP_FILL_A + DBG_CLIP_FILL_STEP * lvl;
    if ( fill_a > DBG_CLIP_FILL_MAX ) fill_a = DBG_CLIP_FILL_MAX;
    f32 t = 1.0f + (f32)lvl;
    if ( t > 4.0f ) t = 4.0f;

    dbg_push_fill   ( vp, r, ( c & 0x00FFFFFFu ) | ( fill_a << 24 ) );
    dbg_push_outline( vp, r, t, c );
}

/* Region screen geometry: the view rect (teal outline), the reserved scrollbar gutters that the
   view reservation carved out (orange fill, sitting exactly on the view's right / bottom edges),
   and the body's interaction clip (faint white tint -- where widgets can actually hover).  Any
   content interacting outside the teal box, or a gutter the tint reaches into, is the geometry
   drift this layer exists to expose at a glance. */
void
dbg_capture_region( gui_rect_t view, gui_rect_t hit_clip, f32 sb_w, f32 sb_h )
{
    if ( !( s_dbg.layers & GUI_DBG_REGION ) ) return;
    i32 vp = dbg_build_viewport();

    dbg_push_fill( vp, hit_clip, DBG_COL_HITCLIP );
    if ( sb_w > 0.0f )
        dbg_push_fill( vp, ( gui_rect_t ){ view.x + view.w, view.y, sb_w, view.h }, DBG_COL_GUTTER );
    if ( sb_h > 0.0f )
        dbg_push_fill( vp, ( gui_rect_t ){ view.x, view.y + view.h, view.w, sb_h }, DBG_COL_GUTTER );
    dbg_push_outline( vp, view, 1.0f, DBG_COL_VIEW );
}

void
dbg_capture_window( gui_rect_t r, bool is_hover )
{
    if ( !( s_dbg.layers & GUI_DBG_WINDOW ) ) return;
    dbg_push_outline( dbg_build_viewport(), r,
                      is_hover ? 2.0f : 1.0f,
                      is_hover ? DBG_COL_WIN_HOVER : DBG_COL_WIN );
}

void
dbg_capture_resize( gui_rect_t band, u8 hot_edges )
{
    if ( !( s_dbg.layers & GUI_DBG_RESIZE ) ) return;
    dbg_push_outline( dbg_build_viewport(), band,
                      hot_edges ? 2.0f : 1.0f,
                      hot_edges ? DBG_COL_RESIZE_HOT : DBG_COL_RESIZE );
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

bool
dbg_init( void )
{
    s_dbg.quads = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS * GUI_DBG_QUAD_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_dbg_quads",
    } );
    if ( !rhi_handle_valid( s_dbg.quads ) ) return false;

    s_dbg.quads_idx = rhi()->register_buffer( s_dbg.quads );
    if ( s_dbg.quads_idx == 0 )
    {
        rhi()->buffer_destroy( s_dbg.quads );
        s_dbg.quads = ( rhi_buffer_t ){ 0 };
        return false;
    }
    return true;
}

void
dbg_shutdown( void )
{
    if ( s_dbg.quads_idx )
        rhi()->unregister_buffer( s_dbg.quads_idx );
    if ( rhi_handle_valid( s_dbg.quads ) )
        rhi()->buffer_destroy( s_dbg.quads );
    memset( &s_dbg, 0, sizeof( s_dbg ) );
}

void
dbg_reset( void )
{
    s_dbg.cmd_count = 0;
    s_dbg.overflow  = false;
}

/*==============================================================================================
    Flush -- expand commands for viewport vp into scratch quad records, upload, draw.
    Scratch memory is treated as write-once per call: overwritten next flush, never read back.
==============================================================================================*/

/* Emit one filled rect as a quad record.  Caller guarantees capacity.  Every lane the overlay
   does not use stays zero, and the whole index word with them: style 0 (the one record dbg_flush
   writes at the overlay entry), clip 0 (clip_buf 0 means no clipping), rule EXACT, no fx record,
   uv unused under the record's OP_SELF. */
static void
dbg_expand_quad( f32 x, f32 y, f32 w, f32 h, u32 abgr, u32* qc )
{
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );
    s_dbg.scratch_quads[ ( *qc )++ ] = ( gui_quad_t ){
        .cx   = gui_quad_pos_pack( x + w * 0.5f ),
        .cy   = gui_quad_pos_pack( y + h * 0.5f ),
        .hw   = gui_quad_ext_pack( w * 0.5f ),
        .hh   = gui_quad_ext_pack( h * 0.5f ),
        .abgr = abgr,
    };
}

void
dbg_flush( i32 vp, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( vp < 0 || vp >= GUI_MAX_VIEWPORTS ) return;
    if ( s_dbg.cmd_count == 0 || !rhi_cmd_valid( cmd ) ) return;
    if ( s_dbg.quads_idx == 0 ) return;

    u8  v  = (u8)vp;
    u32 qc = 0;

    /* Filter and expand commands for this viewport into scratch quads. */
    for ( u32 i = 0; i < s_dbg.cmd_count; ++i )
    {
        const dbg_cmd_t* c = &s_dbg.cmds[ i ];
        if ( c->vp != v ) continue;

        if ( c->thickness <= 0.0f )
        {
            /* Filled rect -- one quad. */
            if ( qc + 1 > GUI_DBG_FLUSH_MAX_QUADS ) break;
            dbg_expand_quad( c->r.x, c->r.y, c->r.w, c->r.h, c->abgr, &qc );
        }
        else
        {
            /* Hollow outline -- 4 edge quads. */
            if ( qc + 4 > GUI_DBG_FLUSH_MAX_QUADS ) break;
            f32 t  = c->thickness;
            f32 rw = c->r.w, rh = c->r.h;
            f32 rx = c->r.x, ry = c->r.y;
            if ( t > rh * 0.5f ) t = rh * 0.5f;
            dbg_expand_quad( rx,          ry,          rw, t,             c->abgr, &qc );  /* top    */
            dbg_expand_quad( rx,          ry + rh - t, rw, t,             c->abgr, &qc );  /* bottom */
            dbg_expand_quad( rx,          ry + t,      t,  rh - 2.0f * t, c->abgr, &qc );  /* left   */
            dbg_expand_quad( rx + rw - t, ry + t,      t,  rh - 2.0f * t, c->abgr, &qc );  /* right  */
        }
    }

    if ( qc == 0 ) return;

    if ( s_dbg.overflow )
        GUI_WARN_ONCE( "debug overlay command list overflow -- some rects dropped (cap %u).\n",
                       GUI_DBG_MAX_CMDS );

    u32 frame    = rhi()->cmd_frame_index( cmd );
    u32 region   = frame * GUI_MAX_VIEWPORTS + (u32)vp;
    u32 quad_off = region * (u32)GUI_DBG_QUAD_REGION_BYTES;

    rhi()->buffer_write( s_dbg.quads, s_dbg.scratch_quads, qc * sizeof( gui_quad_t ), quad_off );

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
    rhi()->cmd_bind_pipeline( cmd, s_render.pipeline_quad );
    rhi()->cmd_bind_bindless( cmd );
    rhi()->cmd_set_scissor  ( cmd, &( rhi_rect_t ){ .x = 0, .y = 0, .width = win_w, .height = win_h } );

    gui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );
    push.samp_point = s_render.font_sampler_idx;
    push.samp_image = s_render.image_sampler_idx ? s_render.image_sampler_idx
                                                 : s_render.font_sampler_idx;
    push.dbg_flat   = 0u;   /* the overlay always renders normally, never flat/batch-tinted */
    push.dbg_tint   = 0u;
    push.clip_buf   = 0u;   /* no clip table: overlay quads carry clip 0 and clip nothing */
    push.clip_base  = 0u;
    push.time       = s_render.fx_time;   /* overlay geometry is fx-free, but the block must be
                                             fully initialized */
    push.quad_buf   = s_dbg.quads_idx;    /* the overlay's OWN quad table, this flush's region */
    push.quad_base  = region * (u32)GUI_DBG_FLUSH_MAX_QUADS;

    /* The overlay's single prim record, refreshed every flush because the one thing in it that
       is not a constant -- the atlas bindless slot -- can move when the atlas is rebuilt.  Every
       overlay quad carries style 0 against this base, so one entry serves the whole surface.
       OP_SELF: solid colour, the texel is never consulted.  It sits in the prim table's fixed
       header (GUI_PRIM_OVERLAY_ORIGIN, one record per (frame, viewport)) and therefore cannot
       collide with a window's records or move when the claim space grows. */
    u32        prim_region = region;
    gui_prim_t overlay_rec = {
        .field = (u32)GUI_FX_NONE,
        .ops   = GUI_OP_SELF,
        .tex   = res_atlas_idx() | GUI_TEX_MODE( GUI_TEX_COVERAGE ),
    };
    rhi()->buffer_write( s_render.prim_buf, &overlay_rec, (u32)GUI_PRIM_BYTES,
                         ( (u32)GUI_PRIM_OVERLAY_ORIGIN + prim_region ) * (u32)GUI_PRIM_BYTES );

    push.prim_buf  = s_render.prim_buf_idx;
    push.prim_base = (u32)GUI_PRIM_OVERLAY_ORIGIN + prim_region;

    /* The overlay builds its own quads and never names a palette entry, a glyph or a text atlas,
       but the block must be fully initialized -- Vulkan leaves an unwritten push constant
       undefined.  Real values rather than zeros, so a stray read resolves in-bounds. */
    push.pal_base  = render_pal_base( frame );
    push.glyph_buf = s_render.glyph_buf_idx;
    push.tex_cov   = res_atlas_idx();
    push.tex_sdf   = res_sdf_idx();

    rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

    rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){
        .vertex_count   = qc * 6u,
        .instance_count = 1,
        .first_vertex   = 0,
        .first_instance = 0,
    } );

    rhi()->cmd_end_rendering( cmd );
}

/*==============================================================================================
    Public layer control
==============================================================================================*/

void gui_debug_set_layers( u32 layers ) { s_dbg.layers = layers; }
u32  gui_debug_get_layers( void )       { return s_dbg.layers; }

#else  /* !GUI_DEBUG_OVERLAY */

void gui_debug_set_layers( u32 layers ) { (void)layers; }
u32  gui_debug_get_layers( void )       { return 0u; }

/* Registry is compiled out with the rest of the overlay -- the state panel still links and
   falls back to hex ids. */
const char* gui_debug_name( gui_id_t id ) { (void)id; return NULL; }

#endif /* GUI_DEBUG_OVERLAY */

// clang-format on
/*============================================================================================*/
