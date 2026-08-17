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
    /* Sprite and SDF atlases report 0 until something creates them. */
    s.gpu_texture_bytes = res_atlas_bytes() + res_sprite_bytes() + res_sdf_bytes();

    /* The two storage-buffer tables the fragment resolves through: clip entries and primitive
       records.  Both are allocated whole at init and are NOT per-surface -- one region per
       (frame-in-flight, viewport) is baked into the size -- so unlike the geometry above they do
       not scale with live_viewports.  The record table dominates by two orders of magnitude,
       which is why it is worth reporting rather than folding into a "misc". */
    if ( rhi_handle_valid( s_render.clip_buf ) )
        s.gpu_table_bytes += (u32)( GUI_CLIP_REGION_COUNT * GUI_CLIP_REGION_BYTES );
    if ( rhi_handle_valid( s_render.prim_buf ) )
        s.gpu_table_bytes += (u32)( GUI_PRIM_REGION_COUNT * GUI_PRIM_REGION_BYTES );
    s.gpu_table_bytes += glyph_table_gpu_bytes();   /* the glyph uv table (0 until a font packs) */

#ifdef GUI_DEBUG_OVERLAY

    /* The overlay's own VB/IB (one region per viewport per frame-in-flight, dbg_init). */
    if ( rhi_handle_valid( s_dbg.vb ) )
        s.gpu_debug_bytes = (u32)( RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS
                                 * ( GUI_DBG_VB_REGION_BYTES + GUI_DBG_IB_REGION_BYTES ) );
#endif

    s.gpu_total = s.gpu_vertex_bytes + s.gpu_index_bytes + s.gpu_texture_bytes
                + s.gpu_table_bytes  + s.gpu_debug_bytes;

    /* EMIT: the semantic draw list and the line/path stroker built on it. */
    s.cpu_drawlist_bytes = (u32)( sizeof( s_draw ) + sizeof( s_path ) );

    /* BUILD: tessellation staging.  The unit-arc tables that used to be counted here are gone --
       rounded shapes are resolved by the fragment shader now (the effect band), so the backend
       caches no corner geometry at all. */
    s.cpu_tess_bytes = (u32)sizeof( s_tess );

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

    /* The three resource atlas instance records (packer nodes + tenant bookkeeping) plus the
       glyph uv table's CPU mirror.  Always-resident statics; the atlases' PIXEL buffers and
       tenant source copies are heap, reported in cpu_atlas_bytes below. */
    s.cpu_res_bytes = (u32)( sizeof( s_res ) + sizeof( s_spr ) + sizeof( s_sdf )
                           + sizeof( s_glyph_table ) );

    /* Atlas-owned heap: each atlas's resident staging mirror plus every tenant's retained source
       copy (fonts, icons, sprites keep a second CPU copy so a repack never goes back to disk).
       A dynamic bucket -- it exists only once an atlas / tenant does. */
    s.cpu_atlas_bytes = res_atlas_cpu_bytes();

    /* RENDER: pipeline / sampler / push state.  The shader bytecode used to be counted here as
       well; it is loaded from bin/shaders now and never sits in the exe's .rdata. */
    s.cpu_render_bytes = (u32)sizeof( s_render );

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
                       + s.cpu_draw_bytes + s.cpu_res_bytes + s.cpu_render_bytes
                       + s.cpu_select_bytes + s.cpu_debug_bytes;
    return s;
}

// clang-format on
/*============================================================================================*/
