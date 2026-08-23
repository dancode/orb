/*==============================================================================================

    runtime_service/gui/render/gui_render_mem.c -- Backend memory accounting + pool report.

    Fills the backend-owned buckets of gui_mem_stats_t (gui.h): the GPU device memory the
    backend created, and every fixed CPU static the backend unity TU defines -- the resident
    footprint the image pays whether one window is open or fifty.  The frontend (gui_mem_stats,
    core/gui_ctx.c) adds the per-context heap blocks and the totals.  backend_pool_report at the
    foot is the other half of the same question: not how much the pools COST, but how full they
    have been, which is what a raised or lowered cap should be argued from.

    MUST be the LAST include in gui_render.c: every bucket below is a sizeof over another
    file's static, and unity visibility only flows downward.  Adding a static to the backend?
    Add it to a bucket here -- the full-accounting contract is that the grand total is the true
    resident footprint, not a sampling of the big arrays.

    Compiled-out features (debug overlay / dashboard / stepper) contribute 0 through the same
    #ifdefs that remove their state.  Not counted: string literals (pooled by the linker) and
    the few scalar statics (flags, counters, heads) -- sub-cache-line noise.

==============================================================================================*/
// clang-format off

gui_mem_stats_t
backend_memory( u32 live_viewports )
{
    gui_mem_stats_t s;
    memset( &s, 0, sizeof( s ) );

    /* GPU: the atlas textures, plus the storage-buffer tables below.  Surfaces own no geometry
       buffers of their own -- the quad table's (frame-in-flight, viewport) regions are baked
       into its size. */
    s.viewport_count    = live_viewports;
    /* Sprite and SDF atlases report 0 until something creates them. */
    s.gpu_texture_bytes = res_atlas_bytes() + res_sprite_bytes() + res_sdf_bytes();

    /* The storage-buffer tables the pipeline resolves through.  Clip is REGIONED: rewritten
       every frame, so each frame-in-flight needs a full set of window slabs to land in (one copy
       per frame only -- a window's clip slab means the same thing to every viewport,
       gui_render_init.c).  The quad and prim tables are CLAIM-sized: per-viewport claims over a
       frame-in-flight-copied buffer that grows on demand, so their bytes are live measurements,
       not constants -- the prim buffer additionally leads with the fixed header (the palette
       blocks + the overlay records, gui_render_pal.c).  The glyph table changes only when a font
       enters the atlas or a repack moves a page, so ONE buffer serves every surface and is
       replaced wholesale on the rare rebuild. */
    s.gpu_clip_regions = (u32)GUI_CLIP_REGION_COUNT;
    if ( rhi_handle_valid( s_render.clip_buf ) )
        s.gpu_clip_bytes  = (u32)( GUI_CLIP_REGION_COUNT * GUI_CLIP_REGION_BYTES );
    s.gpu_prim_capacity = s_prim_gpu.capacity;
    if ( rhi_handle_valid( s_render.prim_buf ) )
        s.gpu_prim_bytes = (u32)( ( GUI_PRIM_HDR_RECORDS
                                    + RHI_MAX_FRAMES_IN_FLIGHT * s_prim_gpu.capacity )
                                  * GUI_PRIM_BYTES );
    s.gpu_quad_capacity = s_quad_gpu.capacity;
    if ( rhi_handle_valid( s_render.quad_buf ) )
        s.gpu_quad_bytes = (u32)( RHI_MAX_FRAMES_IN_FLIGHT * s_quad_gpu.capacity * GUI_QUAD_BYTES );
    if ( rhi_handle_valid( s_render.glyph_buf ) )
        s.gpu_glyph_bytes = s_render.glyph_buf_bytes;

    s.gpu_table_bytes = s.gpu_clip_bytes + s.gpu_prim_bytes + s.gpu_quad_bytes
                      + s.gpu_glyph_bytes;

#ifdef GUI_DEBUG_OVERLAY

    /* The overlay's own quad table (one region per viewport per frame-in-flight, dbg_init). */
    if ( rhi_handle_valid( s_dbg.quads ) )
        s.gpu_debug_bytes = (u32)( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS
                                 * GUI_DBG_QUAD_REGION_BYTES );
#endif

    s.gpu_total = s.gpu_texture_bytes + s.gpu_table_bytes + s.gpu_debug_bytes;

    /* EMIT: the semantic draw list and the line/path stroker built on it. */
    s.cpu_drawlist_bytes = (u32)( sizeof( s_draw ) + sizeof( s_path ) );

    /* BUILD: tessellation staging -- the quad / style / gpu-command arenas (s_tess), the cold
       diagnostics beside them, and the per-(frame, viewport) dirty-span table the flush drains.
       No corner geometry is cached at all: rounded shapes are resolved by the fragment. */
    s.cpu_tess_bytes = (u32)( sizeof( s_tess ) + sizeof( s_tess_stats )
                            + sizeof( s_patch_pending ) );

    /* Retained cache: ping-pong slot tables, dispatch order, the id-keyed stable command cache
       (entries + counts + keys + occupancy), diff records + stats, per-window segment chains,
       the clip-sort permutation scratch, and the volatile-widget registry. */
    s.cpu_cache_bytes = (u32)( sizeof( s_slots_a ) + sizeof( s_slots_b ) + sizeof( s_dispatch )
                             + sizeof( s_win_cached ) + sizeof( s_win_cached_count )
                             + sizeof( s_win_cached_win ) + sizeof( s_win_cached_live )
                             + sizeof( s_cache ) + sizeof( s_stats ) + sizeof( s_seg_next )
                             + sizeof( s_win_order )
                             + sizeof( s_volatile )
                             + sizeof( s_clip_slab_pending ) + sizeof( s_prim_range_pending )
                             + sizeof( s_patch_order ) );

    /* The DRAW unit's statics (icon + sprite registries) -- the font registry moved to the font/
       leaf and reports through the frontend bucket (gui_ui_mem.c). */
    s.cpu_draw_bytes = draw_unit_mem_bytes();

    /* The three resource atlas instance records (packer nodes + tenant bookkeeping), plus the
       shared skyline scratch a repack trial runs against.  Always-resident statics; the atlases'
       PIXEL buffers and tenant source copies are heap, reported in cpu_atlas_bytes below. */
    s.cpu_res_bytes = (u32)( sizeof( s_res ) + sizeof( s_spr ) + sizeof( s_sdf )
                           + sizeof( s_trial_nodes ) );

    /* Atlas-owned heap: each atlas's resident staging mirror plus every tenant's retained source
       copy (fonts, icons, sprites keep a second CPU copy so a repack never goes back to disk).
       A dynamic bucket -- it exists only once an atlas / tenant does. */
    s.cpu_atlas_bytes = res_atlas_cpu_bytes();

    /* RENDER: pipeline / sampler / push state, the published prim palette the flush uploads from,
       and the palette's own working set -- the live table, its content-hash lookup, the candidate
       hashes waiting on a second sighting, and the per-command parked answers.  The shaders are
       loaded from bin/shaders and never sit in the exe's .rdata. */
    s.cpu_render_bytes = (u32)( sizeof( s_render ) + sizeof( s_pal ) + sizeof( s_intern )
                              + sizeof( s_cmd_entry ) + sizeof( s_cmd_hash ) );

    /* Text-selection run capture (always compiled; a product feature). */
    s.cpu_select_bytes = (u32)sizeof( s_select_cap );

    /* Debug tooling -- each block exists only when its feature is compiled in. */
#ifdef GUI_DEBUG_OVERLAY
    s.cpu_debug_bytes += (u32)( sizeof( s_dbg ) + sizeof( s_dbg_names ) );
#endif
#ifdef GUI_PIPELINE_DASHBOARD
    s.cpu_debug_bytes += (u32)sizeof( s_dash );
#endif
#ifdef GUI_CMD_STEPPER
    s.cpu_debug_bytes += (u32)sizeof( s_step );
#endif
#ifdef GUI_PRIM_CENSUS
    s.cpu_debug_bytes += (u32)sizeof( s_census );
#endif

    s.cpu_static_total = s.cpu_drawlist_bytes + s.cpu_tess_bytes + s.cpu_cache_bytes
                       + s.cpu_draw_bytes + s.cpu_res_bytes + s.cpu_render_bytes
                       + s.cpu_select_bytes + s.cpu_debug_bytes;
    return s;
}

/*==============================================================================================
    backend_pool_report -- lifetime peak fill of every capped pool, against its cap.

    The companion to the byte totals above, and the answer to the question that actually comes up:
    a cap was hit (or is suspected), so which one, and how close are the others?  Every number here
    is a lifetime HIGH-WATER MARK, not a live count -- a pool that peaked at 98% on one busy frame
    is the one to raise, however empty it looks right now.

    Split by stage, because the two halves fill for different reasons.  EMIT pools are bounded by
    what the UI code asks for (one command per draw_push_*, one entry per distinct clip rect); the
    BUILD pools are bounded by what those commands expand into, which a single text run can move a
    thousand quads of.  A raised cap costs .bss on the EMIT side and .bss PLUS eight GPU regions on
    the BUILD side, which is why the two are worth reading apart.

    Called from gui_print_mem_stats (gui_ui_mem.c), which cannot see these statics itself.
==============================================================================================*/

void
backend_pool_report( void )
{
    #define GUI_POOL_ROW( label, used, cap ) \
        gui_log( GUI_LOG_INFO, "  %-22s %6u / %-6u  (%5.1f%%)", (label), (u32)(used), (u32)(cap), \
                 100.0f * (f32)(u32)(used) / (f32)(u32)(cap) )

    gui_log( GUI_LOG_INFO, "  -- pool peaks (lifetime high-water) --------------------" );
    GUI_POOL_ROW( "quad records",   s_tess_stats.quad_hwm,  GUI_MAX_QUADS );
    GUI_POOL_ROW( "prim records",   s_tess_stats.prim_hwm,  GUI_MAX_PRIMS );
    GUI_POOL_ROW( "gpu draw cmds",  s_tess_stats.cmd_hwm,   GUI_MAX_CMDS );
    GUI_POOL_ROW( "semantic cmds",  s_draw.cmd_hwm,         GUI_MAX_CMDS );

    /* Per-type breakdown of the row above: which gui_cmd_type_t values actually fill the
       semantic cmd pool, each against the same GUI_MAX_CMDS cap.  Each type's peak is its own
       independent lifetime high-water mark (may not all have landed on the same frame), same
       as every other row in this report -- read it as "how big could this type alone get",
       not as a breakdown that sums to the pool's peak. */
    {
        static const char* const k_cmd_type_name[ GUI_CMD_COUNT ] = {
            [GUI_CMD_RECT_FILL]     = "  rect_fill",
            [GUI_CMD_RECT_TEX]      = "  rect_tex",
            [GUI_CMD_RECT_OUTLINE]  = "  rect_outline",
            [GUI_CMD_TRIANGLE]      = "  triangle",
            [GUI_CMD_BEZIER]        = "  bezier",
            [GUI_CMD_TEXT]          = "  text",
            [GUI_CMD_TEXT_XF]       = "  text_xf",
            [GUI_CMD_TEXT_SHADOW]   = "  text_shadow",
            [GUI_CMD_LINE]          = "  line",
            [GUI_CMD_POLYLINE]      = "  polyline",
            [GUI_CMD_DASHED_LINE]   = "  dashed_line",
            [GUI_CMD_RECT_GRADIENT] = "  rect_gradient",
            [GUI_CMD_RECT_LIST]     = "  rect_list",
            [GUI_CMD_SPRITE]        = "  sprite",
            [GUI_CMD_FX_BOX]        = "  fx_box",
            [GUI_CMD_ROUND_RECT_EX] = "  round_rect_ex",
            [GUI_CMD_ARC]           = "  arc",
            [GUI_CMD_PIE]           = "  pie",
            [GUI_CMD_ARC_DASH]      = "  arc_dash",
            [GUI_CMD_ARC_GRAD]      = "  arc_grad",
            [GUI_CMD_IMAGE_XF]      = "  image_xf",
            [GUI_CMD_CHECKER]       = "  checker",
            [GUI_CMD_GRID]          = "  grid",
            [GUI_CMD_NGON]          = "  ngon",
            [GUI_CMD_BOX_DASH]      = "  box_dash",
            [GUI_CMD_FRAME]         = "  frame",
            [GUI_CMD_REPEAT]        = "  repeat",
            [GUI_CMD_REPEAT_POLAR]  = "  repeat_polar",
            [GUI_CMD_BOX_CUT]       = "  box_cut",
        };

        for ( u32 t = 0; t < GUI_CMD_COUNT; ++t )
            if ( s_draw.cmd_type_hwm[ t ] > 0 )
                GUI_POOL_ROW( k_cmd_type_name[ t ], s_draw.cmd_type_hwm[ t ], GUI_MAX_CMDS );
    }

    /* Payload pool peak: bytes claimed from s_draw.cmd_pool by every command's payload at once,
       on the busiest frame -- see draw_cmd_ext_slot().  Independent of the envelope cap above:
       this is what GUI_CMD_POOL_BYTES should be sized from. */
    GUI_POOL_ROW( "cmd pool (B)", s_draw.pool_hwm, GUI_CMD_POOL_BYTES );

    GUI_POOL_ROW( "cmd segments",   s_draw.seg_hwm,         GUI_MAX_SEGS );
    GUI_POOL_ROW( "clip rects",     s_draw.clip_hwm,        GUI_MAX_CLIP_RECTS );
    GUI_POOL_ROW( "text pool (B)",  s_draw.text_hwm,        GUI_MAX_TEXT_POOL );
    GUI_POOL_ROW( "path points",    s_draw.pt_hwm,          GUI_MAX_PATH_PTS );
    GUI_POOL_ROW( "rect entries",   s_draw.rect_hwm,        GUI_MAX_RECT_ENTRIES );
    GUI_POOL_ROW( "windows",        s_tess_stats.win_hwm,   RENDER_MAX_WIN );
    GUI_POOL_ROW( "volatile rows",  s_volatile_count,       GUI_MAX_VOLATILE );

    if ( s_tess_stats.overflow_walls )
    {
        char walls[ 128 ];
        gui_log( GUI_LOG_WARN, "  %-22s %s", "OVERFLOWED",
                 tess_overflow_walls( s_tess_stats.overflow_walls, walls, (u32)sizeof( walls ) ) );
    }

    #undef GUI_POOL_ROW
}

// clang-format on
/*============================================================================================*/
