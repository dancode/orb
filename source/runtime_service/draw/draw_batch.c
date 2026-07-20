/*==============================================================================================

    runtime_service/draw/draw_batch.c -- Per-frame CPU->GPU vertex/index ring buffer.

    Creates two CPU_TO_GPU buffers (vertex + index) sized to the batch limits, with one
    independent region per frame-in-flight: the CPU records up to RHI_MAX_FRAMES_IN_FLIGHT
    frames ahead of the GPU, so rewriting a single region every frame would overwrite vertex
    data the GPU is still reading for the previous frame (visible as torn/twisted geometry
    whenever the data changes frame to frame -- e.g. a moving camera).  draw_batch_reset
    selects the frame's region; draw_batch_push writes geometry into it via
    rhi()->buffer_write and returns the region-relative base offsets needed to record the
    draw call (the flush binds the buffers at the region's byte offset).

==============================================================================================*/

typedef struct
{
    rhi_buffer_t vb;          /* CPU_TO_GPU vertex buffer (RHI_MAX_FRAMES_IN_FLIGHT regions) */
    rhi_buffer_t ib;          /* CPU_TO_GPU index buffer  (RHI_MAX_FRAMES_IN_FLIGHT regions) */
    u32          vb_base;     /* first vertex of this frame's region */
    u32          ib_base;     /* first index of this frame's region  */
    u32          vb_count;    /* vertices written this frame (region-relative) */
    u32          ib_count;    /* indices written this frame  (region-relative) */
    u32          cur_frame;   /* frame-in-flight index the counts belong to (see reset) */

} draw_batch_t;

/*==============================================================================================
    draw_batch_init  --  allocate persistent GPU-visible buffers
==============================================================================================*/

static bool
draw_batch_init( draw_batch_t* b )
{
    b->vb = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * DRAW_BATCH_MAX_VERTS * sizeof( draw_vertex_t ),
        .usage      = RHI_BUFFER_USAGE_VERTEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "draw_vb",
    } );
    if ( !rhi_handle_valid( b->vb ) )
        return false;

    b->ib = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * DRAW_BATCH_MAX_IDX * sizeof( u16 ),
        .usage      = RHI_BUFFER_USAGE_INDEX,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "draw_ib",
    } );
    if ( !rhi_handle_valid( b->ib ) )
    {
        rhi()->buffer_destroy( b->vb );
        return false;
    }

    b->cur_frame = 0xFFFFFFFFu;   /* sentinel: the first begin always resets the cursor */
    return true;
}

/*==============================================================================================
    draw_batch_begin_frame  --  select this frame's buffer region before any draw_xxx calls.
    `frame` is the frame-in-flight slot (rhi()->cmd_frame_index).

    The write cursor (vb_count / ib_count) is reset ONLY when `frame` differs from the region the
    counts currently belong to -- i.e. on a genuinely new frame.  Within a SINGLE frame the host
    may open the draw service more than once (render()->draw_scene draws the scene, then a later
    overlay pass draws an HUD over it); a second begin must APPEND after the first pass's geometry,
    not rewind to the region base and clobber vertex data the first pass's already-recorded draw
    calls will read when the command buffer executes.  Consecutive frames always land on different
    frame-in-flight slots (RHI_MAX_FRAMES_IN_FLIGHT >= 2), so a real new frame always resets.
==============================================================================================*/

static void
draw_batch_begin_frame( draw_batch_t* b, u32 frame )
{
    b->vb_base = frame * DRAW_BATCH_MAX_VERTS;
    b->ib_base = frame * DRAW_BATCH_MAX_IDX;

    if ( b->cur_frame != frame )
    {
        b->vb_count  = 0;
        b->ib_count  = 0;
        b->cur_frame = frame;
    }
}

/*==============================================================================================
    draw_batch_push  --  copy geometry into the GPU buffers; returns false if full

    On success, *out_base_vertex and *out_first_index are the offsets to pass into
    the draw call record (the geo indices are 0-relative; vertex_offset in the draw
    call adjusts them to the correct position in the buffer).
==============================================================================================*/

static bool
draw_batch_push( draw_batch_t* b,
                 const draw_vertex_t* verts, u32 nv,
                 const u16*           idxs,  u32 ni,
                 u32* out_base_vertex, u32* out_first_index )
{
    if ( b->vb_count + nv > DRAW_BATCH_MAX_VERTS ) return false;
    if ( b->ib_count + ni > DRAW_BATCH_MAX_IDX   ) return false;

    *out_base_vertex  = b->vb_count;
    *out_first_index  = b->ib_count;

    rhi()->buffer_write( b->vb, verts, nv * sizeof( draw_vertex_t ),
                         ( b->vb_base + b->vb_count ) * sizeof( draw_vertex_t ) );
    rhi()->buffer_write( b->ib, idxs, ni * sizeof( u16 ),
                         ( b->ib_base + b->ib_count ) * sizeof( u16 ) );

    b->vb_count += nv;
    b->ib_count += ni;
    return true;
}

/*==============================================================================================
    draw_batch_shutdown
==============================================================================================*/

static void
draw_batch_shutdown( draw_batch_t* b )
{
    if ( rhi_handle_valid( b->ib ) ) rhi()->buffer_destroy( b->ib );
    if ( rhi_handle_valid( b->vb ) ) rhi()->buffer_destroy( b->vb );
    *b = ( draw_batch_t ){ 0 };
}

/*============================================================================================*/