/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_render_submit.c -- Per-surface GPU submit

    Last of the three render phases:

        EMIT    gui_emit_draw.c    widgets -> s_draw semantic command list
        BUILD   gui_build_cache.c  diff + tessellate -> s_tess quad records + s_dispatch slots
        RENDER  this file          upload each surface's slots + emit draw calls

    One job, per-surface: flush (gui_render_flush) -- trigger the once-per-frame BUILD if it
    hasn't run yet, then upload that surface's quad/clip/style spans into its (frame-in-flight,
    viewport) regions of the global tables and issue one bufferless draw call per command.
    Surfaces own no geometry buffers of their own.

    Included by gui_render.c right after gui_render_init.c, whose gui_push_t layout and s_render
    static this file reads.

==============================================================================================*/

// clang-format off
/*==============================================================================================
    Debug render mode -- the debug view (normal / wireframe / batch-tint).

    Cheap to flip every frame; read by gui_render_flush.  Backs gui()->debug_set/get_render_mode.
==============================================================================================*/

void
gui_render_set_mode( gui_render_mode_t mode )
{
    if ( mode < 0 || mode >= GUI_RENDER_MODE_COUNT )
        mode = GUI_RENDER_NORMAL;
    s_render.debug_mode = mode;
}

gui_render_mode_t
gui_render_get_mode( void )
{
    return s_render.debug_mode;
}

/*==============================================================================================
    Frame clock -- the effect band's `time` push constant.

    Handed down once per app frame from the orchestrator, which owns the IO snapshot this server
    cannot see.  Wrapping is the CALLER's job so the wrap point is stated once, in the public
    header, next to the constant that defines it.
==============================================================================================*/

void
gui_render_set_time( f32 seconds )
{
    s_render.fx_time = seconds;
}


/*==============================================================================================
    render_ortho -- column-major pixel-space orthographic matrix.

    Maps pixel coords ([0,w] x [0,h], origin top-left) to Vulkan NDC:
        x: [0,w] -> [-1,+1]   y: [0,h] -> [-1,+1]  (top-left is -1,-1 in Vulkan NDC)
==============================================================================================*/

static void
render_ortho( f32 out[ 16 ], f32 w, f32 h )
{
    out[  0 ] =  2.0f / w; out[  1 ] =  0.0f;     out[  2 ] = 0.0f; out[  3 ] = 0.0f;
    out[  4 ] =  0.0f;     out[  5 ] =  2.0f / h; out[  6 ] = 0.0f; out[  7 ] = 0.0f;
    out[  8 ] =  0.0f;     out[  9 ] =  0.0f;     out[ 10 ] = 1.0f; out[ 11 ] = 0.0f;
    out[ 12 ] = -1.0f;     out[ 13 ] = -1.0f;     out[ 14 ] = 0.0f; out[ 15 ] = 1.0f;
}

/* render_batch_debug_color -- a distinct, saturated, fully-opaque color per draw-call index for the BATCH
   view.  Packed RGBA8 (R low byte), matching the shader's dbg_tint decode and GUI_COLOR byte order.
   A 12-entry table cycles; consecutive entries are spread around the hue wheel so neighbouring
   batches stay easy to tell apart, and the wrap is harmless (it only marks boundaries). */
static u32
render_batch_debug_color( u32 i )
{
    static const u32 palette[ 12 ] = {
        GUI_COLOR( 0xE6, 0x39, 0x46, 0xFF ),   // red
        GUI_COLOR( 0x2A, 0x9D, 0x8F, 0xFF ),   // teal
        GUI_COLOR( 0xE9, 0xC4, 0x6A, 0xFF ),   // yellow
        GUI_COLOR( 0x45, 0x7B, 0x9D, 0xFF ),   // blue
        GUI_COLOR( 0xF4, 0x7A, 0x20, 0xFF ),   // orange
        GUI_COLOR( 0x8E, 0x44, 0xAD, 0xFF ),   // purple
        GUI_COLOR( 0x6A, 0xBE, 0x30, 0xFF ),   // green
        GUI_COLOR( 0xD6, 0x4B, 0x9C, 0xFF ),   // pink
        GUI_COLOR( 0x34, 0x98, 0xDB, 0xFF ),   // sky
        GUI_COLOR( 0xC0, 0x8A, 0x3E, 0xFF ),   // brown
        GUI_COLOR( 0x1A, 0xBC, 0x9C, 0xFF ),   // mint
        GUI_COLOR( 0xBD, 0xC3, 0xC7, 0xFF ),   // silver
    };
    return palette[ i % 12u ];
}

/*==============================================================================================
    gui_render_flush -- upload one surface's geometry and emit its draw calls (SUBMIT phase).

    Paints into the surface at index `vp_index`, whose target arrives as a parameter -- the
    orchestrator (frame/gui_frame_loop.c) passes the record's fields; this server never sees the
    record.  First kicks the once-per-frame BUILD (cache_build_frame, lazy -- only the first
    surface this frame pays for it; the rest reuse the result).  Then uploads this surface's
    slice of the shared quad arena into its own (frame-in-flight, viewport) region of the global
    quad table and opens a LOAD pass on the target.  The host calls this once per live surface
    with that surface's drawable size; slots tagged for another viewport are skipped.

    Geometry is shared: the whole quad span is uploaded to every surface's region and each
    surface draws only its own slots (a draw's first_vertex carries the arena-absolute quad
    offset * 6).  LOAD preserves the scene rendered before this call.

    The scissor is set ONCE, to the full surface: clipping is resolved per fragment against the
    frame clip table (gui_fx.hlsli, clip_coverage), which this flush also uploads -- each
    dispatched slot's local clip entries, concatenated into this (frame, viewport) region, with
    the slot's base entry pushed per draw (pc.clip_base).
==============================================================================================*/

void
gui_render_flush( rhi_texture_t target, i32 vp_index, rhi_cmd_t cmd, i32 win_w, i32 win_h )
{
    // Nothing was recorded this frame, or the caller's command buffer isn't ready to record into.
    if ( s_draw.cmd_count == 0 || !rhi_cmd_valid( cmd ) )
        return;

    // Tessellate + sort the shared list once per frame; this surface reuses the cached result.
    cache_build_frame();

    // Select this frame's quad region so the upload cannot clobber data the GPU is still reading
    // for another in-flight frame.
    u32 frame = rhi()->cmd_frame_index( cmd );

    /* This surface's quad upload span, taken from the slot table.  Slots tagged for this
       viewport contribute their [vert_base, +count) ranges; the union is what we upload.  For a
       single surface this covers the whole arena. */
    u32 vtx_lo = s_tess.vert_count, vtx_hi = 0;

    u32 overlay_bytes = 0;   // debug-band windows' share, excluded from the upload stats

    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* sl = s_dispatch[ d ];
        if ( sl->vp != vp_index || sl->vert_count == 0 ) continue;

        if ( sl->band != 0 )
            overlay_bytes += sl->vert_count * (u32)sizeof( gui_quad_t );

        if ( sl->vert_base                     < vtx_lo ) vtx_lo = sl->vert_base;
        if ( sl->vert_base + sl->vert_count    > vtx_hi ) vtx_hi = sl->vert_base + sl->vert_count;
    }

    u32 up_batches = 0;   // geometry buffer_write calls this flush issued (full spans or patch spans)
    u32 up_bytes = 0;     // total bytes those writes moved

    /* Geometry-generation skip: if this (frame, viewport) region already holds the current
       arena generation (s_geo_gen, gui_build_tess.c), its bytes are identical to what these
       spans would upload -- a presented-but-unchanged frame (fx animation, reused real frame)
       moves nothing.  Any live-byte change bumped the generation, so the first flush after it
       re-uploads and re-stamps. */
    static u32 s_region_geo_gen[ RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS ];

    u32  clip_region = frame * (u32)GUI_MAX_VIEWPORTS + (u32)vp_index;
    bool geo_dirty   = s_region_geo_gen[ clip_region ] != s_geo_gen;
    if ( geo_dirty )
        s_region_geo_gen[ clip_region ] = s_geo_gen;

    /* The glyph UV table, keyed on its own generation rather than the geometry's: the rects only
       move when a font's pixels enter the atlas or a repack shifts a page.  A move replaces the
       whole buffer and retires the old one through rhi's deferred destroy, so this is a no-op on
       every frame but the handful that follow a font upload or a repack. */
    render_glyph_buf_refresh();

    // Upload the quad span into this surface's region of the global quad table -- only if this
    // surface actually touched geometry in that range (and the region is stale at all).
    u32 quad_off = clip_region * (u32)GUI_QUAD_REGION_BYTES;
    if ( geo_dirty && vtx_hi > vtx_lo )
    {
        u32 bytes = ( vtx_hi - vtx_lo ) * (u32)sizeof( gui_quad_t );
        rhi()->buffer_write( s_render.quad_buf, &s_tess.quads[ vtx_lo ], bytes,
                             quad_off + vtx_lo * (u32)sizeof( gui_quad_t ) );
        up_batches++;
        up_bytes += bytes;
    }

    /* Fine dirty spans (gui_build_tess.c, s_patch_pending), one per arena band so a changed app
       window and a changed overlay never union across the gap between them: a full upload above
       covered every accumulated byte of this surface, so it just clears the entries; a
       generation-matching flush uploads only the accumulated ranges -- a changed window or a
       live volatile widget costs its own bytes per present, not the whole span. */
    u32 up_overlay = geo_dirty ? overlay_bytes : 0;   // debug-band share of what this flush moved
    for ( u32 b = 0; b < 2; ++b )
    {
        u32 pv_lo = s_patch_pending[ clip_region ][ b ].v_lo;
        u32 pv_hi = s_patch_pending[ clip_region ][ b ].v_hi;

        s_patch_pending[ clip_region ][ b ].v_lo = s_patch_pending[ clip_region ][ b ].v_hi = 0;
        if ( geo_dirty )
            continue;   // the full span above already carried these bytes

        if ( pv_hi > pv_lo )
        {
            u32 bytes = ( pv_hi - pv_lo ) * (u32)sizeof( gui_quad_t );
            rhi()->buffer_write( s_render.quad_buf, &s_tess.quads[ pv_lo ], bytes,
                                 quad_off + pv_lo * (u32)sizeof( gui_quad_t ) );
            up_batches++;
            up_bytes += bytes;
            if ( b != 0 ) up_overlay += bytes;
        }
    }

    // Report upload stats net of the debug overlay's own geometry, so the dashboard reflects
    // what the app drew, not the tooling drawn on top of it.
    if ( up_batches > 0 )
    {
        u32 actual_bytes = up_bytes > up_overlay ? up_bytes - up_overlay : 0;
        u32 actual_batches = actual_bytes > 0 ? up_batches : 0;
        cache_count_upload( actual_batches, actual_bytes );
    }

    /* Frame clip table upload: each window's local clip entries live at a FIXED SLAB of this
       (frame, viewport) region -- cache_idx * GUI_WIN_CLIP_MAX, the same base its vertices baked
       into the clip band -- so a slab only re-uploads when its content changed
       (s_clip_slab_pending, set by tess_clip_local on append).  A stable frame uploads nothing;
       nothing per-slot reaches the push constants. */
    u8 region_bit = (u8)( 1u << clip_region );
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* sl = s_dispatch[ d ];
        if ( sl->vp != vp_index || sl->clip_count == 0 )
            continue;
        if ( !( s_clip_slab_pending[ sl->cache_idx ] & region_bit ) )
            continue;
        s_clip_slab_pending[ sl->cache_idx ] &= (u8)~region_bit;

        f32 stage[ GUI_WIN_CLIP_MAX * GUI_CLIP_ENTRY_FLOATS ];
        for ( u32 c = 0; c < sl->clip_count; ++c )
        {
            const gui_clip_entry_t* e = &sl->clips[ c ];
            f32* w = &stage[ c * GUI_CLIP_ENTRY_FLOATS ];

            /* Edges snap to the nearest whole pixel -- the grid the hardware scissor rounded to,
               so the radius-0 fragment test reproduces the old cut exactly (fragment centres sit
               at .5 against integer edges).  No framebuffer clamp: off-surface area has no
               fragments to cut. */
            w[ 0 ] = floorf( e->rect.x + 0.5f );
            w[ 1 ] = floorf( e->rect.y + 0.5f );
            w[ 2 ] = floorf( e->rect.x + e->rect.w + 0.5f );
            w[ 3 ] = floorf( e->rect.y + e->rect.h + 0.5f );
            w[ 4 ] = e->radius;
            w[ 5 ] = 0.0f;
            w[ 6 ] = 0.0f;
            w[ 7 ] = 0.0f;
        }
        rhi()->buffer_write( s_render.clip_buf, stage,
                             sl->clip_count * (u32)GUI_CLIP_ENTRY_BYTES,
                             clip_region * (u32)GUI_CLIP_REGION_BYTES
                                 + sl->cache_idx * GUI_WIN_CLIP_MAX * (u32)GUI_CLIP_ENTRY_BYTES );
    }

    /* Primitive record upload.  The records are already in their final form on the CPU (gui.h,
       gui_prim_t is what the shader reads), so unlike the clip entries there is no staging step --
       a slot's run copies straight out of the arena.
       Where this genuinely differs from the clip slabs above: a record range is PACKED, so its
       offset moves whenever the arena repacks or the window relocates.  s_prim_range_pending is
       therefore set by every fresh tessellation (which is the only thing that can move a base or
       change a record) rather than only on content change. */
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* sl = s_dispatch[ d ];
        if ( sl->vp != vp_index || sl->prim_count == 0 )
            continue;
        if ( !( s_prim_range_pending[ sl->cache_idx ] & region_bit ) )
            continue;
        s_prim_range_pending[ sl->cache_idx ] &= (u8)~region_bit;

        rhi()->buffer_write( s_render.prim_buf, &s_tess.prims[ sl->prim_base ],
                             sl->prim_count * (u32)GUI_PRIM_BYTES,
                             clip_region * (u32)GUI_PRIM_REGION_BYTES
                                 + sl->prim_base * (u32)GUI_PRIM_BYTES );
    }

    /* Open a LOAD pass on the swapchain color target (no depth).  LOAD preserves the scene content
       rendered before this call; CLEAR would wipe it. */
    rhi_color_attachment_t color_att = {
        .texture  = target,
        .load_op  = RHI_LOAD_OP_LOAD,
        .store_op = RHI_STORE_OP_STORE,
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    // Full-window viewport (dynamic state; must be set before the first draw).
    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f,
        .width     = (f32)win_w,
        .height    = (f32)win_h,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    } );

    /* The scissor is a full-surface constant: clipping happens in the fragment against the clip
       table uploaded above, so nothing per-draw touches scissor state again. */
    rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){
        .x = 0, .y = 0, .width = win_w, .height = win_h } );
    ++s_render.state_scissors;

    // Bufferless: one pipeline, nothing bound but the bindless set.  Wireframe mode binds the
    // LINE twin (falling back to fill if it failed to compile); batch mode rasterizes filled
    // triangles under the per-draw tint below.
    rhi_pipeline_t pipe = ( s_render.debug_mode == GUI_RENDER_WIREFRAME
                            && rhi_handle_valid( s_render.pipeline_quad_wire ) )
                        ? s_render.pipeline_quad_wire : s_render.pipeline_quad;
    rhi()->cmd_bind_pipeline( cmd, pipe );
    rhi()->cmd_bind_bindless( cmd );

    // Ortho matrix: pixel [0,w]x[0,h] -> NDC [-1,+1]x[-1,+1].
    gui_push_t push;
    render_ortho( push.mvp, (f32)win_w, (f32)win_h );

    /* Both samplers travel every flush and the FRAGMENT picks between them per vertex, by sampling
       model.  Coverage is the one model that must stay point-sampled -- a filtered glyph atlas
       stops being crisp (see the motion-snap revert) -- and everything else filters.  Falls back to
       the point sampler if the bilinear one failed to create. */
    push.samp_point = s_render.font_sampler_idx;
    push.samp_image = s_render.image_sampler_idx ? s_render.image_sampler_idx
                                                 : s_render.font_sampler_idx;

    /* Debug render-mode push state.  dbg_flat makes the fragment bypass the atlas and emit a flat
       color: WIREFRAME keeps each window's vertex color (tint 0), BATCH overrides it per draw call
       with a palette color below.  NORMAL leaves both 0 for the textured/blended path. */
    bool batch_view = ( s_render.debug_mode == GUI_RENDER_BATCH );
    push.dbg_flat   = ( s_render.debug_mode == GUI_RENDER_NORMAL ) ? 0u : 1u;
    push.dbg_tint   = 0u;

    /* Frame-constant, so it is written once here and left alone through the whole dispatch walk:
       the effect band's clock costs no batch split and no per-draw work (gui.h, GUI_FX_TIME_WRAP). */
    push.time = s_render.fx_time;

    /* Fragment-clip plumbing (gui_fx.hlsli, "the clip band").  The buffer slot is flush-constant;
       clip_base is not -- quads bake SLOT-LOCAL entry indices, so the window's fixed slab origin
       goes out with the record base as the walk crosses a slot boundary (below).  Seeded to the
       region origin for the same reason prim_base is: the walk pushes before any draw of a slot,
       so an in-bounds seed keeps a never-read value from being an out-of-range read. */
    push.clip_buf  = s_render.clip_buf_idx;
    push.clip_base = clip_region * (u32)GUI_CLIP_REGION_MAX;

    /* Record plumbing.  The buffer slot is flush-constant like the clip one; prim_base is NOT --
       records are packed per window slot, so it is re-pushed from the tail as the walk crosses a
       slot boundary (below).  Seeded to the region ORIGIN rather than to a sentinel: the walk
       pushes before any draw of a slot so this value is never read, and an in-bounds seed keeps
       that a rendering question rather than an out-of-range storage-buffer read. */
    push.prim_buf  = s_render.prim_buf_idx;
    push.prim_base = clip_region * (u32)GUI_PRIM_REGION_MAX;

    /* Quad plumbing, both flush-constant: the quad table's slot and this region's origin (a
       draw's first_vertex carries the arena-absolute quad offset on top). */
    push.quad_buf  = s_render.quad_buf_idx;
    push.quad_base = clip_region * (u32)GUI_QUAD_REGION_MAX;

    /* Glyph table plumbing: the slot, and nothing else.  There is one table rather than a region
       per (frame, viewport), so an ID indexes it directly and there is no base to push. */
    push.glyph_buf = s_render.glyph_buf_idx;

    /* Push the whole struct once, before the walk. tex_idx/tex_mode/samp_idx live in the vertex,
       not the push constant, so everything left here -- both sampler slots, dbg_flat, the frame
       clock, the clip buffer slot and base -- is constant for the entire flush.

       One field moves mid-walk, via the tail re-push: dbg_tint changes every draw in the BATCH
       debug view so a colour shift marks a batch split.  Normal rendering pushes once per flush
       and nothing per draw. */
    const u8* tail = (const u8*)&push + GUI_PUSH_TAIL_OFF;
    rhi()->cmd_push_constants( cmd, &push, sizeof( push ), 0 );
    ++s_render.state_pushes;
    ++s_render.state_flushes;

    u32 draw_calls = 0;   // indexed draws actually emitted this surface (one per non-empty command)

    /* Walk s_dispatch[] (z-sorted slot pointers) back-to-front.  Each slot owns a contiguous
       region of s_tess.quads[]; its GPU commands reference it via their own arena-absolute
       vbase -- explicit rather than accumulated, since the arena may contain reserved headroom
       gaps between a volatile block's live quads and the commands that follow it.  Slots for
       other viewports are skipped entirely, as are commands with a mismatched vp (including a
       volatile block's dormant reserved commands, tagged GUI_VP_INVALID). */
    for ( u32 d = 0; d < s_dispatch_count; ++d )
    {
        const win_geo_slot_t* slot = s_dispatch[ d ];
        if ( slot->vp != vp_index )
            continue;

        /* The per-SLOT state: where this window's records start and where its clip slab does.
           Quads bake slot-local indices for both so they survive a repack, and these are the bases
           that put them back.  Filtered against the last values pushed -- a surface showing one
           window pushes once, and consecutive slots that happen to share both bases push nothing. */
        u32 slot_prim_base = clip_region * (u32)GUI_PRIM_REGION_MAX + slot->prim_base;
        u32 slot_clip_base = clip_region * (u32)GUI_CLIP_REGION_MAX
                           + slot->cache_idx * (u32)GUI_WIN_CLIP_MAX;
        if ( push.prim_base != slot_prim_base || push.clip_base != slot_clip_base )
        {
            push.prim_base = slot_prim_base;
            push.clip_base = slot_clip_base;
            rhi()->cmd_push_constants( cmd, tail, GUI_PUSH_TAIL_SIZE, GUI_PUSH_TAIL_OFF );
            ++s_render.state_pushes;
        }

        for ( u32 k = 0; k < slot->cmd_count; ++k )
        {
            u32                    ci = slot->cmd_base + k;
            const tess_gpu_cmd_t* gc = &s_tess.gpu_cmds[ ci ];
            const gui_gpu_cmd_t*  dc = &gc->cmd;

            if ( gc->vp != vp_index )
                continue;
            if ( dc->elem_count == 0 )
                continue;

            ++s_render.state_draws;

            /* The one piece of tail state that is genuinely per-draw, and only in the debug view
               that wants it: a distinct tint per draw call, so a colour change marks a batch split.
               Normal rendering never reaches this -- its tail went out per slot at most, above. */
            if ( batch_view )
            {
                push.dbg_tint = render_batch_debug_color( draw_calls );
                rhi()->cmd_push_constants( cmd, tail, GUI_PUSH_TAIL_SIZE, GUI_PUSH_TAIL_OFF );
                ++s_render.state_pushes;
            }

            /* elem_count is QUADS and vbase the arena-absolute first quad; the pull vertex stage
               divides VertexIndex (which includes first_vertex) back down. */
            rhi()->cmd_draw( cmd, &( rhi_draw_args_t ){
                .vertex_count   = dc->elem_count * 6u,
                .instance_count = 1,
                .first_vertex   = gc->vbase * 6u,
                .first_instance = 0,
            } );

            if ( slot->band == 0 )   /* debug-band draws never count in the stats they display */
                ++draw_calls;
        }
    }

    rhi()->cmd_end_rendering( cmd );

    // Fold this surface's draw-call count into the frame accumulator + lifetime peak (cache stats).
    cache_count_draw_calls( draw_calls );

    /* Pipeline-dashboard flush capture: what physically hit the GPU for this surface (frame
       region, upload spans, bytes/writes, draws).  A no-op unless GUI_PIPELINE_DASHBOARD. */
    DASH_CAPTURE_FLUSH( vp_index, frame, vtx_lo, vtx_hi,
                        up_bytes, up_batches, draw_calls );
}

// clang-format on
/*============================================================================================*/
