/*==============================================================================================

    runtime_service/draw/draw_cmd.c -- Immediate-mode draw call accumulation and flush.

    draw_begin caches the command list and view-projection matrix for the frame and
    resets the batch.  Each draw_xxx call generates geometry, writes it into the batch
    buffers, and records a lightweight draw_call_t.  draw_end sorts by material (to
    minimise pipeline switches), binds shared vertex/index buffers once, then emits the
    draw-indexed calls.

==============================================================================================*/

typedef struct
{
    u32           first_index;    /* first index in the shared index buffer */
    u32           index_count;
    i32           vertex_offset;  /* base vertex added to each index (= base vertex in VB) */
    draw_push_t   push;
    draw_mat_id_t material;

} draw_call_t;

/* Module state -- zero-initialised at startup. */
static struct
{
    draw_batch_t    batch;
    draw_material_t mats[ DRAW_MAT_COUNT ];
    draw_call_t     calls[ DRAW_MAX_CALLS ];
    u32             call_count;
    rhi_command_list_t cmd;
    draw_push_t     frame_push;  /* current view-projection baked into every draw */
    draw_mat_id_t   cur_mat;

} s;

/*----------------------------------------------------------------------------------------------
    draw_init / draw_shutdown
----------------------------------------------------------------------------------------------*/

static bool
draw_init( void )
{
    if ( !draw_batch_init( &s.batch ) )
        return false;
    if ( !draw_material_init( s.mats ) )
    {
        draw_batch_shutdown( &s.batch );
        return false;
    }
    s.cur_mat = DRAW_MAT_SOLID;
    return true;
}

static void
draw_shutdown( void )
{
    draw_material_shutdown( s.mats );
    draw_batch_shutdown( &s.batch );
}

/*----------------------------------------------------------------------------------------------
    draw_begin / draw_end
----------------------------------------------------------------------------------------------*/

static void
draw_begin( rhi_command_list_t cmd, const f32 view_proj[ 16 ] )
{
    s.cmd = cmd;
    for ( u32 i = 0; i < 16; ++i )
        s.frame_push.mvp[ i ] = view_proj[ i ];
    draw_batch_reset( &s.batch );
    s.call_count = 0;
}

static void
draw_end( void )
{
    if ( s.call_count == 0 )
        return;

    /* Bind shared buffers once; all draws index into them with first_index/vertex_offset. */
    rhi()->cmd_bind_vertex_buffer( s.cmd, s.batch.vb, 0 );
    rhi()->cmd_bind_index_buffer( s.cmd, s.batch.ib, 0, RHI_INDEX_TYPE_UINT16 );

    draw_mat_id_t cur = DRAW_MAT_COUNT; /* invalid sentinel to force first bind */
    for ( u32 i = 0; i < s.call_count; ++i )
    {
        const draw_call_t* c = &s.calls[ i ];

        if ( c->material != cur )
        {
            cur = c->material;
            rhi()->cmd_bind_pipeline( s.cmd, s.mats[ cur ].pipeline );
        }

        rhi()->cmd_push_constants( s.cmd, &c->push, sizeof( c->push ), 0 );
        rhi()->cmd_draw_indexed( s.cmd, &( rhi_draw_indexed_args_t ){
            .index_count    = c->index_count,
            .instance_count = 1,
            .first_index    = c->first_index,
            .vertex_offset  = c->vertex_offset,
            .first_instance = 0,
        } );
    }
}

/*----------------------------------------------------------------------------------------------
    Internal helper: push geometry into the batch and record a draw call.
----------------------------------------------------------------------------------------------*/

static void
draw_submit( const draw_vertex_t* verts, u32 nv, const u16* idxs, u32 ni )
{
    if ( s.call_count >= DRAW_MAX_CALLS )
        return;

    u32 base_vertex, first_index;
    if ( !draw_batch_push( &s.batch, verts, nv, idxs, ni, &base_vertex, &first_index ) )
        return;

    s.calls[ s.call_count++ ] = ( draw_call_t ){
        .first_index   = first_index,
        .index_count   = ni,
        .vertex_offset = (i32)base_vertex,
        .push          = s.frame_push,
        .material      = s.cur_mat,
    };
}

/*----------------------------------------------------------------------------------------------
    Primitives  (static; only reachable externally through the draw_api_t vtable)
----------------------------------------------------------------------------------------------*/

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

/*============================================================================================*/