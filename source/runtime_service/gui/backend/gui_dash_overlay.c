/*==============================================================================================

    runtime_service/gui/backend/gui_dash_overlay.c -- Pipeline dashboard content (backend half).

    The render half of the pipeline diagnostic dashboard (see gui_dashboard.c for the window
    shell).  Captures a coherent snapshot of the pipeline at two defined points -- the end of
    cache_build_frame (slot table, dispatch order, tess counters, volatile registry, emit
    counters, stats) and the end of each surface's gui_render_flush (frame index, upload spans)
    -- then at flush time expands the diagnostic geometry (memory-map bars, batch rows, glyph
    labels) from that snapshot into its OWN vertex/index buffers, drawn in a LOAD pass scissored
    to the canvas rects the shell registered.

    Deliberately NOT intertwined with the pipeline it visualizes (the debug-overlay rule):
      - its own scratch verts/idx and its own GPU vb/ib -- it never writes a byte into s_draw,
        s_tess, or a viewport's buffers, so opening the dashboard cannot perturb the data shown;
      - content regenerates from the snapshot every flush, so the display stays live even on
        retained/idle frames where the widget emit is skipped -- you can watch the retained
        cache actually work;
      - reuses only the shared pipeline + font atlas (fills sample the white texel, labels the
        glyph rows -- one texture, one draw per panel).

    Known v1 limitation: content draws after the surface's whole normal pipeline, so a window
    overlapping ON TOP of the dashboard is painted over inside the canvas rects.  Clicking the
    dashboard raises it; the intended workflow is tearing it off into its own viewport.

    Included by gui_backend.c LAST -- after gui_render.c and gui_debug_overlay.c -- so every
    pipeline static it reads (s_draw, s_tess, s_slots/s_dispatch/s_cache/s_stats, s_volatile,
    s_render, render_ortho, the font_* accessors, gui_debug_name) is in scope.  Compiled out
    unless GUI_PIPELINE_DASHBOARD (gui_backend.h); the capture hooks compile to (void)0 then.

==============================================================================================*/
// clang-format off

#ifdef GUI_PIPELINE_DASHBOARD

#include <stdarg.h>   /* dash_textf */

/*==============================================================================================
    Dashboard caps
==============================================================================================*/

/* Scratch geometry caps for one surface flush.  4096 quads: labels dominate (one quad per
   glyph); the panel expanders cap their row counts so truncation degrades the display, never
   corrupts it.  u16 indices are safe up to 65535 verts. */
#define GUI_DASH_MAX_VERTS  16384
#define GUI_DASH_MAX_IDX    24576

/* GPU buffer regions: one per viewport per frame-in-flight, same scheme as the debug overlay. */
#define GUI_DASH_VB_REGION_BYTES ( GUI_DASH_MAX_VERTS * sizeof( gui_draw_vert_t ) )
#define GUI_DASH_IB_REGION_BYTES ( GUI_DASH_MAX_IDX   * sizeof( u16 ) )

/* Hover hit rects recorded while expanding (absolute pixels, rebuilt every flush). */
#define GUI_DASH_MAX_HITS   320

/*==============================================================================================
    Dashboard colors
==============================================================================================*/

#define DASH_COL_TEXT       GUI_COLOR( 0xD8, 0xD8, 0xD8, 0xFF )
#define DASH_COL_TEXT_DIM   GUI_COLOR( 0x90, 0x90, 0x90, 0xFF )
#define DASH_COL_BG         GUI_COLOR( 0x14, 0x14, 0x18, 0xFF )
#define DASH_COL_CHECKER    GUI_COLOR( 0x20, 0x20, 0x26, 0xFF )
#define DASH_COL_HWM        GUI_COLOR( 0xF0, 0xE0, 0x40, 0xE0 )
#define DASH_COL_OVERFLOW   GUI_COLOR( 0xEE, 0x40, 0x30, 0xFF )
#define DASH_COL_CHANGED    GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xD0 )
#define DASH_COL_SELF       GUI_COLOR( 0x50, 0xE0, 0xF0, 0xFF )
#define DASH_COL_VOLATILE   GUI_COLOR( 0xFF, 0xFF, 0xFF, 0x60 )
#define DASH_COL_FIF_ACTIVE GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
#define DASH_COL_FIF_IDLE   GUI_COLOR( 0x40, 0x40, 0x48, 0xFF )
#define DASH_COL_SPAN_VERT  GUI_COLOR( 0x66, 0xBB, 0xEE, 0xFF )
#define DASH_COL_SPAN_IDX   GUI_COLOR( 0xBB, 0x88, 0xEE, 0xFF )
#define DASH_COL_OK         GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
#define DASH_COL_WARN       GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
#define DASH_COL_BAD        GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF )
#define DASH_COL_CUT_TEX    GUI_COLOR( 0xF0, 0xC0, 0x40, 0xFF )
#define DASH_COL_CUT_CLIP   GUI_COLOR( 0xE0, 0x50, 0xE0, 0xFF )
#define DASH_COL_CUT_FORCE  GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xC0 )

/* Stable per-window palette -- a window keeps its color across frames (indexed by id hash). */
static const u32 s_dash_palette[] = {
    GUI_COLOR( 0x4E, 0x9D, 0xE0, 0xC8 ),  GUI_COLOR( 0xE0, 0x8A, 0x3C, 0xC8 ),
    GUI_COLOR( 0x5E, 0xC2, 0x6A, 0xC8 ),  GUI_COLOR( 0xD9, 0x5C, 0x5C, 0xC8 ),
    GUI_COLOR( 0xA8, 0x7A, 0xE8, 0xC8 ),  GUI_COLOR( 0x8A, 0x6E, 0x5A, 0xC8 ),
    GUI_COLOR( 0xE2, 0x8A, 0xC8, 0xC8 ),  GUI_COLOR( 0x9A, 0xA0, 0xA8, 0xC8 ),
    GUI_COLOR( 0xC8, 0xC8, 0x50, 0xC8 ),  GUI_COLOR( 0x50, 0xC8, 0xC8, 0xC8 ),
};
#define DASH_PALETTE_N ( sizeof( s_dash_palette ) / sizeof( s_dash_palette[ 0 ] ) )

static u32
dash_win_color( gui_id_t win )
{
    return s_dash_palette[ ( win * 2654435761u >> 16 ) % DASH_PALETTE_N ];
}

/*==============================================================================================
    Pipeline snapshot -- copied at the two capture points so the display is coherent (taken at a
    defined pipeline moment, not mid-mutation) and freezable.  ~44 KB of static memory; a debug
    tool's budget.
==============================================================================================*/

typedef struct                       /* win_geo_slot_t + this frame's diff verdict */
{
    gui_id_t win;
    u32      z, vp;
    u32      vert_base, vert_count, vert_alloc;
    u32      idx_base,  idx_count,  idx_alloc;
    u32      cmd_base,  cmd_count;
    u32      tess_gen;
    bool     valid, changed;

} dash_slot_t;

typedef struct                       /* gui_gpu_cmd_t + its parallel arrays, flattened */
{
    u32        elem_count, tex_idx, vp, vbase, ibase;
    gui_rect_t clip;

} dash_cmd_t;

typedef struct                       /* gui_volatile_slot_t, display fields only */
{
    gui_id_t id, win;
    u32      tess_gen;
    u32      lvert_base, vert_count, vert_alloc;
    u32      lidx_base,  idx_count,  idx_alloc;
    u32      cmd_count,  cmd_alloc;
    bool     active, hidden;

} dash_vol_t;

typedef struct                       /* one surface's FLUSH capture */
{
    bool live;
    u32  frame_index;
    u32  vtx_lo, vtx_hi, idx_lo, idx_hi;             /* lo >= hi means nothing uploaded */
    u32  up_bytes, up_batches, draw_calls;

} dash_surf_t;

typedef struct
{
    u32  serial;                                     /* bumped per build capture; stale when frozen */

    /* BUILD capture -- end of cache_build_frame. */
    dash_slot_t slots[ RENDER_MAX_WIN ];     u32 slot_count;
    u8          dispatch[ RENDER_MAX_WIN ];  u32 dispatch_count;   /* slot indices, z-sorted */
    dash_cmd_t  cmds[ GUI_MAX_CMDS ];        u32 cmd_count;
    dash_vol_t  vols[ GUI_MAX_VOLATILE ];    u32 vol_count;

    u32  tess_verts, tess_idx, tess_cmds, vert_hwm, idx_hwm;
    bool overflow_ever;
    u32  emit_cmds, emit_segs, emit_pts, emit_text, emit_clips;
    u32  diff_unchanged;  bool any_changed;
    u32  tess_gen_next;

    gui_render_stats_t stats;                        /* last published frame (one-frame lag) */
    u32  draw_call_hwm;

    /* FLUSH capture -- end of gui_render_flush, per surface. */
    dash_surf_t surf[ GUI_MAX_VIEWPORTS ];

} dash_snapshot_t;

/*==============================================================================================
    Hover hit rects -- recorded while expanding, resolved by gui_dash_probe (one-frame lag).
==============================================================================================*/

typedef enum
{
    DASH_HIT_SLOT_VB = 0,   /* a = slot index                    */
    DASH_HIT_SLOT_IB,       /* a = slot index                    */
    DASH_HIT_VOL_VB,        /* a = volatile index                */
    DASH_HIT_BATCH_ROW,     /* a = dispatch index                */
    DASH_HIT_CMD,           /* a = dispatch index, b = local cmd */
    DASH_HIT_FIF,           /* a = viewport index                */
    DASH_HIT_EMIT_BAR,      /* a = bar index                     */
    DASH_HIT_VOL_ROW,       /* a = volatile index                */

} dash_hit_kind_t;

typedef struct
{
    gui_rect_t r;
    u8         kind, a, b;

} dash_hit_t;

/*==============================================================================================
    Dashboard state
==============================================================================================*/

typedef struct
{
    gui_rect_t r;      /* full canvas rect (layout space)                          */
    gui_rect_t clip;   /* r intersected with the ambient clip at registration      */
    u32        vp;
    bool       live;

} dash_canvas_t;

static struct
{
    bool            freeze;                          /* captures halted; display holds the snapshot */
    dash_snapshot_t snap;

    dash_canvas_t   canvas[ GUI_DASH_PANEL_COUNT ];

    /* Private cursor tooltip -- armed by the shell while it owns the hover, drawn LAST at flush
       through these same buffers so panel content can never paint over it (a normal-pipeline
       tooltip window would lose that race: dash content flushes after the whole UI).  Keeping
       the tooltip out of the normal pipeline also keeps the slot arena still while hovering --
       a real tooltip window re-tessellates every mouse move and made the memory map flicker. */
    bool            tip_armed;
    f32             tip_x, tip_y;

    dash_hit_t      hits[ GUI_DASH_MAX_HITS ];
    u32             hit_count;

    /* Scratch geometry: filled per-surface at flush time, uploaded once, discarded. */
    gui_draw_vert_t scratch_verts[ GUI_DASH_MAX_VERTS ];
    u16             scratch_idx  [ GUI_DASH_MAX_IDX   ];
    u32             vc, ic;
    bool            overflow;

    f32             wu, wv;                          /* white texel UV, cached per flush */

    rhi_buffer_t    vb, ib;                          /* own GPU buffers -- never the pipeline's */

} s_dash;

/*==============================================================================================
    Capture hooks -- called from the pipeline files via the DASH_* macros (gui_backend.h).
==============================================================================================*/

static bool
dash_any_canvas( void )
{
    for ( u32 p = 0; p < GUI_DASH_PANEL_COUNT; ++p )
        if ( s_dash.canvas[ p ].live )
            return true;
    return false;
}

/* End of cache_build_frame: every slot is placed, dispatch is z-sorted, stats are accumulated. */
void
dash_capture_build( void )
{
    if ( s_dash.freeze || !dash_any_canvas() )
        return;

    dash_snapshot_t* sn = &s_dash.snap;
    sn->serial++;

    sn->slot_count = s_slot_count < RENDER_MAX_WIN ? s_slot_count : RENDER_MAX_WIN;
    for ( u32 i = 0; i < sn->slot_count; ++i )
    {
        const win_geo_slot_t* sl = &s_slots[ i ];
        dash_slot_t*          d  = &sn->slots[ i ];
        d->win        = sl->win;
        d->z          = sl->z;          d->vp        = sl->vp;
        d->vert_base  = sl->vert_base;  d->vert_count = sl->vert_count;  d->vert_alloc = sl->vert_alloc;
        d->idx_base   = sl->idx_base;   d->idx_count  = sl->idx_count;   d->idx_alloc  = sl->idx_alloc;
        d->cmd_base   = sl->cmd_base;   d->cmd_count  = sl->cmd_count;
        d->tess_gen   = sl->tess_gen;   d->valid      = sl->valid;

        d->changed = false;
        for ( u32 c = 0; c < s_cache.cur_n; ++c )
            if ( s_cache.cur[ c ].win == sl->win ) { d->changed = s_cache.cur[ c ].changed; break; }
    }

    sn->dispatch_count = s_dispatch_count < RENDER_MAX_WIN ? s_dispatch_count : RENDER_MAX_WIN;
    for ( u32 d = 0; d < sn->dispatch_count; ++d )
        sn->dispatch[ d ] = (u8)( s_dispatch[ d ] - s_slots );

    sn->cmd_count = s_tess.cmd_count < GUI_MAX_CMDS ? s_tess.cmd_count : GUI_MAX_CMDS;
    for ( u32 c = 0; c < sn->cmd_count; ++c )
    {
        sn->cmds[ c ].elem_count = s_tess.cmds     [ c ].elem_count;
        sn->cmds[ c ].tex_idx    = s_tess.cmds     [ c ].tex_idx;
        sn->cmds[ c ].clip       = s_tess.cmds     [ c ].clip_rect;
        sn->cmds[ c ].vp         = s_tess.cmd_vp   [ c ];
        sn->cmds[ c ].vbase      = s_tess.cmd_vbase[ c ];
        sn->cmds[ c ].ibase      = s_tess.cmd_ibase[ c ];
    }

    sn->vol_count = s_volatile_count < GUI_MAX_VOLATILE ? s_volatile_count : GUI_MAX_VOLATILE;
    for ( u32 v = 0; v < sn->vol_count; ++v )
    {
        const gui_volatile_slot_t* vs = &s_volatile[ v ];
        dash_vol_t*                d  = &sn->vols[ v ];
        d->id         = vs->id;               d->win        = vs->win;
        d->tess_gen   = vs->tess_gen;
        d->lvert_base = vs->local_vert_base;  d->vert_count = vs->vert_count;  d->vert_alloc = vs->vert_alloc;
        d->lidx_base  = vs->local_idx_base;   d->idx_count  = vs->idx_count;   d->idx_alloc  = vs->idx_alloc;
        d->cmd_count  = vs->cmd_count;        d->cmd_alloc  = vs->cmd_alloc;
        d->active     = vs->active;           d->hidden     = vs->hidden;
    }

    sn->tess_verts    = s_tess.vert_count;   sn->tess_idx = s_tess.idx_count;
    sn->tess_cmds     = s_tess.cmd_count;
    sn->vert_hwm      = s_tess.vert_hwm;     sn->idx_hwm  = s_tess.idx_hwm;
    sn->overflow_ever = s_tess.overflow_ever;

    sn->emit_cmds     = s_draw.cmd_count;    sn->emit_segs  = s_draw.seg_count;
    sn->emit_pts      = s_draw.pt_count;     sn->emit_text  = s_draw.text_pool_used;
    sn->emit_clips    = s_draw.clip_table_n;

    sn->diff_unchanged = s_cache.unchanged;  sn->any_changed = s_cache.any_changed;
    sn->tess_gen_next  = s_tess_gen_next;

    sn->stats          = s_stats.published;
    sn->draw_call_hwm  = s_stats.draw_call_hwm;
}

/* End of one surface's gui_render_flush: what physically hit the GPU for that surface.  Runs
   every frame (real or idle) since cached geometry is replayed regardless; a surface flushed
   before the dashboard's own shows this frame's values, one flushed after shows last frame's --
   the standard one-frame self-measurement lag. */
void
dash_capture_flush( u32 vp, u32 frame, u32 vtx_lo, u32 vtx_hi, u32 idx_lo, u32 idx_hi,
                    u32 bytes, u32 batches, u32 draws )
{
    if ( s_dash.freeze || vp >= GUI_MAX_VIEWPORTS || !dash_any_canvas() )
        return;
    s_dash.snap.surf[ vp ] = ( dash_surf_t ){
        .live = true, .frame_index = frame,
        .vtx_lo = vtx_lo, .vtx_hi = vtx_hi, .idx_lo = idx_lo, .idx_hi = idx_hi,
        .up_bytes = bytes, .up_batches = batches, .draw_calls = draws,
    };
}

/*==============================================================================================
    UI-shell seam (called from gui_dashboard.c, UI unit)
==============================================================================================*/

void
gui_dash_ui_begin( void )
{
    for ( u32 p = 0; p < GUI_DASH_PANEL_COUNT; ++p )
        s_dash.canvas[ p ].live = false;
    s_dash.tip_armed = false;
}

/* Arm the private cursor tooltip for this emit.  The backend probes at flush time, so the
   tooltip and the bars always come from the same hit table -- no shell-side formatting. */
void
gui_dash_tooltip( f32 mx, f32 my )
{
    s_dash.tip_armed = true;
    s_dash.tip_x     = mx;
    s_dash.tip_y     = my;
}

void
gui_dash_canvas( u32 panel, gui_rect_t r, u32 vp )
{
    if ( panel >= GUI_DASH_PANEL_COUNT || vp >= GUI_MAX_VIEWPORTS )
        return;
    /* Intersect with the ambient emit clip so a scrolled-out panel draws nothing outside the
       window body -- the scissor at flush time is this stored clip, not the raw canvas rect. */
    gui_rect_t clip = s_draw.clip_table[ s_draw.cur_clip_idx ];
    s_dash.canvas[ panel ] = ( dash_canvas_t ){ .r = r, .clip = rect_intersect( r, clip ),
                                                .vp = vp, .live = true };
}

void gui_dash_set_freeze( bool on ) { s_dash.freeze = on; }
bool gui_dash_frozen    ( void )    { return s_dash.freeze; }
u32  gui_dash_serial    ( void )    { return s_dash.snap.serial; }

/*==============================================================================================
    Lifecycle
==============================================================================================*/

bool
gui_dash_init( void )
{
    s_dash.vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS * GUI_DASH_VB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_dash_vb",
    } );
    if ( !rhi_handle_valid( s_dash.vb ) ) return false;

    s_dash.ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS * GUI_DASH_IB_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_INDEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_dash_ib",
    } );
    if ( !rhi_handle_valid( s_dash.ib ) )
    {
        rhi()->buffer_destroy( s_dash.vb );
        s_dash.vb = ( rhi_buffer_t ){ 0 };
        return false;
    }
    return true;
}

void
gui_dash_shutdown( void )
{
    if ( rhi_handle_valid( s_dash.ib ) ) rhi()->buffer_destroy( s_dash.ib );
    if ( rhi_handle_valid( s_dash.vb ) ) rhi()->buffer_destroy( s_dash.vb );
    memset( &s_dash, 0, sizeof( s_dash ) );
}

/*==============================================================================================
    Scratch emitters -- all writing gui_draw_vert_t + u16 into s_dash.scratch_*, capacity-checked
    (overflow truncates the display, never corrupts).
==============================================================================================*/

static bool
dash_room( u32 nv, u32 ni )
{
    if ( s_dash.vc + nv > GUI_DASH_MAX_VERTS || s_dash.ic + ni > GUI_DASH_MAX_IDX )
    {
        s_dash.overflow = true;
        return false;
    }
    return true;
}

/* Generic 4-corner quad (a..d clockwise), sampling one UV point -- fills and hatch stripes. */
static void
dash_quad4( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, f32 dx, f32 dy, u32 abgr )
{
    if ( !dash_room( 4, 6 ) ) return;
    u16 base = (u16)s_dash.vc;
    gui_draw_vert_t* v = &s_dash.scratch_verts[ s_dash.vc ];
    v[ 0 ] = ( gui_draw_vert_t ){ ax, ay, s_dash.wu, s_dash.wv, abgr };
    v[ 1 ] = ( gui_draw_vert_t ){ bx, by, s_dash.wu, s_dash.wv, abgr };
    v[ 2 ] = ( gui_draw_vert_t ){ cx, cy, s_dash.wu, s_dash.wv, abgr };
    v[ 3 ] = ( gui_draw_vert_t ){ dx, dy, s_dash.wu, s_dash.wv, abgr };
    s_dash.vc += 4;

    u16* idx = &s_dash.scratch_idx[ s_dash.ic ];
    idx[ 0 ] = base;  idx[ 1 ] = base + 1;  idx[ 2 ] = base + 2;
    idx[ 3 ] = base;  idx[ 4 ] = base + 2;  idx[ 5 ] = base + 3;
    s_dash.ic += 6;
}

static void
dash_fill( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    if ( w <= 0.0f || h <= 0.0f ) return;
    dash_quad4( x, y, x + w, y, x + w, y + h, x, y + h, abgr );
}

static void
dash_outline( gui_rect_t r, f32 t, u32 abgr )
{
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    if ( t > r.h * 0.5f ) t = r.h * 0.5f;
    dash_fill( r.x,           r.y,           r.w, t,              abgr );   /* top    */
    dash_fill( r.x,           r.y + r.h - t, r.w, t,              abgr );   /* bottom */
    dash_fill( r.x,           r.y + t,       t,   r.h - 2.0f * t, abgr );   /* left   */
    dash_fill( r.x + r.w - t, r.y + t,       t,   r.h - 2.0f * t, abgr );   /* right  */
}

/* Diagonal 45-degree hatch, exact-clipped to the rect (reserved / padding regions). */
static void
dash_hatch( gui_rect_t r, f32 pitch, u32 abgr )
{
    if ( r.w <= 1.0f || r.h <= 1.0f ) return;
    if ( pitch < 3.0f ) pitch = 3.0f;
    const f32 th = 1.0f;
    f32 y1 = r.y + r.h;
    for ( f32 s = r.x - r.h; s < r.x + r.w; s += pitch )
    {
        /* stripe p(t) = (s + t*h, y1 - t*h), t in [0,1]; clamp t so x stays inside the rect */
        f32 t0 = ( r.x - s ) / r.h;
        f32 t1 = ( r.x + r.w - th - s ) / r.h;
        t0 = saturate( t0 );
        t1 = saturate( t1 );
        if ( t1 <= t0 ) continue;
        f32 ax = s + t0 * r.h, ay = y1 - t0 * r.h;
        f32 cx = s + t1 * r.h, cy = y1 - t1 * r.h;
        dash_quad4( ax, ay, ax + th, ay, cx + th, cy, cx, cy, abgr );
    }
}

/* Alternate-cell checker (free / unclaimed space). */
static void
dash_checker( gui_rect_t r, f32 cell, u32 abgr )
{
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    u32 row = 0;
    for ( f32 y = r.y; y < r.y + r.h; y += cell, ++row )
    {
        f32 h = ( y + cell > r.y + r.h ) ? r.y + r.h - y : cell;
        u32 col_i = row & 1u;
        for ( f32 x = r.x + (f32)col_i * cell; x < r.x + r.w; x += cell * 2.0f )
        {
            f32 w = ( x + cell > r.x + r.w ) ? r.x + r.w - x : cell;
            dash_fill( x, y, w, h, abgr );
        }
    }
}

/* Dashed vertical marker (high-water lines). */
static void
dash_vline_dashed( f32 x, f32 y0, f32 y1, u32 abgr )
{
    for ( f32 y = y0; y < y1; y += 6.0f )
    {
        f32 h = ( y + 3.0f > y1 ) ? y1 - y : 3.0f;
        dash_fill( x, y, 1.0f, h, abgr );
    }
}

/* Glyph run truncated at max_x (self-fit rule: never bleed past the cell).  Returns the pen x
   after the last emitted glyph.  y is the line top, matching tess_text_n. */
static f32
dash_text( f32 x, f32 y, f32 max_x, u32 abgr, const char* str )
{
    f32 cx = x;
    for ( u32 i = 0; str[ i ]; ++i )
    {
        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( (u8)str[ i ], &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );
        if ( cx + advance > max_x )
            break;
        if ( gw > 0.0f && gh > 0.0f )
        {
            if ( !dash_room( 4, 6 ) ) return cx;
            u16 base = (u16)s_dash.vc;
            f32 gx = cx + ox, gy = y + oy;
            gui_draw_vert_t* v = &s_dash.scratch_verts[ s_dash.vc ];
            v[ 0 ] = ( gui_draw_vert_t ){ gx,      gy,      u0, v0, abgr };
            v[ 1 ] = ( gui_draw_vert_t ){ gx + gw, gy,      u1, v0, abgr };
            v[ 2 ] = ( gui_draw_vert_t ){ gx + gw, gy + gh, u1, v1, abgr };
            v[ 3 ] = ( gui_draw_vert_t ){ gx,      gy + gh, u0, v1, abgr };
            s_dash.vc += 4;
            u16* idx = &s_dash.scratch_idx[ s_dash.ic ];
            idx[ 0 ] = base;  idx[ 1 ] = base + 1;  idx[ 2 ] = base + 2;
            idx[ 3 ] = base;  idx[ 4 ] = base + 2;  idx[ 5 ] = base + 3;
            s_dash.ic += 6;
        }
        cx += advance;
    }
    return cx;
}

static f32
dash_textf( f32 x, f32 y, f32 max_x, u32 abgr, const char* fmt, ... )
{
    char buf[ 128 ];
    va_list args;
    va_start( args, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );
    return dash_text( x, y, max_x, abgr, buf );
}

/*----------------------------------------------------------------------------------------------
    Hit rects + name lookup
----------------------------------------------------------------------------------------------*/

static void
dash_hit( gui_rect_t r, u8 kind, u8 a, u8 b )
{
    if ( s_dash.hit_count >= GUI_DASH_MAX_HITS ) return;
    s_dash.hits[ s_dash.hit_count++ ] = ( dash_hit_t ){ r, kind, a, b };
}

/* id -> registered source string (Debug overlay's registry) or hex.  buf must hold >= 12. */
static const char*
dash_name( gui_id_t id, char* buf, u32 bufsz )
{
    const char* n = gui_debug_name( id );
    if ( n ) return n;
    snprintf( buf, bufsz, "%08X", id );
    return buf;
}

/*==============================================================================================
    Panel expanders -- each derives its geometry from the snapshot into the scratch buffers,
    laid out inside the registered canvas rect.
==============================================================================================*/

/* Shared memory-map body: the whole s_tess vertex (or index) arena as one horizontal bar. */
static void
dash_expand_memmap( gui_rect_t r, bool vb_axis )
{
    const dash_snapshot_t* sn  = &s_dash.snap;
    const f32              lh  = font_line_h();
    const u32              cap = vb_axis ? GUI_MAX_VERTS : GUI_MAX_IDX;

    /* Header: live totals + high-water + cap. */
    u32 used = vb_axis ? sn->tess_verts : sn->tess_idx;
    u32 hwm  = vb_axis ? sn->vert_hwm   : sn->idx_hwm;
    dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT_DIM, "%s  %u / %u   hwm %u   pad %u",
                vb_axis ? "verts" : "indices", used, cap, hwm,
                vb_axis ? (u32)SLOT_VERT_PAD : (u32)SLOT_IDX_PAD );

    gui_rect_t bar = { r.x, r.y + lh + 2.0f, r.w, r.h - lh - 4.0f };
    if ( bar.h < 12.0f ) return;

    /* Free space background: dim checker over the whole arena. */
    dash_fill( bar.x, bar.y, bar.w, bar.h, DASH_COL_BG );
    dash_checker( bar, 8.0f, DASH_COL_CHECKER );

    f32 px_per = bar.w / (f32)cap;

    for ( u32 i = 0; i < sn->slot_count; ++i )
    {
        const dash_slot_t* sl = &sn->slots[ i ];
        if ( !sl->valid ) continue;

        u32 base  = vb_axis ? sl->vert_base  : sl->idx_base;
        u32 count = vb_axis ? sl->vert_count : sl->idx_count;
        u32 alloc = vb_axis ? sl->vert_alloc : sl->idx_alloc;

        f32 x0 = bar.x + (f32)base * px_per;
        f32 xc = bar.x + (f32)( base + count ) * px_per;
        f32 xa = bar.x + (f32)( base + alloc ) * px_per;
        u32 col = dash_win_color( sl->win );

        dash_fill( x0, bar.y, xc - x0, bar.h, col );                       /* live geometry     */
        dash_fill ( xc, bar.y, xa - xc, bar.h, ( col & 0x00FFFFFFu ) | 0x30000000u );
        dash_hatch( ( gui_rect_t ){ xc, bar.y, xa - xc, bar.h }, 5.0f,     /* padded headroom   */
                    ( col & 0x00FFFFFFu ) | 0x80000000u );

        /* Volatile sub-slots: brighter inset strips across the top of their owner's extent. */
        if ( vb_axis )
        {
            for ( u32 v = 0; v < sn->vol_count; ++v )
            {
                const dash_vol_t* vo = &sn->vols[ v ];
                if ( !vo->active || vo->win != sl->win ) continue;
                f32 vx0 = bar.x + (f32)( base + vo->lvert_base ) * px_per;
                f32 vx1 = bar.x + (f32)( base + vo->lvert_base + vo->vert_alloc ) * px_per;
                gui_rect_t vr = { vx0, bar.y + 1.0f, vx1 - vx0, bar.h * 0.3f };
                dash_fill( vr.x, vr.y, vr.w, vr.h, DASH_COL_VOLATILE );
                dash_hit( vr, DASH_HIT_VOL_VB, (u8)v, 0 );
            }
        }

        /* Diff pulse / self-marker outlines. */
        gui_rect_t seg = { x0, bar.y, xa - x0, bar.h };
        if ( sl->win == g_gui_dash_window_id )
            dash_outline( seg, 1.0f, DASH_COL_SELF );
        else if ( sl->changed )
            dash_outline( seg, 1.0f, DASH_COL_CHANGED );

        /* Window name inside the segment when it is wide enough to read. */
        if ( xa - x0 >= 40.0f )
        {
            char nb[ 12 ];
            dash_text( x0 + 3.0f, bar.y + ( bar.h - font_char_h() ) * 0.5f, xa - 2.0f,
                       DASH_COL_TEXT, dash_name( sl->win, nb, sizeof( nb ) ) );
        }

        dash_hit( seg, vb_axis ? DASH_HIT_SLOT_VB : DASH_HIT_SLOT_IB, (u8)i, 0 );
    }

    /* High-water marker + lifetime overflow flag. */
    dash_vline_dashed( bar.x + (f32)hwm * px_per, bar.y, bar.y + bar.h, DASH_COL_HWM );
    if ( sn->overflow_ever )
        dash_fill( bar.x + bar.w - 8.0f, bar.y, 8.0f, 8.0f, DASH_COL_OVERFLOW );
}

/* Frames-in-flight: per live surface, one box per in-flight region; the region written last is
   highlighted with its upload spans drawn inside. */
static void
dash_expand_fif( gui_rect_t r )
{
    const dash_snapshot_t* sn    = &s_dash.snap;
    const f32              row_h = 22.0f;
    f32                    y     = r.y + 2.0f;

    for ( u32 vp = 0; vp < GUI_MAX_VIEWPORTS; ++vp )
    {
        if ( !sn->surf[ vp ].live ) continue;
        if ( y + row_h > r.y + r.h ) break;

        const dash_surf_t* sf = &sn->surf[ vp ];

        dash_textf( r.x + 2.0f, y + 3.0f, r.x + 40.0f, DASH_COL_TEXT, "vp%u", vp );

        f32 box_x = r.x + 40.0f;
        f32 box_w = ( r.w * 0.55f - 40.0f ) / (f32)RHI_MAX_FRAMES_IN_FLIGHT;
        for ( u32 f = 0; f < RHI_MAX_FRAMES_IN_FLIGHT; ++f )
        {
            gui_rect_t box = { box_x + (f32)f * ( box_w + 4.0f ), y, box_w, row_h - 4.0f };
            bool       act = ( f == sf->frame_index );
            dash_fill( box.x, box.y, box.w, box.h, DASH_COL_BG );
            dash_outline( box, 1.0f, act ? DASH_COL_FIF_ACTIVE : DASH_COL_FIF_IDLE );
            if ( act && sf->vtx_hi > sf->vtx_lo )
            {
                f32 half = box.h * 0.5f - 2.0f;
                f32 vx0  = box.x + 1.0f + ( box.w - 2.0f ) * (f32)sf->vtx_lo / (f32)GUI_MAX_VERTS;
                f32 vx1  = box.x + 1.0f + ( box.w - 2.0f ) * (f32)sf->vtx_hi / (f32)GUI_MAX_VERTS;
                dash_fill( vx0, box.y + 2.0f, vx1 - vx0, half, DASH_COL_SPAN_VERT );
                f32 ix0  = box.x + 1.0f + ( box.w - 2.0f ) * (f32)sf->idx_lo / (f32)GUI_MAX_IDX;
                f32 ix1  = box.x + 1.0f + ( box.w - 2.0f ) * (f32)sf->idx_hi / (f32)GUI_MAX_IDX;
                dash_fill( ix0, box.y + 2.0f + half, ix1 - ix0, half, DASH_COL_SPAN_IDX );
            }
        }

        dash_textf( r.x + r.w * 0.55f + 8.0f, y + 3.0f, r.x + r.w, DASH_COL_TEXT_DIM,
                    "up %u B  %u wr  %u draw", sf->up_bytes, sf->up_batches, sf->draw_calls );

        dash_hit( ( gui_rect_t ){ r.x, y, r.w, row_h }, DASH_HIT_FIF, (u8)vp, 0 );
        y += row_h;
    }

    if ( y == r.y + 2.0f )
        dash_text( r.x + 2.0f, y, r.x + r.w, DASH_COL_TEXT_DIM, "no surfaces flushed yet" );
}

/* Batch inspector: dispatch-order rows (back to front), each slot's cached GPU commands as
   bars, with a colored tick at each batch CUT (tex change / clip change / forced). */
static void
dash_expand_batch( gui_rect_t r )
{
    const dash_snapshot_t* sn    = &s_dash.snap;
    const f32              row_h = font_line_h() + 3.0f;
    f32                    y     = r.y + 2.0f;
    const f32              bars_x = r.x + 210.0f;

    for ( u32 d = 0; d < sn->dispatch_count; ++d )
    {
        if ( y + row_h > r.y + r.h )
        {
            dash_textf( r.x + 2.0f, y - row_h, r.x + r.w, DASH_COL_TEXT_DIM,
                        "+%u more", sn->dispatch_count - d );
            break;
        }

        const dash_slot_t* sl = &sn->slots[ sn->dispatch[ d ] ];
        u32                col = dash_win_color( sl->win );
        char               nb[ 12 ];

        dash_fill( r.x + 2.0f, y + 3.0f, 8.0f, 8.0f, col );
        dash_text( r.x + 14.0f, y, r.x + 120.0f, DASH_COL_TEXT,
                   dash_name( sl->win, nb, sizeof( nb ) ) );
        dash_textf( r.x + 124.0f, y, bars_x - 4.0f, DASH_COL_TEXT_DIM, "z%-2u v%u g%u",
                    sl->z, sl->vp, sl->tess_gen );

        /* One bar per cached GPU command: width ~ log2(elem_count), colored by texture. */
        f32 bx = bars_x;
        for ( u32 k = 0; k < sl->cmd_count && sl->cmd_base + k < sn->cmd_count; ++k )
        {
            const dash_cmd_t* dc = &sn->cmds[ sl->cmd_base + k ];
            if ( dc->vp == GUI_VP_INVALID ) continue;   /* dormant volatile pad -- not drawn */

            f32 bw = 4.0f;
            for ( u32 e = dc->elem_count; e > 1; e >>= 1 ) bw += 3.0f;   /* ~3px per log2 step */
            if ( bw > 44.0f ) bw = 44.0f;
            if ( bx + bw > r.x + r.w - 4.0f ) break;

            if ( k > 0 )
            {
                /* Why this command split from the previous one -- the batch-cut cause. */
                const dash_cmd_t* pc  = &sn->cmds[ sl->cmd_base + k - 1 ];
                u32               cut = ( dc->tex_idx != pc->tex_idx ) ? DASH_COL_CUT_TEX
                                      : ( dc->clip.x != pc->clip.x || dc->clip.y != pc->clip.y
                                       || dc->clip.w != pc->clip.w || dc->clip.h != pc->clip.h )
                                                                       ? DASH_COL_CUT_CLIP
                                                                       : DASH_COL_CUT_FORCE;
                dash_fill( bx - 2.0f, y + 1.0f, 1.0f, row_h - 3.0f, cut );
            }

            u32 bcol = ( dc->tex_idx == font_atlas_idx() )
                     ? ( col | 0xFF000000u )
                     : GUI_COLOR( 0x50, 0xC0, 0xB0, 0xFF );   /* icon/other atlas */
            gui_rect_t bar = { bx, y + 2.0f, bw, row_h - 5.0f };
            dash_fill( bar.x, bar.y, bar.w, bar.h, bcol );
            dash_hit( bar, DASH_HIT_CMD, sn->dispatch[ d ], (u8)k );
            bx += bw + 3.0f;
        }

        dash_hit( ( gui_rect_t ){ r.x, y, bars_x - r.x - 4.0f, row_h }, DASH_HIT_BATCH_ROW,
                  sn->dispatch[ d ], 0 );
        y += row_h;
    }
}

/* EMIT/BUILD usage bars vs caps, graded green -> amber -> red, with hwm ticks where tracked. */
static void
dash_expand_emit( gui_rect_t r )
{
    const dash_snapshot_t* sn = &s_dash.snap;

    struct { const char* name; u32 used, cap, hwm; } rows[] = {
        { "cmds",  sn->emit_cmds,  GUI_MAX_CMDS,       0            },
        { "segs",  sn->emit_segs,  GUI_MAX_SEGS,       0            },
        { "pts",   sn->emit_pts,   GUI_MAX_PATH_PTS,   0            },
        { "text",  sn->emit_text,  GUI_MAX_TEXT_POOL,  0            },
        { "clips", sn->emit_clips, GUI_MAX_CLIP_RECTS, 0            },
        { "verts", sn->tess_verts, GUI_MAX_VERTS,      sn->vert_hwm },
        { "idx",   sn->tess_idx,   GUI_MAX_IDX,        sn->idx_hwm  },
        { "gpu",   sn->tess_cmds,  GUI_MAX_CMDS,       0            },
    };
    const u32 n     = sizeof( rows ) / sizeof( rows[ 0 ] );
    const f32 row_h = ( r.h - 4.0f ) / (f32)n;
    const f32 bar_x = r.x + 84.0f;   /* label column + a clear gap before the bars */
    const f32 bar_w = r.w - 84.0f - 110.0f;

    for ( u32 i = 0; i < n; ++i )
    {
        f32 y = r.y + 2.0f + (f32)i * row_h;
        f32 frac = rows[ i ].cap ? (f32)rows[ i ].used / (f32)rows[ i ].cap : 0.0f;
        u32 col  = frac > 0.90f ? DASH_COL_BAD : frac > 0.75f ? DASH_COL_WARN : DASH_COL_OK;

        dash_text( r.x + 2.0f, y, bar_x - 10.0f, DASH_COL_TEXT_DIM, rows[ i ].name );
        dash_fill( bar_x, y + 2.0f, bar_w, row_h - 4.0f, DASH_COL_BG );
        dash_fill( bar_x, y + 2.0f, bar_w * frac, row_h - 4.0f, col );
        if ( rows[ i ].hwm )
            dash_vline_dashed( bar_x + bar_w * (f32)rows[ i ].hwm / (f32)rows[ i ].cap,
                               y + 1.0f, y + row_h - 1.0f, DASH_COL_HWM );
        dash_textf( bar_x + bar_w + 6.0f, y, r.x + r.w, DASH_COL_TEXT_DIM, "%u / %u",
                    rows[ i ].used, rows[ i ].cap );
        dash_hit( ( gui_rect_t ){ r.x, y, r.w, row_h }, DASH_HIT_EMIT_BAR, (u8)i, 0 );
    }
}

/* Volatile registry: one row per captured sub-slot with live-vs-reserved mini bars. */
static void
dash_expand_volatile( gui_rect_t r )
{
    const dash_snapshot_t* sn    = &s_dash.snap;
    const f32              row_h = font_line_h() + 2.0f;
    f32                    y     = r.y + 2.0f;

    if ( sn->vol_count == 0 )
    {
        dash_text( r.x + 2.0f, y, r.x + r.w, DASH_COL_TEXT_DIM, "no volatile widgets registered" );
        return;
    }

    for ( u32 v = 0; v < sn->vol_count; ++v )
    {
        if ( y + row_h > r.y + r.h ) break;
        const dash_vol_t* vo = &sn->vols[ v ];
        char nb[ 12 ], wb[ 12 ];

        /* Generation check against the owner slot: a mismatch means patches are not landing. */
        u32  slot_gen = 0;
        bool have_own = false;
        for ( u32 i = 0; i < sn->slot_count; ++i )
            if ( sn->slots[ i ].win == vo->win ) { slot_gen = sn->slots[ i ].tess_gen; have_own = true; break; }
        bool stale = have_own && vo->active && vo->tess_gen != slot_gen;

        dash_fill( r.x + 2.0f, y + 4.0f, 6.0f, 6.0f,
                   vo->active ? ( vo->hidden ? DASH_COL_WARN : DASH_COL_OK ) : DASH_COL_FIF_IDLE );
        dash_text ( r.x + 14.0f,  y, r.x + 116.0f, DASH_COL_TEXT, dash_name( vo->id, nb, sizeof( nb ) ) );
        dash_text ( r.x + 120.0f, y, r.x + 210.0f, DASH_COL_TEXT_DIM, dash_name( vo->win, wb, sizeof( wb ) ) );

        f32 bx = r.x + 216.0f, bw = 70.0f, bh = row_h - 6.0f;
        f32 vfrac = vo->vert_alloc ? (f32)vo->vert_count / (f32)vo->vert_alloc : 0.0f;
        f32 ifrac = vo->idx_alloc  ? (f32)vo->idx_count  / (f32)vo->idx_alloc  : 0.0f;
        dash_fill( bx, y + 3.0f, bw, bh, DASH_COL_BG );
        dash_fill( bx, y + 3.0f, bw * vfrac, bh, DASH_COL_SPAN_VERT );
        dash_fill( bx + bw + 6.0f, y + 3.0f, bw, bh, DASH_COL_BG );
        dash_fill( bx + bw + 6.0f, y + 3.0f, bw * ifrac, bh, DASH_COL_SPAN_IDX );

        dash_textf( bx + 2.0f * bw + 14.0f, y, r.x + r.w, stale ? DASH_COL_BAD : DASH_COL_TEXT_DIM,
                    stale ? "gen %u STALE" : "gen %u", vo->tess_gen );

        dash_hit( ( gui_rect_t ){ r.x, y, r.w, row_h }, DASH_HIT_VOL_ROW, (u8)v, 0 );
        y += row_h;
    }
}

/* Frame stats strip (last published frame + capture state). */
static void
dash_expand_stats( gui_rect_t r )
{
    const dash_snapshot_t*    sn = &s_dash.snap;
    const gui_render_stats_t* st = &sn->stats;
    const f32                 lh = font_line_h();

    dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT,
                "draws %u (hwm %u)   upload %u B / %u wr   vol patched %u",
                st->draw_calls, sn->draw_call_hwm, st->upload_bytes, st->upload_batches,
                st->volatile_patched );
    dash_textf( r.x + 2.0f, r.y + lh, r.x + r.w, DASH_COL_TEXT,
                "wins ret %u/%u   verts ret %u/%u   tris ret %u/%u",
                st->win_retained, st->win_total, st->vert_retained, st->vert_count,
                st->tri_retained, st->tri_count );
    dash_textf( r.x + 2.0f, r.y + 2.0f * lh, r.x + r.w,
                s_dash.freeze ? DASH_COL_WARN : DASH_COL_TEXT_DIM,
                "capture #%u%s   any_changed %u   unchanged %u   gen %u%s",
                sn->serial, s_dash.freeze ? "  FROZEN" : "", (u32)sn->any_changed,
                sn->diff_unchanged, sn->tess_gen_next,
                sn->overflow_ever ? "   OVERFLOWED" : "" );
}

/*==============================================================================================
    Hover probe -- resolve a mouse position against the hit rects recorded at the last flush and
    format the answer from the same snapshot the bars were drawn from, so they always agree.
==============================================================================================*/

static bool
dash_pt_in( gui_rect_t r, f32 x, f32 y )
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

bool
gui_dash_probe( f32 mx, f32 my, gui_dash_probe_t* out )
{
    const dash_snapshot_t* sn = &s_dash.snap;
    out->count = 0;

    /* Walk backwards: later hits were drawn later (volatile strips over slot segments). */
    for ( u32 i = s_dash.hit_count; i-- > 0; )
    {
        const dash_hit_t* h = &s_dash.hits[ i ];
        if ( !dash_pt_in( h->r, mx, my ) ) continue;

        char nb[ 12 ], wb[ 12 ];
        #define DASH_LINE( ... ) \
            do { if ( out->count < GUI_DASH_PROBE_LINES ) \
                snprintf( out->line[ out->count++ ], sizeof( out->line[ 0 ] ), __VA_ARGS__ ); } while ( 0 )

        switch ( h->kind )
        {
        case DASH_HIT_SLOT_VB:
        case DASH_HIT_SLOT_IB:
        {
            if ( h->a >= sn->slot_count ) return false;
            const dash_slot_t* sl = &sn->slots[ h->a ];
            DASH_LINE( "window  %s%s", dash_name( sl->win, nb, sizeof( nb ) ),
                       sl->win == g_gui_dash_window_id ? "  (this dashboard)" : "" );
            DASH_LINE( "verts   [%u..%u)  alloc %u  (pad %u)", sl->vert_base,
                       sl->vert_base + sl->vert_count, sl->vert_alloc, sl->vert_alloc - sl->vert_count );
            DASH_LINE( "indices [%u..%u)  alloc %u  (pad %u)", sl->idx_base,
                       sl->idx_base + sl->idx_count, sl->idx_alloc, sl->idx_alloc - sl->idx_count );
            DASH_LINE( "cmds    [%u..%u)   z %u  vp %u  gen %u", sl->cmd_base,
                       sl->cmd_base + sl->cmd_count, sl->z, sl->vp, sl->tess_gen );
            DASH_LINE( "%s this frame", sl->changed ? "re-tessellated" : "retained" );
            break;
        }
        case DASH_HIT_VOL_VB:
        case DASH_HIT_VOL_ROW:
        {
            if ( h->a >= sn->vol_count ) return false;
            const dash_vol_t* vo = &sn->vols[ h->a ];
            DASH_LINE( "volatile %s  in %s", dash_name( vo->id, nb, sizeof( nb ) ),
                       dash_name( vo->win, wb, sizeof( wb ) ) );
            DASH_LINE( "verts +%u  %u / %u reserved", vo->lvert_base, vo->vert_count, vo->vert_alloc );
            DASH_LINE( "idx   +%u  %u / %u reserved", vo->lidx_base, vo->idx_count, vo->idx_alloc );
            DASH_LINE( "cmds  %u / %u   gen %u   %s%s", vo->cmd_count, vo->cmd_alloc, vo->tess_gen,
                       vo->active ? "active" : "retired", vo->hidden ? " (hidden)" : "" );
            break;
        }
        case DASH_HIT_BATCH_ROW:
        {
            if ( h->a >= sn->slot_count ) return false;
            const dash_slot_t* sl = &sn->slots[ h->a ];
            DASH_LINE( "window  %s", dash_name( sl->win, nb, sizeof( nb ) ) );
            DASH_LINE( "%u draw cmds  %u verts  %u tris", sl->cmd_count, sl->vert_count,
                       sl->idx_count / 3u );
            DASH_LINE( "z %u  vp %u  gen %u  %s", sl->z, sl->vp, sl->tess_gen,
                       sl->changed ? "re-tessellated" : "retained" );
            break;
        }
        case DASH_HIT_CMD:
        {
            if ( h->a >= sn->slot_count ) return false;
            const dash_slot_t* sl = &sn->slots[ h->a ];
            u32 ci = sl->cmd_base + h->b;
            if ( ci >= sn->cmd_count ) return false;
            const dash_cmd_t* dc = &sn->cmds[ ci ];
            DASH_LINE( "draw %u of %s", (u32)h->b, dash_name( sl->win, nb, sizeof( nb ) ) );
            DASH_LINE( "%u indices (%u tris)  tex %u", dc->elem_count, dc->elem_count / 3u,
                       dc->tex_idx );
            DASH_LINE( "clip %.0f,%.0f  %.0fx%.0f", dc->clip.x, dc->clip.y, dc->clip.w, dc->clip.h );
            DASH_LINE( "vbase %u  first_index %u", dc->vbase, dc->ibase );
            break;
        }
        case DASH_HIT_FIF:
        {
            if ( h->a >= GUI_MAX_VIEWPORTS || !sn->surf[ h->a ].live ) return false;
            const dash_surf_t* sf = &sn->surf[ h->a ];
            DASH_LINE( "surface vp%u  in-flight region %u of %u", (u32)h->a, sf->frame_index,
                       (u32)RHI_MAX_FRAMES_IN_FLIGHT );
            if ( sf->vtx_hi > sf->vtx_lo )
                DASH_LINE( "uploaded verts [%u..%u)  idx [%u..%u)", sf->vtx_lo, sf->vtx_hi,
                           sf->idx_lo, sf->idx_hi );
            else
                DASH_LINE( "nothing uploaded (fully retained)" );
            DASH_LINE( "%u B in %u writes   %u draw calls", sf->up_bytes, sf->up_batches,
                       sf->draw_calls );
            break;
        }
        case DASH_HIT_EMIT_BAR:
        {
            static const char* names[] = { "semantic draw commands", "command segments",
                                           "polyline points", "text pool bytes", "clip rects",
                                           "tessellated vertices", "tessellated indices",
                                           "GPU draw commands" };
            if ( h->a >= sizeof( names ) / sizeof( names[ 0 ] ) ) return false;
            DASH_LINE( "%s", names[ h->a ] );
            break;
        }
        default:
            return false;
        }
        #undef DASH_LINE
        return out->count > 0;
    }
    return false;
}

/*----------------------------------------------------------------------------------------------
    Private cursor tooltip -- probe the hit table (rebuilt just above by this flush's panel
    expanders, so bars and tooltip always agree with zero lag) and lay the text block out beside
    the cursor, clamped to the window.  Drawn last, full-window scissor: nothing in the dashboard
    can paint over it.
----------------------------------------------------------------------------------------------*/

static void
dash_expand_tooltip( i32 win_w, i32 win_h )
{
    gui_dash_probe_t p;
    if ( !gui_dash_probe( s_dash.tip_x, s_dash.tip_y, &p ) || p.count == 0 )
        return;

    const f32 lh  = font_line_h();
    const f32 pad = 6.0f;

    f32 w = 0.0f;
    for ( u32 i = 0; i < p.count; ++i )
    {
        f32 tw = font_text_w( p.line[ i ] );
        if ( tw > w ) w = tw;
    }
    w += pad * 2.0f;
    f32 h = (f32)p.count * lh + pad * 2.0f;

    f32 x = s_dash.tip_x + 16.0f;
    f32 y = s_dash.tip_y + 18.0f;
    if ( x + w > (f32)win_w ) x = (f32)win_w - w;
    if ( y + h > (f32)win_h ) y = s_dash.tip_y - h - 6.0f;
    if ( x < 0.0f ) x = 0.0f;
    if ( y < 0.0f ) y = 0.0f;

    dash_fill( x, y, w, h, GUI_COLOR( 0x1A, 0x1A, 0x20, 0xF4 ) );
    dash_outline( ( gui_rect_t ){ x, y, w, h }, 1.0f, GUI_COLOR( 0x50, 0x50, 0x5C, 0xFF ) );
    for ( u32 i = 0; i < p.count; ++i )
        dash_text( x + pad, y + pad + (f32)i * lh, x + w - pad, DASH_COL_TEXT, p.line[ i ] );
}

/*==============================================================================================
    Flush -- expand every live panel for this surface into scratch, upload once, draw one
    scissored indexed call per panel; the cursor tooltip rides the same upload as one extra
    full-window-scissor draw at the very end.  Mirrors gui_debug_flush; same region scheme.
==============================================================================================*/

void
gui_dash_flush( gui_vp_t vp, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    if ( vp >= GUI_MAX_VIEWPORTS || !rhi_cmd_valid( cmd ) )
        return;
    if ( !rhi_handle_valid( s_dash.vb ) || !font_valid() )
        return;

    bool any = false;
    for ( u32 p = 0; p < GUI_DASH_PANEL_COUNT; ++p )
        if ( s_dash.canvas[ p ].live && s_dash.canvas[ p ].vp == vp
             && s_dash.canvas[ p ].clip.w > 0.0f && s_dash.canvas[ p ].clip.h > 0.0f )
            any = true;
    if ( !any )
        return;

    s_dash.vc = s_dash.ic = 0;
    s_dash.overflow  = false;
    s_dash.hit_count = 0;                       /* all canvases share the dashboard's viewport */
    font_white_uv( &s_dash.wu, &s_dash.wv );

    struct { u32 lo, hi; } range[ GUI_DASH_PANEL_COUNT + 1 ] = { 0 };   /* +1: cursor tooltip */

    for ( u32 p = 0; p < GUI_DASH_PANEL_COUNT; ++p )
    {
        const dash_canvas_t* cv = &s_dash.canvas[ p ];
        range[ p ].lo = range[ p ].hi = s_dash.ic;
        if ( !cv->live || cv->vp != vp || cv->clip.w <= 0.0f || cv->clip.h <= 0.0f )
            continue;

        switch ( p )
        {
        case GUI_DASH_PANEL_VBMAP:    dash_expand_memmap  ( cv->r, true  ); break;
        case GUI_DASH_PANEL_IBMAP:    dash_expand_memmap  ( cv->r, false ); break;
        case GUI_DASH_PANEL_FIF:      dash_expand_fif     ( cv->r );        break;
        case GUI_DASH_PANEL_BATCH:    dash_expand_batch   ( cv->r );        break;
        case GUI_DASH_PANEL_EMIT:     dash_expand_emit    ( cv->r );        break;
        case GUI_DASH_PANEL_VOLATILE: dash_expand_volatile( cv->r );        break;
        case GUI_DASH_PANEL_STATS:    dash_expand_stats   ( cv->r );        break;
        default: break;
        }
        range[ p ].hi = s_dash.ic;
    }

    /* Cursor tooltip last -- probes the hit rects the expanders just recorded. */
    range[ GUI_DASH_PANEL_COUNT ].lo = s_dash.ic;
    if ( s_dash.tip_armed )
        dash_expand_tooltip( win_w, win_h );
    range[ GUI_DASH_PANEL_COUNT ].hi = s_dash.ic;

    if ( s_dash.ic == 0 )
        return;

    if ( s_dash.overflow )
    {
        static bool warned = false;
        if ( !warned )
        {
            printf( "[gui] WARNING: pipeline dashboard scratch overflow -- content truncated "
                    "(caps %u verts, %u idx).\n", GUI_DASH_MAX_VERTS, GUI_DASH_MAX_IDX );
            warned = true;
        }
    }

    u32 frame  = rhi()->cmd_frame_index( cmd );
    u32 vb_off = ( frame * GUI_MAX_VIEWPORTS + vp ) * (u32)GUI_DASH_VB_REGION_BYTES;
    u32 ib_off = ( frame * GUI_MAX_VIEWPORTS + vp ) * (u32)GUI_DASH_IB_REGION_BYTES;

    rhi()->buffer_write( s_dash.vb, s_dash.scratch_verts, s_dash.vc * sizeof( gui_draw_vert_t ), vb_off );
    rhi()->buffer_write( s_dash.ib, s_dash.scratch_idx,   s_dash.ic * sizeof( u16 ),             ib_off );

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
    rhi()->cmd_bind_vertex_buffer( cmd, s_dash.vb, vb_off );
    rhi()->cmd_bind_index_buffer ( cmd, s_dash.ib, ib_off, RHI_INDEX_TYPE_UINT16 );

    gui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );
    push.samp_idx = s_render.font_sampler_idx;
    push.tex_idx  = font_atlas_idx();
    push.dbg_flat = 0u;   /* the dashboard always renders normally, never flat/batch-tinted */
    push.dbg_tint = 0u;
    rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );

    for ( u32 p = 0; p <= GUI_DASH_PANEL_COUNT; ++p )
    {
        if ( range[ p ].hi <= range[ p ].lo )
            continue;
        /* Panels scissor to their canvas clip; the tooltip (the +1 slot) gets the full window. */
        gui_rect_t full = { 0.0f, 0.0f, (f32)win_w, (f32)win_h };
        const gui_rect_t* cl = ( p < GUI_DASH_PANEL_COUNT ) ? &s_dash.canvas[ p ].clip : &full;
        i32 sx = (i32)cl->x, sy = (i32)cl->y;
        i32 sw = (i32)( cl->w + 0.5f ), sh = (i32)( cl->h + 0.5f );
        if ( sx < 0 ) { sw += sx; sx = 0; }
        if ( sy < 0 ) { sh += sy; sy = 0; }
        if ( sx + sw > win_w ) sw = win_w - sx;
        if ( sy + sh > win_h ) sh = win_h - sy;
        if ( sw <= 0 || sh <= 0 )
            continue;
        rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){ .x = sx, .y = sy, .width = sw, .height = sh } );

        rhi()->cmd_draw_indexed( cmd, &( rhi_draw_indexed_args_t ){
            .index_count    = range[ p ].hi - range[ p ].lo,
            .instance_count = 1,
            .first_index    = range[ p ].lo,
            .vertex_offset  = 0,
            .first_instance = 0,
        } );
    }

    rhi()->cmd_end_rendering( cmd );
}

#endif /* GUI_PIPELINE_DASHBOARD */

// clang-format on
/*============================================================================================*/
