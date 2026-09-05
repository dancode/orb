/*==============================================================================================

    gui/render/pipeline/gui_render_init.c -- Shared GPU resources

    Sets up the GPU resources that every surface shares for the whole run: the compiled
    pipeline (plus its wireframe twin), the two bindless samplers --

            1. font/coverage
            2. image

    -- and the push-constant layout they're built against.

    render_init creates all of this once; render_shutdown tears it down and logs the run's
    peak pool usage, draw-call count, and per-draw state cost.

    The GUI pipeline runs in three stages:

        EMIT    gui_emit_*.c       widgets -> a list of draw commands (s_draw)
        BUILD   gui_build_cache.c  diff + tessellate -> geometry (s_tess) + a slot table (s_dispatch)
        RENDER  this file          the shared GPU resources set up once here, then submitted per surface

    No surface owns its own geometry buffers. The actual per-surface work -- uploading a
    surface's slots into these shared tables and issuing its draw calls -- happens next door in
    pipeline/gui_render_submit.c, which only reads s_render (this file's static state) and
    never writes to it.

==============================================================================================*/

#include "runtime_service/gui/gui_res.h"   // the cooked .oshd pair, read by resource name through fs

// clang-format off
/*==============================================================================================
    Push constant layout -- must match gui_pc_t in shaders/gui_common.hlsli.ord

    The cooked shader container records the block's SIZE, and pipeline_create checks this
    struct against that. Field ORDER isn't checked automatically, though -- keeping this
    struct and the .hlsli struct in the same order is up to whoever edits either one.
==============================================================================================*/

/* Note: in the future we may want to compress some values to make space to 16 bit values.
   (samp_point, samp_image, clip_buf, prim_buf, quad_buf, glyph_buf, tex_cov, tex_sdf)
   are good candidates */

typedef struct
{
    f32 mvp[ 16 ];      // column-major ortho matrix           64 bytes
    u32 samp_point;     // bindless sampler: NEAREST            4 bytes
    u32 samp_image;     // bindless sampler: LINEAR             4 bytes
    u32 dbg_flat;       // debug: 1 = flat color (no atlas)     4 bytes
    u32 dbg_tint;       // debug: packed RGBA8 batch tint       4 bytes
    f32 time;           // frame clock, wrapped seconds         4 bytes
    u32 clip_buf;       // bindless slot: frame clip table      4 bytes (0 = no table, no clipping)
    u32 clip_base;      // draw's first clip-table entry        4 bytes
    u32 prim_buf;       // bindless slot: prim records          4 bytes (0 = no records bound)
    u32 prim_base;      // the SLOT's first record              4 bytes
    u32 pal_base;       // this frame's palette block           4 bytes
    u32 quad_buf;       // bindless slot: quad-record table     4 bytes
    u32 quad_base;      // flush's region origin                4 bytes (quads, not float4s)
    u32 glyph_buf;      // bindless slot: glyph UV table        4 bytes (0 = no table bound)
                        // no base: the table is not regioned, 
                        // so an ID indexes it directly
    u32 tex_cov;        // bindless slot: coverage atlas        4 bytes
    u32 tex_sdf;        // bindless slot: SDF atlas             4 bytes

} gui_push_t;           // total 124 bytes / 128 (just within RHI_MAX_PUSH_CONST_SIZE)

/*  Push Constant Notes:

    - This struct holds per-frame state for one viewport's render; a few fields get updated
      again mid-frame, per window (the "tail", explained below).
    - mvp comes first and is set once per viewport, holding its screen pixel dimensions.
    - The texture and its filter mode used to live directly in this struct, which forced a
      separate draw call per texture. They now live on the STYLE RECORD each quad points to,
      and the fragment shader reads the sampler slot from there instead.
    - That record picks which of the two SAMPLERS above to use, via a mode flag packed into it.
    - A glyph quad doesn't point to a style record at all -- its atlas comes straight from
      tex_cov / tex_sdf above.
    - Anything project-specific goes at the tail (the end of this struct).
    - The whole struct is pushed to the GPU ONCE per flush, before walking the draw list. A few
      fields then get re-pushed on their own partway through, because they change more often
      than "once per flush":

        The prim_base and clip_base change per WINDOW SLOT, because a window's records sit
        wherever the arena placed them rather than at a fixed offset; and the BATCH debug 
        view's tint changes per draw call on purpose (see gui_render_flush).

    - All three of those re-pushes go through the tail, and each is skipped if its value hasn't
      changed since the last push -- so a normal frame with just one window still only pushes
      the whole struct exactly once.
    - Vulkan leaves push constants with undefined content until something writes them. The head
      is written right after this flush's cmd_bind_pipeline, so even if an earlier scene pass
      bound a different pipeline first, no stale matrix can leak through.
*/
/*==============================================================================================
    Push Constant Tail Split -- marks where the "changes per window slot" part of the push
    constant struct begins, so that part can be re-pushed on its own (see the notes above).
==============================================================================================*/

#define GUI_PUSH_TAIL_OFF   ( (u32)offsetof( gui_push_t, samp_point ) )
#define GUI_PUSH_TAIL_SIZE  ( (u32)( sizeof( gui_push_t ) - offsetof( gui_push_t, samp_point )))

ORB_STATIC_ASSERT( offsetof( gui_push_t, mvp ) == 0,
                   "the mvp must lead gui_push_t -- the per-draw push writes everything after it" );
ORB_STATIC_ASSERT( GUI_PUSH_TAIL_OFF == sizeof( ( (gui_push_t*)0 )->mvp ),
                   "gui_push_t tail must start immediately after the mvp" );
ORB_STATIC_ASSERT( GUI_PUSH_TAIL_OFF % 4 == 0 && GUI_PUSH_TAIL_SIZE % 4 == 0,
                   "vkCmdPushConstants requires 4-byte aligned offset and size" );

/*==============================================================================================
    Clip Table Sizing -- the storage buffer clip_coverage reads from (gui_fx.hlsli).

    An ENTRY is two float4s: pixel edges (x0, y0, x1, y1) snapped to whole pixels, plus
    (radius, feather, flags, value).

    A REGION is a fixed array of per-window SLABS -- one per stable cache slot, each holding
    GUI_WIN_CLIP_MAX entries, at offset cache_idx * GUI_WIN_CLIP_MAX (clip_base). A window's
    quads bake in that same base when they're built, so uploads always land at a fixed,
    predictable offset and can never overflow into another window's slab.

    There's one region per frame-in-flight -- and that's it, NOT one per viewport too. That's
    because a window's cache slot is unique across the whole app (cache_idx comes from one
    single global table, RENDER_MAX_WIN), so its slab means the same thing no matter which
    surface is drawing it. Compare that to the prim table's per-viewport regions and the quad
    table's per-viewport claims below, which each hold geometry specific to one surface.
==============================================================================================*/

#define GUI_CLIP_ENTRY_FLOATS  8u
#define GUI_CLIP_ENTRY_BYTES   ( GUI_CLIP_ENTRY_FLOATS * 4u )
#define GUI_CLIP_REGION_MAX    ( RENDER_MAX_WIN * GUI_WIN_CLIP_MAX )     /* entries per region */
#define GUI_CLIP_REGION_BYTES  ( GUI_CLIP_REGION_MAX * GUI_CLIP_ENTRY_BYTES )
#define GUI_CLIP_REGION_COUNT  ( RHI_MAX_FRAMES_IN_FLIGHT )

/*==============================================================================================
    Claim Space -- one viewport's slice of a CLAIM-sized table, in records.

    Both the prim table and the quad table below are sized by claim rather than by a fixed
    per-viewport slab (see each section's sizing notes). This is the shape they share; the
    growth logic that allocates and compacts it, claim_space_reserve, lives further down where
    it's used by both tables' create/reserve functions.
==============================================================================================*/

typedef struct
{
    u32 base;       // claim start within one frame-in-flight copy, in records (bucket-aligned)
    u32 alloc;      // claim size in records (bucket multiple); 0 = viewport holds no claim

} gui_claim_t;

/*==============================================================================================
    Primitive Palette -- a storage buffer of shape records. Every quad's fragment shader
    reads one of these records to know what shape and style to draw.

    The buffer has two parts, back to back:

      1. A FIXED HEADER at the front, laid out as:

             [ style palette blocks x FIF | overlay records x (FIF x viewports) | claim copies x FIF ]

         This part never moves, even as the claim space below it grows.

      2. The CLAIM SPACE, sized the same way as the quad table (see below): it grows as
         windows need more room, and once grown it never shrinks.

    Where a window's records live, and why:

    A window's own records are NOT laid out in one tidy block per window. They live wherever
    the arena happened to place that window (win_geo_slot_t.prim_base), so uploading them means
    writing to a different offset every flush. This is different from the clip table, which
    always sits at a fixed, predictable offset.

    STYLE PALETTE (see GUI_PAL_FIRST in gui.h): every window, on every surface, shares the same
    set of style records -- unlike a window's own claim records, which only that window can see.
    There is one copy of the whole palette PER FRAME-IN-FLIGHT, not just one copy total. That's
    because when a style gets re-baked, older frames might still be mid-flight and reading the
    old table; writing into a separate per-frame block guarantees no in-flight draw ever reads a
    half-written palette. When the buffer is swapped out, render_pal_invalidate (in
    gui_render_pal.c) marks every frame's block dirty so the next flush re-uploads it fresh from
    the CPU-side table.

    OVERLAY RECORDS: the debug overlay draws its own quads outside the normal tessellator path,
    so it has no window to borrow a record from. It still needs to name a texture (which atlas
    slot to sample), and that slot is only known once the frame actually flushes -- so it can't
    just reuse an existing style record ahead of time. Instead it gets its own record per
    (frame-in-flight, viewport) pair, rewritten every flush by dbg_flush.

    Note: every offset/base value in this section counts in RECORDS, not float4s.
==============================================================================================*/

#define GUI_PAL_REGION_COUNT       RHI_MAX_FRAMES_IN_FLIGHT
#define GUI_PAL_REGION_BYTES     ( GUI_PAL_MAX * GUI_PRIM_BYTES )

#define GUI_PRIM_OVERLAY_ORIGIN  ( GUI_PAL_REGION_COUNT * GUI_PAL_MAX )     /* in records */
#define GUI_PRIM_HDR_RECORDS     ( GUI_PRIM_OVERLAY_ORIGIN \
                                 + RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS )

#define GUI_PRIM_BUCKET        32u    // claim granularity, records: 4 KB steps absorb jitter
#define GUI_PRIM_GPU_BOOT_CAP  512u   // boot capacity, records per frame-in-flight copy

/* Tracks what one (frame-in-flight, viewport) slice of the claim space last uploaded.
   Checked at every flush. Unlike the quad regions below, this has no geometry-generation field
   -- content changes are tracked separately, through per-slot pending bits (s_prim_range_pending).
   This struct only needs to catch when the mapping itself has moved; any mismatch here means
   every slot for that surface gets re-uploaded. */

typedef struct      // A viewport's last-uploaded region of the primitive table, in records.
{
    u32 anchor;     // arena record index mapped to the claim's first record
    u32 base;       // claim base the uploads targeted
    u32 buf_gen;    // buffer generation the uploads landed in

} gui_prim_region_t;

static struct
{
    u32               capacity;  // records per frame-in-flight copy (header excluded)
    u32               tail;      // first free record past every claim (bump; compacts on swap)
    u32               buf_gen;   // bumped per buffer swap; stale regions re-upload on next flush

    gui_claim_t       claim [ GUI_MAX_VIEWPORTS ];
    gui_prim_region_t region[ RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS ];

} s_prim_gpu;

/*==============================================================================================
    Quad table sizing -- the geometry table (gui.h, gui_quad_t). Unlike the tables above, 
    this one is NOT sized by a fixed cap -- it's sized by CLAIM.

    The tables above carve out fixed-size regions, but the quad table is the big one: giving
    every (frame-in-flight, viewport) pair a fixed slab of GUI_MAX_QUADS would mean paying the
    worst-case size for every viewport, even ones that are closed or nearly empty. Instead, 
    each viewport holds a CLAIM -- a contiguous run of the table, rounded to bucket-sized 
    chunks, sized to whatever its own windows actually need. The buffer holds one copy of 
    this whole claim space per frame-in-flight.

    Different viewports never need extra synchronization to write safely alongside each other:
    claims never overlap and never move while in use, so each surface only ever touches its own
    claim. (The usual frame-in-flight hazard -- reading a buffer while it's being overwritten --
    is still handled the same way as the fixed regions above: one copy per frame-in-flight.)
    When a viewport's claim needs to grow, it takes fresh space at the tail of the buffer and
    leaves its old space unused. Once the tail runs out of room, the whole buffer gets REPLACED
    and every claim is compacted into the new one (same pattern as the glyph table below): any
    frame still in flight keeps reading the old buffer through the bindless slot its push
    constant already captured, and rhi's deferred-destroy logic holds onto that old buffer until
    those frames are done with it.

    Mapping from the CPU-side geometry arena to a GPU claim is recomputed every flush: a
    viewport's windows cover some range of arena records [anchor, hi) -- the union of what they
    reserved, gaps included (gap bytes are simply never uploaded or read). Arena record `a` maps
    to claim.base + (a - anchor), and pc.quad_base carries that whole offset so the shader
    indexes it exactly as before. If the anchor moves, the claim moves, or the buffer gets
    swapped, the region's uploaded bytes no longer line up with the new mapping -- in any of
    those cases the flush just re-uploads the whole span, the same fallback used when a
    geometry-generation bump forces a re-upload.
==============================================================================================*/

#define GUI_QUAD_BUCKET        256u    /* claim granularity, records: 4 KB steps absorb jitter */
#define GUI_QUAD_GPU_BOOT_CAP  8192u   /* boot capacity, records per frame-in-flight copy */

/* Tracks what one (frame-in-flight, viewport) region of the table last uploaded; checked at
   every flush. Any mismatch here means the region's bytes can't just be patched -- the whole
   span gets re-uploaded from scratch. */

typedef struct
{
    u32 gen;      // geometry generation (s_geo_gen) the region was last uploaded with
    u32 anchor;   // arena record index mapped to the claim's first record
    u32 base;     // claim base the upload targeted
    u32 buf_gen;  // buffer generation the upload landed in

} gui_quad_region_t;

static struct
{
    u32               capacity;  // records per frame-in-flight copy
    u32               tail;      // first free record past every claim (bump; compacts on swap)
    u32               buf_gen;   // bumped per buffer swap; stale regions re-upload on next flush

    gui_claim_t       claim [ GUI_MAX_VIEWPORTS ];
    gui_quad_region_t region[ RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS ];

} s_quad_gpu;

/*==============================================================================================
    Glyph UV table sizing -- the ID-indexed atlas rects (gui.h, gui_glyph_uv_t).

    This table is NOT split into per-frame regions, unlike the three tables above. Those get
    rewritten every single frame, so each (frame-in-flight, viewport) pair needs its own place
    to land safely. This table only gets written when its generation changes -- a font's pixels
    entering the atlas, or a repack moving a page -- which happens at boot and then almost
    never, and its contents are the same for every surface anyway.

    So instead of keeping N copies around to write into, there's just ONE buffer, replaced
    wholesale on the rare occasions it needs to change: render_glyph_buf_refresh builds a fresh
    buffer and hands the old one to rhi's deferred-destroy, which holds onto it until no frame
    in flight could still be reading it. Nothing is ever overwritten in place, so there's no
    race for an in-flight draw to lose.

    The buffer is sized by glyph_table_used(), not by the full space of possible IDs. An ID is
    (font slot x stride), so the buffer only needs to reach as far as the highest slot actually
    in use -- one loaded font costs 4 KB, where reserving room for the full 16-slot space would
    cost 64 KB. used() only ever grows when a rebuild happens, and a rebuild is exactly what
    bumps the generation -- so the same "replace on generation change" logic above also handles
    growth, with no separate code path needed.
==============================================================================================*/

/* see: gui_glyph_uv_t in gui.h for the struct layout, and 
   gui_glyph_uv.c for the CPU-side table that backs this GPU buffer. */

/*==============================================================================================
    Shared GPU resources -- created once in render_init, destroyed in render_shutdown.

    None of this changes frame to frame, and it's shared by every viewport (and the debug
    overlay too), so it's never a per-viewport or per-frame bottleneck.

    No surface owns its own geometry buffers. The quad, style, and clip tables below are each a
    single buffer, carved into one region per (frame-in-flight, viewport). A viewport is just a
    render TARGET that windows get dispatched to -- it doesn't own the windows themselves. The
    one shared context emits every window, and the flush step routes each window's geometry to
    whichever viewport is hosting it.

    The actual list of viewports lives in the bound context (core/gui_ctx.c). This file never
    looks at that list directly -- it only ever touches a surface through the GPU handles it's
    handed.
==============================================================================================*/

static struct
{
    rhi_pipeline_t  pipeline_quad;      // THE pipeline: bufferless gui_quad shaders + alpha blend
    rhi_pipeline_t  pipeline_quad_wire; // same in VK_POLYGON_MODE_LINE (wireframe debug view)
    rhi_sampler_t   font_sampler;       // coverage sampler: point, U repeats (dash tiling), V clamps
    u32             font_sampler_idx;   // bindless slot for font_sampler

    /* The second sampler: bilinear, used for anything that should be filtered rather than
       sampled crisp -- authored sprite art, a caller's own textures (like a scene render
       target), and distance-field glyphs. BOTH sampler slots are pushed every flush, and the
       FRAGMENT shader picks between them itself, based on the sampling mode each quad's style
       record carries. That's what lets one batch mix a point-sampled glyph with a filtered
       image without needing separate draw calls. The rule for which mode a quad gets: glyphs
       and icons (coverage) must stay point-sampled to look crisp; everything else -- a picture
       or a distance field -- gets filtered. */

    rhi_sampler_t   image_sampler;      // sampler for RGBA images (bilinear clamp)
    u32             image_sampler_idx;  // bindless slot for image_sampler

    /* The frame clip table -- the storage buffer clip_coverage reads from (gui_fx.hlsli). It
       has one region per (frame-in-flight, viewport): each flush writes that surface's clip
       entries into its own region only, so one surface's upload can never overwrite entries
       another surface's in-flight draws are still reading. Registered bindless once at init;
       the slot index rides along in pc.clip_buf on every flush. */

    rhi_buffer_t    clip_buf;           // storage buffer: all clip regions
    u32             clip_buf_idx;       // bindless buffer slot (never 0 after a successful init)

    /* The prim record table (gui.h, gui_prim_t): sized by claim, replaced wholesale when it
       needs to grow (see s_prim_gpu above), and written per window slot on each flush. The
       bindless slot index rides in pc.prim_buf; that window's own record offset rides in
       pc.prim_base. */

    rhi_buffer_t    prim_buf;           // storage buffer: header + frame-in-flight claim copies
    u32             prim_buf_idx;       // bindless buffer slot (never 0 after a successful init)

    /* The quad-record geometry table: sized by claim, replaced wholesale when it needs to grow
       (see s_quad_gpu above). */

    rhi_buffer_t    quad_buf;           // storage buffer: frame-in-flight copies of the claim space
    u32             quad_buf_idx;       // bindless buffer slot (never 0 after a successful init)

    /* The glyph UV table: just ONE buffer, replaced wholesale when its generation changes,
       rather than split into per-frame regions like the tables above (see the sizing note
       above for why). The slot index rides in pc.glyph_buf; there's no base offset to push,
       since there's only ever the one table. */

    rhi_buffer_t    glyph_buf;          // storage buffer: the glyph table through its used extent
    u32             glyph_buf_idx;      // bindless buffer slot (never 0 after a successful init)
    u32             glyph_buf_gen;      // table generation the buffer currently holds
    u32             glyph_buf_bytes;    // the size it was created at (glyph_table_used then)

    gui_render_mode_t debug_mode;       // NORMAL / WIREFRAME / BATCH -- how the UI list is rasterized

    /* The frame clock, handed down by the orchestrator (gui_render_set_time) already wrapped to
       a repeating range. It's stored here, rather than read straight from the IO snapshot,
       because s_io lives in the frontend unit and this backend code can't see it -- the same
       one-way boundary gui_render_set_mode crosses. */

    f32 fx_time;                        // seconds since the first frame, wrapped; -> pc.time

    /* Running totals of per-draw GPU state changes, accumulated by gui_render_flush and
       reported at shutdown. The scissor filter never shows up visually -- it changes no
       pixel -- so without counting it there'd be no way to tell whether it's actually saving
       anything on a real UI. The push-count is no longer measuring savings; it's a cost figure:
       the tail push now happens per-FLUSH, so state_flushes is the number to divide it by. */

    u64 state_draws;                  // draw calls walked (the scissor filter's denominator)
    u64 state_flushes;                // surface flushes walked (the push denominator)
    u64 state_pushes;                 // push-constant writes issued (1 whole block per flush + tail re-pushes)
    u64 state_scissors;               // scissor sets actually issued

} s_render;

/*==============================================================================================
    Init / shutdown for the shared GPU resources (pipeline, font sampler, atlas).
==============================================================================================*/

/*==============================================================================================
    render_load_shaders -- loads the cooked shader pair, the only way the gui gets a pipeline.

    The shaders are the resources "shader/gui_quad.vs" and "shader/gui_quad.ps": content/shader/
    gui_quad.{vs,ps}.hlsl, marked with RID() below so the build resolves them and cooks them to
    build/content/shader/gui_quad.{vs,ps}.oshd, the cooked mirror the host mounts above content/.
    That makes the .hlsl files the single source of truth for what the GPU runs -- there's no
    embedded SPIR-V array and no separate GLSL copy that could drift out of sync with them.  The
    cooked containers also carry shader reflection data, which is what lets pipeline_create check
    the push-constant range below against what the actual compiled shader expects.

    Loading is all-or-nothing: a shader no mount serves is a hard failure that names it, so an
    out-of-date build or a host that forgot to mount build/content shows up immediately at init
    instead of silently drawing a frame from stale bytes.
==============================================================================================*/

static rhi_shader_t
render_load_shader( const char* res, const char* debug_name )
{
    fs_blob_t blob = gui_res_read( res, ".oshd" );
    if ( !blob.ok )
    {
        gui_log( GUI_LOG_ERROR, "gui shader '%s' not found -- no %s.oshd in the content mounts "
                                "(run build_tool -content to cook it; the host mounts build/content)",
                 res, res );
        return ( rhi_shader_t ){ RHI_NULL_HANDLE };
    }
    rhi_shader_t sh = rhi()->shader_load_oshd_memory( blob.data, blob.size, debug_name );
    fs()->free( &blob );
    return sh;
}

static bool
render_load_shaders( rhi_shader_t* out_vert, rhi_shader_t* out_frag )
{
    rhi_shader_t vert = render_load_shader( RID( "shader/gui_quad.vs" ), "gui_quad_vert(oshd)" );
    if ( !rhi_handle_valid( vert ) )
        return false;

    rhi_shader_t frag = render_load_shader( RID( "shader/gui_quad.ps" ), "gui_quad_frag(oshd)" );
    if ( !rhi_handle_valid( frag ) )
    {
        rhi()->shader_destroy( vert );
        return false;
    }

    *out_vert = vert;
    *out_frag = frag;
    return true;
}

static void render_shutdown( void );   /* the clip-table failure path below unwinds through it */

/*==============================================================================================
    The glyph UV table's buffer -- created once, then REPLACED (never rewritten in place) when
    it needs to change.

    render_glyph_buf_create allocates the buffer, registers it bindless, and uploads whatever
    the table currently holds. render_glyph_buf_refresh is the per-flush check: whenever the
    table's generation has moved on, it builds a brand new buffer and retires the old handle and
    slot. rhi holds onto both until no frame in flight could still be reading them
    (vk_garbage_push / vk_retire_safe_at).

    Replacing the buffer, instead of overwriting it in place, is exactly what lets this get away
    with just ONE buffer. The alternative -- a separate copy per frame in flight, like the three
    per-frame tables use -- would mean paying 3x the memory for a table that's identical every
    frame, just to guard against a write hazard that only exists if you write in place at all.
==============================================================================================*/

static bool
render_glyph_buf_create( void )
{
    u32 bytes = glyph_table_used() * (u32)sizeof( gui_glyph_uv_t );

    s_render.glyph_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = bytes,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_glyph_table",
    } );
    if ( !rhi_handle_valid( s_render.glyph_buf ) )
        return false;

    s_render.glyph_buf_idx = rhi()->register_buffer( s_render.glyph_buf );
    if ( s_render.glyph_buf_idx == 0 )
    {
        /* Clean up after ourselves, same rule the quad/prim creators follow: on failure the
           refresh path below restores the OLD handle into this member, so leaving an
           unregistered buffer here would leak it. */
        rhi()->buffer_destroy( s_render.glyph_buf );
        s_render.glyph_buf = ( rhi_buffer_t ){ RHI_NULL_HANDLE };
        return false;
    }

    rhi()->buffer_write( s_render.glyph_buf, glyph_table_data(), bytes, 0 );
    s_render.glyph_buf_bytes = bytes;
    s_render.glyph_buf_gen   = glyph_table_generation();
    return true;
}

static void
render_glyph_buf_refresh( void )
{
    u32 gen = glyph_table_generation();
    if ( s_render.glyph_buf_gen == gen )
        return;

    rhi_buffer_t old_buf = s_render.glyph_buf;
    u32          old_idx = s_render.glyph_buf_idx;

    if ( !render_glyph_buf_create() )
    {
        /* Keep drawing from the buffer we already have. Its rects being one generation stale
           just means an occasional wrong glyph rectangle -- far better than falling back to a
           zeroed bindless slot, which would make every character disappear. */
        s_render.glyph_buf     = old_buf;
        s_render.glyph_buf_idx = old_idx;
        GUI_WARN_ONCE( "glyph table rebuild could not allocate -- holding generation %u\n",
                       s_render.glyph_buf_gen );
        return;
    }

    if ( old_idx )
        rhi()->unregister_buffer( old_idx );
    if ( rhi_handle_valid( old_buf ) )
        rhi()->buffer_destroy( old_buf );
}

/*==============================================================================================
    claim_space_reserve -- shared growth logic for the quad and prim tables' claim spaces.

    Give a viewport a claim of at least `need` records. On a typical frame the claim is already
    big enough, and this returns immediately. When it needs to grow, it allocates a FRESH run at
    the buffer's tail (sized with some geometric headroom, rounded up to `bucket`) -- the old run
    just goes unused, but its bytes stay valid for as long as any frame in flight might still be
    reading them. If the tail doesn't have room for the new run, the whole buffer gets replaced
    (via `create_fn`, which stores the new capacity into `*p_capacity` itself) and every claim is
    compacted into the new one; any frame still in flight keeps reading the old buffer through
    the bindless slot it already captured -- `*p_buf`/`*p_idx` are only swapped forward once
    `create_fn` succeeds, and unregistered/destroyed after the compaction is safely recorded.

    If allocation fails outright, the old (too-small) claim is returned unchanged -- the caller
    is expected to clamp its upload to fit within that claim rather than write past it, so the
    failure shows up as truncated geometry rather than memory corruption. `what` names the table
    in the resulting log/warn lines. */

static gui_claim_t*
claim_space_reserve( gui_claim_t* claim, u32* p_tail, u32* p_capacity, u32 bucket,
                      u32 hdr_records, u32 bytes_per_record, i32 vp, u32 need,
                      rhi_buffer_t* p_buf, u32* p_idx, bool (*create_fn)( u32 capacity ),
                      const char* what )
{
    gui_claim_t* c = &claim[ vp ];
    if ( need <= c->alloc )
        return c;

    u32 alloc = need + need / 4u;
    alloc = ( alloc + bucket - 1u ) / bucket * bucket;

    if ( *p_tail + alloc <= *p_capacity )
    {
        c->base   = *p_tail;
        c->alloc  = alloc;
        *p_tail  += alloc;
        return c;
    }

    /* Replace the buffer: grow capacity to at least double, or enough to fit every claim
       compacted together if that needs more room. */
    u32 total = alloc;
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
        if ( (i32)v != vp )
            total += claim[ v ].alloc;

    u32 capacity = *p_capacity * 2u;
    if ( capacity < total )
        capacity = total;

    rhi_buffer_t old_buf = *p_buf;
    u32          old_idx = *p_idx;

    if ( !create_fn( capacity ) )
    {
        *p_buf = old_buf;
        *p_idx = old_idx;
        GUI_WARN_ONCE( "gui %s table could not grow to %u records -- rendering may degrade\n",
                       what, capacity );
        return c;
    }

    /* Compact every claim into the fresh buffer here -- but only their offsets, not their
       bytes. Each region tracks the buffer generation it last uploaded into (buf_gen), so once
       this generation bumps, every surface's next flush notices the mismatch and re-uploads its
       span at the new position on its own. */
    *p_tail = 0;
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
    {
        gui_claim_t* cv = &claim[ v ];
        if ( (i32)v == vp )
            cv->alloc = alloc;
        if ( cv->alloc == 0 )
            continue;
        cv->base  = *p_tail;
        *p_tail  += cv->alloc;
    }

    gui_log( GUI_LOG_INFO, "gui %s table grew: %u records per frame copy (%u KB total)",
             what, capacity, ( hdr_records + RHI_MAX_FRAMES_IN_FLIGHT * capacity )
                                 * bytes_per_record / 1024u );

    if ( old_idx )
        rhi()->unregister_buffer( old_idx );
    if ( rhi_handle_valid( old_buf ) )
        rhi()->buffer_destroy( old_buf );
    return c;
}

/*==============================================================================================
    The quad table's buffer -- created at a boot-time capacity, then REPLACED (never resized in
    place) once the claims outgrow it. See the claim scheme in the sizing block above.
==============================================================================================*/

static bool
quad_gpu_create( u32 capacity )
{
    rhi_buffer_t buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = RHI_MAX_FRAMES_IN_FLIGHT * capacity * (u32)GUI_QUAD_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_quad_table",
    } );
    if ( !rhi_handle_valid( buf ) )
        return false;

    u32 idx = rhi()->register_buffer( buf );
    if ( idx == 0 )
    {
        rhi()->buffer_destroy( buf );
        return false;
    }

    s_render.quad_buf     = buf;
    s_render.quad_buf_idx = idx;
    s_quad_gpu.capacity   = capacity;
    ++s_quad_gpu.buf_gen;   /* every region's bytes are gone: re-upload on next touch */
    return true;
}

static gui_claim_t*
quad_gpu_reserve( i32 vp, u32 need )
{
    return claim_space_reserve( s_quad_gpu.claim, &s_quad_gpu.tail, &s_quad_gpu.capacity,
                                 GUI_QUAD_BUCKET, 0u, (u32)GUI_QUAD_BYTES, vp, need,
                                 &s_render.quad_buf, &s_render.quad_buf_idx, quad_gpu_create,
                                 "quad" );
}

/*==============================================================================================
    The prim table's buffer -- uses the same replace-on-growth scheme as the quad table above,
    with one difference: it also has the fixed header (palette blocks + overlay records) sitting
    in front of the claim space.
==============================================================================================*/

static void render_pal_invalidate( void );   /* gui_render_pal.c, included after this unit */

static bool
prim_gpu_create( u32 capacity )
{
    rhi_buffer_t buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = ( (u32)GUI_PRIM_HDR_RECORDS + RHI_MAX_FRAMES_IN_FLIGHT * capacity )
                          * (u32)GUI_PRIM_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_prim_table",
    } );
    if ( !rhi_handle_valid( buf ) )
        return false;

    u32 idx = rhi()->register_buffer( buf );
    if ( idx == 0 )
    {
        rhi()->buffer_destroy( buf );
        return false;
    }

    s_render.prim_buf     = buf;
    s_render.prim_buf_idx = idx;
    s_prim_gpu.capacity   = capacity;
    ++s_prim_gpu.buf_gen;      /* every region's bytes are gone: re-upload on next touch */
    render_pal_invalidate();   /* the palette blocks are gone with them */
    return true;
}

static gui_claim_t*
prim_gpu_reserve( i32 vp, u32 need )
{
    return claim_space_reserve( s_prim_gpu.claim, &s_prim_gpu.tail, &s_prim_gpu.capacity,
                                 GUI_PRIM_BUCKET, (u32)GUI_PRIM_HDR_RECORDS, (u32)GUI_PRIM_BYTES,
                                 vp, need, &s_render.prim_buf, &s_render.prim_buf_idx,
                                 prim_gpu_create, "prim" );
}

static bool
render_init( void )
{
    rhi_shader_t vert = { RHI_NULL_HANDLE };
    rhi_shader_t frag = { RHI_NULL_HANDLE };

    if ( !render_load_shaders( &vert, &frag ) )
        return false;

    // Alpha blend: out = src_rgb*src_a + dst_rgb*(1-src_a).
    rhi_color_target_t color_target = {
        .format       = RHI_FORMAT_BGRA8_SRGB,
        .blend_enable = true,
        .src_color    = RHI_BLEND_SRC_ALPHA,
        .dst_color    = RHI_BLEND_ONE_MINUS_SRC_A,
        .color_op     = RHI_BLEND_OP_ADD,
        .src_alpha    = RHI_BLEND_ONE,
        .dst_alpha    = RHI_BLEND_ONE_MINUS_SRC_A,
        .alpha_op     = RHI_BLEND_OP_ADD,
    };

    /* BUFFERLESS: attrib_count is 0 and vertex_stride is 0, so the RHI creates this pipeline
       with no vertex buffer binding at all -- the vertex stage instead pulls everything it
       needs straight from the quad table, indexed by SV_VertexID. Both pipelines below share
       one descriptor and differ only in polygon_mode. The wireframe variant
       (VK_POLYGON_MODE_LINE) lets the debug render mode draw triangle edges using the exact
       same shaders and push-constant layout -- the flush just picks whichever pipeline the
       current mode calls for. */
    rhi_pipeline_desc_t pdesc = {
        .vert               = vert,
        .frag               = frag,
        .attrib_count       = 0,
        .vertex_stride      = 0,
        .cull               = RHI_CULL_NONE,
        .polygon_mode       = RHI_POLYGON_FILL,
        .depth_test         = false,
        .depth_write        = false,
        .color_targets      = { color_target },
        .color_target_count = 1,
        .depth_format       = RHI_FORMAT_UNKNOWN,
        .push_const_size    = sizeof( gui_push_t ),
        .debug_name         = "gui_quad",
    };
    s_render.pipeline_quad = rhi()->pipeline_create( &pdesc );

    /* The wireframe pipeline is the only extra GPU resource the debug-view toggle needs (normal
       / wireframe / batch-tint). If it fails to create, gui_render_flush just falls back to the
       normal fill pipeline. */
    pdesc.polygon_mode = RHI_POLYGON_LINE;
    pdesc.debug_name   = "gui_quad_wire";
    s_render.pipeline_quad_wire = rhi()->pipeline_create( &pdesc );

    rhi()->shader_destroy( frag );
    rhi()->shader_destroy( vert );

    /* The wireframe pipeline is just a debug convenience -- if it fails to create that's not
       fatal, since the mode falls back to the fill pipeline at flush time. Only the fill
       pipeline is actually required. */
    if ( !rhi_handle_valid( s_render.pipeline_quad ) )
        return false;

    /* Font sampler: nearest filter, so text stays crisp. V/W clamp-to-edge, so neighboring atlas
       glyph rows never bleed into each other. U repeats, so a single dashed-line quad can tile
       an atlas stipple pattern along its length. Regular glyph and solid-fill U coordinates
       always stay within [0,1] anyway, so that repeat setting never affects text or fills. */
    s_render.font_sampler = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_NEAREST,
        .mag_filter = RHI_FILTER_NEAREST,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_REPEAT,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( !rhi_handle_valid( s_render.font_sampler ) )
    {
        if ( rhi_handle_valid( s_render.pipeline_quad_wire ) )
            rhi()->pipeline_destroy( s_render.pipeline_quad_wire );
        rhi()->pipeline_destroy( s_render.pipeline_quad );
        return false;
    }
    s_render.font_sampler_idx = rhi()->register_sampler( s_render.font_sampler );

    /* The image sampler: bilinear, and CLAMP on both axes. A sprite is never tiled by this
       sampler -- when tiling is wanted, the tessellator does it by repeating quads instead --
       so using REPEAT here would risk wrapping a stretched sprite onto its neighbor in the
       atlas. It's fine if this fails to create: image draws just fall back to the point sampler
       instead, which looks blocky but still renders correctly. */
    s_render.image_sampler = rhi()->sampler_create( &( rhi_sampler_desc_t ){
        .min_filter = RHI_FILTER_LINEAR,
        .mag_filter = RHI_FILTER_LINEAR,
        .mip_filter = RHI_FILTER_NEAREST,
        .address_u  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_v  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
        .address_w  = RHI_ADDRESS_MODE_CLAMP_TO_EDGE,
    } );
    if ( rhi_handle_valid( s_render.image_sampler ) )
        s_render.image_sampler_idx = rhi()->register_sampler( s_render.image_sampler );

    /* The frame clip table. This is REQUIRED, not just a nicety: the fragment shader is the
       only thing cutting content to its clip rect now (the scissor rect is a fixed,
       full-surface constant), so running without this table would let every scrolled row paint
       right past its region. */
    s_render.clip_buf = rhi()->buffer_create( &( rhi_buffer_desc_t ){
        .size       = GUI_CLIP_REGION_COUNT * GUI_CLIP_REGION_BYTES,
        .usage      = RHI_BUFFER_USAGE_STORAGE,
        .memory     = RHI_MEMORY_CPU_TO_GPU,
        .debug_name = "gui_clip_table",
    } );
    if ( rhi_handle_valid( s_render.clip_buf ) )
        s_render.clip_buf_idx = rhi()->register_buffer( s_render.clip_buf );
    if ( s_render.clip_buf_idx == 0 )
    {
        gui_log( GUI_LOG_ERROR, "clip table buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* The primitive record table. REQUIRED for the same reason the clip table is: the fragment
       shader resolves a quad's shape from its record, so with no table there's nothing for it
       to draw. Starts with a boot-sized claim space sitting behind the fixed header (palette
       blocks + overlay records), and grows under claim pressure via prim_gpu_reserve. */
    if ( !prim_gpu_create( GUI_PRIM_GPU_BOOT_CAP ) )
    {
        gui_log( GUI_LOG_ERROR, "primitive record buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* The quad-record geometry table. REQUIRED like the two tables above: every draw pulls its
       placement from it. Created at boot capacity; viewport claims grow it on demand via
       quad_gpu_reserve. */
    if ( !quad_gpu_create( GUI_QUAD_GPU_BOOT_CAP ) )
    {
        gui_log( GUI_LOG_ERROR, "quad record buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* The glyph UV table. REQUIRED: a glyph quad only carries a table ID rather than its own
       atlas rect, so without this table every character would end up sampling texel zero. */
    if ( !render_glyph_buf_create() )
    {
        gui_log( GUI_LOG_ERROR, "glyph table buffer unavailable -- gui render disabled" );
        render_shutdown();
        return false;
    }

    /* Fonts get loaded later, in the DRAW unit (gui_draw_boot, once the whole server has stood
       up). Solid-color shapes still get bound to this same shared atlas index even though they
       never actually sample it (GUI_OP_SELF skips the texel read) -- that's what lets solids
       and text merge into a single draw call together. */
    return true;
}

static void
render_shutdown( void )
{
    /* Logs the peak fill of each BUILD-stage pool over the run, so the size caps can be tuned
       from real measurements instead of guesswork. These are three independent budgets: quads
       scale with how much is on screen, styles scale with how many distinct looks are in use,
       and gpu commands scale with how the viewport splits work up. */
    gui_log( GUI_LOG_INFO,
             "peak build pools: quads %u/%u (%.1f%%), styles %u/%u (%.1f%%), gpu cmds %u/%u (%.1f%%)",
             s_tess_stats.quad_hwm, GUI_MAX_QUADS, 100.0f * s_tess_stats.quad_hwm / (f32)GUI_MAX_QUADS,
             s_tess_stats.prim_hwm, GUI_MAX_PRIMS, 100.0f * s_tess_stats.prim_hwm / (f32)GUI_MAX_PRIMS,
             s_tess_stats.cmd_hwm,  GUI_MAX_CMDS,  100.0f * s_tess_stats.cmd_hwm  / (f32)GUI_MAX_CMDS );

    if ( s_tess_stats.overflow_walls )
    {
        char walls[ 128 ];
        gui_log( GUI_LOG_WARN, "  -- OVERFLOWED this run (content dropped): %s",
                 tess_overflow_walls( s_tess_stats.overflow_walls, walls, (u32)sizeof( walls ) ) );
    }
    if ( s_tess_stats.uncacheable_wins )
        gui_log( GUI_LOG_WARN, "  -- %u window placement%s exceeded WIN_SLOT_CMD_MAX (%u) and "
                               "re-tessellated every frame",
                 s_tess_stats.uncacheable_wins, s_tess_stats.uncacheable_wins == 1u ? "" : "s",
                 (unsigned)WIN_SLOT_CMD_MAX );

    // Peak draw calls seen in a single frame -- a rough measure of how well batching is working.
    gui_log( GUI_LOG_INFO, "peak draw calls in a frame: %u", cache_draw_call_hwm() );

    /* Per-draw GPU state cost.
         pushes   -- counts tail writes: the full push-constant block once per flush, plus one
                     tail push per dispatched window slot (for the per-slot prim_base and
                     clip_base values), plus one more per draw call in the BATCH debug view.
                     Reported against the flush count, so a normal run should read roughly
                     1 + the number of windows on the surface.
         scissors -- now exactly one full-surface scissor set per flush: clipping happens per
                     fragment now (via the clip band), so the scissor rect never needs to
                     change mid-walk. */
    if ( s_render.state_draws )
    {
        gui_log( GUI_LOG_INFO,
                 "per-draw state: %llu scissors, %llu draws over %llu flushes; "
                 "%llu tail pushes (%.2f per flush)",
                 (unsigned long long)s_render.state_scissors,
                 (unsigned long long)s_render.state_draws,
                 (unsigned long long)s_render.state_flushes,
                 (unsigned long long)s_render.state_pushes,
                 s_render.state_flushes ? (f64)s_render.state_pushes / (f64)s_render.state_flushes
                                        : 0.0 );
    }

    /* The last submitted frame might still be running on the GPU, but the destroys below (font
       textures, samplers, pipelines) happen immediately -- so drain the device first to make
       sure nothing is still using them. */
    rhi()->device_wait_idle();

    if ( s_render.font_sampler_idx )
        rhi()->unregister_sampler( s_render.font_sampler_idx );
    if ( rhi_handle_valid( s_render.font_sampler ) )
        rhi()->sampler_destroy( s_render.font_sampler );
    if ( s_render.image_sampler_idx )
        rhi()->unregister_sampler( s_render.image_sampler_idx );
    if ( rhi_handle_valid( s_render.image_sampler ) )
        rhi()->sampler_destroy( s_render.image_sampler );

    if ( s_render.clip_buf_idx )
        rhi()->unregister_buffer( s_render.clip_buf_idx );
    if ( rhi_handle_valid( s_render.clip_buf ) )
        rhi()->buffer_destroy( s_render.clip_buf );

    if ( s_render.prim_buf_idx )
        rhi()->unregister_buffer( s_render.prim_buf_idx );
    if ( rhi_handle_valid( s_render.prim_buf ) )
        rhi()->buffer_destroy( s_render.prim_buf );
    memset( &s_prim_gpu, 0, sizeof( s_prim_gpu ) );   /* claims die with the buffer */

    if ( s_render.quad_buf_idx )
        rhi()->unregister_buffer( s_render.quad_buf_idx );
    if ( rhi_handle_valid( s_render.quad_buf ) )
        rhi()->buffer_destroy( s_render.quad_buf );
    memset( &s_quad_gpu, 0, sizeof( s_quad_gpu ) );   /* claims die with the buffer */

    if ( s_render.glyph_buf_idx )
        rhi()->unregister_buffer( s_render.glyph_buf_idx );
    if ( rhi_handle_valid( s_render.glyph_buf ) )
        rhi()->buffer_destroy( s_render.glyph_buf );

    if ( rhi_handle_valid( s_render.pipeline_quad_wire ) )
        rhi()->pipeline_destroy( s_render.pipeline_quad_wire );
    if ( rhi_handle_valid( s_render.pipeline_quad ) )
        rhi()->pipeline_destroy( s_render.pipeline_quad );

    memset( &s_render, 0, sizeof( s_render ) );
}

/* Memory accounting lives in render/gui_render_mem.c (backend_memory). It's included LAST in
   this unity build file, so it can take sizeof() every backend static -- including the
   capture/debug files that get included after this one. */

// clang-format on
/*============================================================================================*/
