/*==============================================================================================

    runtime_service/gui/render/gui_render_mem.c -- Backend memory accounting.

    Fills the backend-owned buckets of gui_mem_stats_t (gui.h): the GPU device memory the
    backend created, and every fixed CPU static the backend unity TU defines -- the resident
    footprint the image pays whether one window is open or fifty.  The frontend (gui_mem_stats,
    core/gui_ctx.c) adds the per-context heap blocks and the totals.

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

    /* GPU: per-surface geometry buffers x live surfaces, plus the font atlas textures. */
    s.viewport_count    = live_viewports;
    s.gpu_vertex_bytes  = live_viewports * RHI_MAX_FRAMES_IN_FLIGHT * (u32)GUI_VB_REGION_BYTES;
    s.gpu_index_bytes   = live_viewports * RHI_MAX_FRAMES_IN_FLIGHT * (u32)GUI_IB_REGION_BYTES;
    s.gpu_texture_bytes = res_atlas_bytes() + res_sprite_bytes();   /* sprite atlas: 0 until used */
#ifdef GUI_DEBUG_OVERLAY
    /* The overlay's own VB/IB (one region per viewport per frame-in-flight, dbg_init). */
    if ( rhi_handle_valid( s_dbg.vb ) )
        s.gpu_debug_bytes = (u32)( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS
                                 * ( GUI_DBG_VB_REGION_BYTES + GUI_DBG_IB_REGION_BYTES ) );
#endif
    s.gpu_total = s.gpu_vertex_bytes + s.gpu_index_bytes + s.gpu_texture_bytes + s.gpu_debug_bytes;

    /* EMIT: the semantic draw list and the line/path stroker built on it. */
    s.cpu_drawlist_bytes = (u32)( sizeof( s_draw ) + sizeof( s_path ) );

    /* BUILD: tessellation staging + the unit-arc tables the rounded-rect tessellator caches. */
    s.cpu_tess_bytes = (u32)( sizeof( s_tess ) + sizeof( s_arc_cos ) + sizeof( s_arc_sin ) );

    /* Retained cache: ping-pong slot tables, dispatch order, the id-keyed stable command cache
       (entries + counts + keys + occupancy), diff records + stats, per-window segment chains,
       the clip-sort permutation scratch, and the volatile-widget registry. */
    s.cpu_cache_bytes = (u32)( sizeof( s_slots_a ) + sizeof( s_slots_b ) + sizeof( s_dispatch )
                             + sizeof( s_win_cached ) + sizeof( s_win_cached_count )
                             + sizeof( s_win_cached_win ) + sizeof( s_win_cached_live )
                             + sizeof( s_cache ) + sizeof( s_stats ) + sizeof( s_seg_next )
                             + sizeof( s_win_order ) + sizeof( s_win_font )
                             + sizeof( s_volatile )
                             + sizeof( s_patch_order ) + sizeof( s_patch_font ) );

    /* Fonts + icons are the DRAW unit's statics now (registry slots, reload queue, icon
       tables) -- reported through its seam so the bucket stays populated. */
    s.cpu_font_bytes = draw_unit_mem_bytes();

    /* The two resource atlases (packer + tenant bookkeeping).  Both instance records are always
       resident; only the sprite atlas's PIXEL buffer is conditional, and that is GPU/heap, not a
       static -- see gpu_texture_bytes above. */
    s.cpu_res_bytes = (u32)( sizeof( s_res ) + sizeof( s_spr ) );

    /* RENDER: pipeline/sampler/push state + the embedded SPIR-V bytecode (.rdata). */
    s.cpu_render_bytes = (u32)( sizeof( s_render )
                              + sizeof( s_gui_vert_spirv ) + sizeof( s_gui_frag_spirv ) );

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

    s.cpu_static_total = s.cpu_drawlist_bytes + s.cpu_tess_bytes + s.cpu_cache_bytes
                       + s.cpu_font_bytes + s.cpu_res_bytes + s.cpu_render_bytes
                       + s.cpu_select_bytes + s.cpu_debug_bytes;
    return s;
}

// clang-format on
/*============================================================================================*/
