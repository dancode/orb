/*==============================================================================================

    runtime_service/draw/draw_cmd.c -- Immediate-mode draw call accumulation and flush.

    draw_begin caches the command list and view-projection matrix for the frame and
    resets the batch.  Each draw_xxx call generates geometry, writes it into the batch
    buffers, and records a lightweight draw_call_t -- or, when the geometry lands right after
    a call it shares state with, just widens that call's index range.  draw_end binds the
    shared vertex/index buffers once and emits the accumulated draw-indexed calls in
    submission order, re-binding the pipeline and re-pushing constants only where they change.

==============================================================================================*/
// clang-format off

typedef struct
{
    u32           first_index;    /* first index in the shared index buffer */
    u32           index_count;
    draw_mat_id_t material;
    u32           tex_idx;        /* DRAW_MAT_TEXTURED only: bindless texture slot */
    u32           samp_idx;       /* DRAW_MAT_TEXTURED only: bindless sampler slot */

} draw_call_t;

/* Module state -- zero-initialised at startup. */
static struct
{
    draw_batch_t    batch;
    draw_material_t mats[ DRAW_MAT_COUNT ];
    draw_call_t     calls[ DRAW_MAX_CALLS ];
    u32             call_count;
    rhi_cmd_t cmd;
    draw_push_t     frame_push;  /* current view-projection baked into every draw */
    draw_mat_id_t   cur_mat;

    /* Draw-owned bindless samplers (clamp-to-edge) for the textured path.  Textures belong
       to the caller; draw only supplies the sampler indices passed to image()/image_uv(). */
    rhi_sampler_t   samp_linear_h;
    rhi_sampler_t   samp_point_h;
    u32             samp_linear;   /* bindless index of samp_linear_h */
    u32             samp_point;    /* bindless index of samp_point_h  */

} s;

/*==============================================================================================
    draw_init / draw_shutdown
==============================================================================================*/

static void draw_shutdown( void );   /* forward decl: draw_init unwinds through it on failure */

static bool
draw_init( void )
{
    if ( !draw_batch_init( &s.batch )) {
         return false;
    }
    if ( !draw_material_init( s.mats )) {
         draw_batch_shutdown( &s.batch );
         return false;
    }

    /* Two draw-owned samplers, both clamp-to-edge; linear for images, point for pixel-exact
       sampling.  Registered in the bindless set so image() can reference them by index. */
    s.samp_linear_h = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_LINEAR, .mag_filter = RHI_FILTER_LINEAR,
        .mip_filter = RHI_FILTER_LINEAR,
        .address_u  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    s.samp_point_h = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_NEAREST, .mag_filter = RHI_FILTER_NEAREST,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( !rhi_handle_valid( s.samp_linear_h ) || !rhi_handle_valid( s.samp_point_h ) )
    {
        draw_shutdown();
        return false;
    }
    s.samp_linear = rhi()->register_sampler( s.samp_linear_h );
    s.samp_point  = rhi()->register_sampler( s.samp_point_h );

    s.cur_mat = DRAW_MAT_SOLID;
    return true;
}

static void
draw_shutdown( void )
{
    /* The last submitted frame may still be executing on the GPU.  Every destroy below is
       immediate (pipelines/samplers/buffers have no deferred-garbage path), so drain the
       device first -- destroying a pipeline a pending command buffer references is invalid. */
    rhi()->device_wait_idle();

    if ( rhi_handle_valid( s.samp_point_h ) )
    {
        rhi()->unregister_sampler( s.samp_point );
        rhi()->sampler_destroy( s.samp_point_h );
        s.samp_point_h = ( rhi_sampler_t ){ 0 };
    }
    if ( rhi_handle_valid( s.samp_linear_h ) )
    {
        rhi()->unregister_sampler( s.samp_linear );
        rhi()->sampler_destroy( s.samp_linear_h );
        s.samp_linear_h = ( rhi_sampler_t ){ 0 };
    }
    draw_material_shutdown( s.mats );
    draw_batch_shutdown( &s.batch );
}

/*==============================================================================================
    draw_begin / draw_end
==============================================================================================*/

static void
draw_begin_mat( rhi_cmd_t cmd, const f32 view_proj[ 16 ], draw_mat_id_t mat )
{
    s.cmd = cmd;
    memcpy( s.frame_push.mvp, view_proj, sizeof( s.frame_push.mvp ) );
    /* Select this frame-in-flight's buffer region so writes never touch data the GPU is
       still reading for a previous in-flight frame.  Within one frame this APPENDS: a second
       begin (e.g. an overlay pass after render()->draw_scene) keeps the first pass's geometry
       instead of rewinding over it -- see draw_batch_begin_frame. */
    draw_batch_begin_frame( &s.batch, rhi()->cmd_frame_index( cmd ) );
    s.call_count = 0;
    s.cur_mat    = mat;
}

/* 2D/overlay frame: no depth test; primitives paint in submission order (draws on top). */
static void
draw_begin( rhi_cmd_t cmd, const f32 view_proj[ 16 ] )
{
    draw_begin_mat( cmd, view_proj, DRAW_MAT_SOLID );
}

/* 3D frame: depth test + write.  The caller MUST have bound a DRAW_DEPTH_FORMAT depth
   attachment to the open render pass (cmd_begin_rendering), else the draws are invalid. */
static void
draw_begin_depth( rhi_cmd_t cmd, const f32 view_proj[ 16 ] )
{
    draw_begin_mat( cmd, view_proj, DRAW_MAT_SOLID_DEPTH );
}

static void
draw_end( void )
{
    if ( s.call_count == 0 )
        return;

    /* Bind this frame's buffer region once; all draws index into it with the
       region-relative first_index recorded at submit time. */
    rhi()->cmd_bind_vertex_buffer( s.cmd, s.batch.vb, s.batch.vb_base * sizeof( draw_vertex_t ) );
    rhi()->cmd_bind_index_buffer ( s.cmd, s.batch.ib, s.batch.ib_base * sizeof( u16 ),
                                   RHI_INDEX_TYPE_UINT16 );

    draw_mat_id_t cur      = DRAW_MAT_COUNT;    /* invalid sentinel to force the first bind */
    u32           cur_tex  = 0xFFFFFFFFu;       /* bindless pair currently in the push block */
    u32           cur_samp = 0xFFFFFFFFu;

    for ( u32 i = 0; i < s.call_count; ++i )
    {
        const draw_call_t* c = &s.calls[ i ];
        bool rebound = ( c->material != cur );

        if ( rebound )
        {
            cur = c->material;
            rhi()->cmd_bind_pipeline( s.cmd, s.mats[ cur ].pipeline );
            /* The bindless descriptor set must be re-bound after every pipeline switch (RHI
               contract).  The textured pipeline samples through it; harmless for the solid
               ones.  Binding it here also frees callers of draw()->begin from doing so. */
            rhi()->cmd_bind_bindless( s.cmd );
        }

        /* Push constants sized to the bound pipeline's layout: textured carries the bindless
           indices after the mvp, solid pushes the mvp alone.  The mvp is fixed for the whole
           begin/end pair, so the block only needs re-pushing when the pipeline switched (which
           discards it) or when a textured call brings a different bindless pair. */
        if ( c->material == DRAW_MAT_TEXTURED )
        {
            if ( rebound || c->tex_idx != cur_tex || c->samp_idx != cur_samp )
            {
                cur_tex  = c->tex_idx;
                cur_samp = c->samp_idx;

                draw_push_tex_t p;
                memcpy( p.mvp, s.frame_push.mvp, sizeof( p.mvp ) );
                p.tex_idx  = cur_tex;
                p.samp_idx = cur_samp;
                rhi()->cmd_push_constants( s.cmd, &p, sizeof( p ), 0 );
            }
        }
        else if ( rebound )
        {
            rhi()->cmd_push_constants( s.cmd, &s.frame_push, sizeof( s.frame_push ), 0 );
        }

        /* Indices are region-absolute (draw_batch_push), so no vertex offset applies. */
        rhi()->cmd_draw_indexed( s.cmd, &( rhi_draw_indexed_args_t ){
            .index_count    = c->index_count,
            .instance_count = 1,
            .first_index    = c->first_index,
            .vertex_offset  = 0,
            .first_instance = 0,
        } );
    }
}

/*==============================================================================================
    Internal helper: the call this submit can extend, or NULL if it needs a fresh one.

    A call absorbs new geometry when it shares every piece of GPU state with it and its index
    range ends exactly where the incoming geometry will start.  Consecutive submits are always
    adjacent in the index buffer, so in practice a run of same-material primitives collapses to
    a single draw call -- and, because merged geometry claims no slot, a run of any length fits
    inside DRAW_MAX_CALLS.
==============================================================================================*/

static draw_call_t*
draw_mergeable( draw_mat_id_t material, u32 tex_idx, u32 samp_idx )
{
    if ( s.call_count == 0 )
        return NULL;

    draw_call_t* p = &s.calls[ s.call_count - 1 ];
    if ( p->material != material )
        return NULL;
    if ( material == DRAW_MAT_TEXTURED && ( p->tex_idx != tex_idx || p->samp_idx != samp_idx ) )
        return NULL;
    if ( p->first_index + p->index_count != draw_batch_next_index( &s.batch ) )
        return NULL;

    return p;
}

/*==============================================================================================
    Internal helper: push geometry into the batch and record a draw call.
==============================================================================================*/

static void
draw_submit( const draw_vertex_t* verts, u32 nv, const u16* idxs, u32 ni )
{
    draw_call_t* prev = draw_mergeable( s.cur_mat, 0, 0 );
    if ( !prev && s.call_count >= DRAW_MAX_CALLS )
        return;

    u32 first_index;
    if ( !draw_batch_push( &s.batch, verts, nv, idxs, ni, &first_index ) )
        return;

    if ( prev )
    {
        prev->index_count += ni;
        return;
    }

    s.calls[ s.call_count++ ] = ( draw_call_t ){
        .first_index = first_index,
        .index_count = ni,
        .material    = s.cur_mat,
    };
}

/*==============================================================================================
    Internal helper: push textured geometry and record a DRAW_MAT_TEXTURED call.

    Independent of s.cur_mat (which tracks the begin/begin_depth solid material) -- image
    calls carry their own material and bindless indices so they can interleave with solid
    primitives in a single begin/end.
==============================================================================================*/

static void
draw_submit_tex( const draw_vertex_t* verts, u32 nv, const u16* idxs, u32 ni,
                 u32 tex_idx, u32 samp_idx )
{
    draw_call_t* prev = draw_mergeable( DRAW_MAT_TEXTURED, tex_idx, samp_idx );
    if ( !prev && s.call_count >= DRAW_MAX_CALLS )
        return;

    u32 first_index;
    if ( !draw_batch_push( &s.batch, verts, nv, idxs, ni, &first_index ) )
        return;

    if ( prev )
    {
        prev->index_count += ni;
        return;
    }

    s.calls[ s.call_count++ ] = ( draw_call_t ){
        .first_index = first_index,
        .index_count = ni,
        .material    = DRAW_MAT_TEXTURED,
        .tex_idx     = tex_idx,
        .samp_idx    = samp_idx,
    };
}

/*==============================================================================================
    Primitives  (static; only reachable externally through the draw_api_t vtable)
==============================================================================================*/

static void
draw_rect( f32 cx, f32 cy, f32 w, f32 h, const f32 rgba[ 4 ] )
{
    draw_vertex_t verts[ 4 ];
    u16           idxs[ 6 ];
    u32 nv = 0, ni = 0;
    geo_rect( verts, idxs, &nv, &ni, cx, cy, w * 0.5f, h * 0.5f, rgba );
    draw_submit( verts, nv, idxs, ni );
}

static void
draw_box( f32 cx, f32 cy, f32 cz, f32 w, f32 h, f32 d, const f32 rgba[ 4 ] )
{
    draw_vertex_t verts[ 8 ];
    u16           idxs[ 36 ];
    u32 nv = 0, ni = 0;
    geo_box( verts, idxs, &nv, &ni, cx, cy, cz, w * 0.5f, h * 0.5f, d * 0.5f, rgba );
    draw_submit( verts, nv, idxs, ni );
}

static void
draw_circle( f32 cx, f32 cy, f32 r, u32 segs, const f32 rgba[ 4 ] )
{
    draw_vertex_t verts[ DRAW_CIRCLE_MAX_SEGS + 1 ];
    u16           idxs[ DRAW_CIRCLE_MAX_SEGS * 3 ];
    u32 nv = 0, ni = 0;
    geo_circle( verts, idxs, &nv, &ni, cx, cy, r, segs, rgba );
    draw_submit( verts, nv, idxs, ni );
}

/*==============================================================================================
    Textured primitives.  tex_idx is a bindless slot from rhi()->register_texture; samp_idx
    is one of draw_sampler_linear()/draw_sampler_point().  tint modulates the sampled texel
    (1,1,1,1 = untinted).  Alpha-blended, so submission order matters.
==============================================================================================*/

static void
draw_image_uv( f32 cx, f32 cy, f32 w, f32 h,
               f32 u0, f32 v0, f32 u1, f32 v1,
               u32 tex_idx, u32 samp_idx, const f32 tint[ 4 ] )
{
    draw_vertex_t verts[ 4 ];
    u16           idxs[ 6 ];
    u32 nv = 0, ni = 0;
    geo_image( verts, idxs, &nv, &ni, cx, cy, w * 0.5f, h * 0.5f, u0, v0, u1, v1, tint );
    draw_submit_tex( verts, nv, idxs, ni, tex_idx, samp_idx );
}

static void
draw_image( f32 cx, f32 cy, f32 w, f32 h, u32 tex_idx, u32 samp_idx, const f32 tint[ 4 ] )
{
    draw_image_uv( cx, cy, w, h, 0.0f, 0.0f, 1.0f, 1.0f, tex_idx, samp_idx, tint );
}

static u32 draw_sampler_linear( void ) { return s.samp_linear; }
static u32 draw_sampler_point ( void ) { return s.samp_point;  }

/*============================================================================================*/
// clang-format on