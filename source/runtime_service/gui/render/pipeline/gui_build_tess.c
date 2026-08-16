/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess.c -- CPU-side tessellation engine.

    Translates the frame's semantic gui_cmd_t list (s_draw) into packed vertex/index
    geometry in s_tess.  This is the CPU half of the command-list split: everything here
    reads semantic commands and writes gui_draw_vert_t / u16 index data; nothing here
    touches the GPU API.

    s_tess is read only by the two files included after: gui_build_cache.c (the BUILD phase
    fills it via tess_dispatch) and gui_render_submit.c (gui_render_flush uploads it and emits
    draw calls).  No file above the backend unit touches it.

    Included by gui_render.c after gui_emit_path.c (provides v2, seg_normal,
    stroke_center_offset, STROKE_* constants) and before gui_build_cache.c (which drives
    tess_reset / tess_dispatch from cache_build_frame / cache_tess_window).

==============================================================================================*/
// clang-format off

/* One GPU command plus its placement -- the AOS command record.  Every consumer (the merge check,
   the flush loop, the volatile copy-back, the dashboard capture) reads a WHOLE command at a given
   index; none sweeps a single field across all commands, so the fields that belong to one command
   live together in one cache line rather than in four parallel arrays keyed on the same index.
   ibase is explicit (not accumulated from elem_counts at flush) so the index buffer may hold
   reserved gaps -- volatile block headroom -- between commands.  Mirrors dash_cmd_t (the snapshot
   type in gui_render.h), which was AOS from the start; this is the live half catching up. */

typedef struct
{
    gui_gpu_cmd_t cmd;      // elem_count, tex_idx, clip_rect -- the GPU draw-call unit
    i32           vp;       // viewport for this command (GUI_VP_INVALID = dormant volatile pad)
    u32           vbase;    // vtx slot -- first vertex of command
    u32           ibase;    // idx slot -- first index of command (its draw call's first_index)

} tess_gpu_cmd_t;

/*==============================================================================================
    Tessellation state -- private vertex/index buffers populated from the semantic command list.

    cache_build_frame tessellates the frame's gui_cmd_t list into s_tess (per window, via
    tess_dispatch), then gui_render_flush uploads s_tess.verts/indices to the GPU.  s_tess is
    backend-private; nothing above the backend unit touches it.  s_draw holds only semantic
    commands (gui_cmd_t) -- no vtx/idx buffers.

    cur_clip/cur_vp are written by tess_dispatch before each primitive and ARE the batch key, which
    is why tess_ensure_gpu_cmd takes no parameters -- it reads them from here.  cur_clip is
    resolved from s_draw.clip_table[c->clip_idx]; z is per-segment and is not tracked here.
==============================================================================================*/

static struct
{
    gui_draw_vert_t verts    [ GUI_MAX_VERTS ];    // geometry buffer
    u16             indices  [ GUI_MAX_IDX   ];    // geometry buffer
    tess_gpu_cmd_t  gpu_cmds [ GUI_MAX_CMDS  ];    // gpu draw commands (AOS: cmd + vp/vbase/ibase)

    u32 vert_count, idx_count, cmd_count;           // write head cursors

    gui_rect_t  cur_clip;   /* clip resolved from s_draw.clip_table[c->clip_idx] for each command */
    u32         cur_clip_local; /* the same clip as an ABSOLUTE frame-region entry index (the slot's
                                   slab base + its local first-seen index) -- the clip-band bits
                                   tess_verts_commit folds into every vertex's tex word           */
    i32         cur_vp;     /* viewport baked from the current semantic command                    */
    u32         cur_tex;    /* GUI_TEX_MODE | bindless slot stamped into every vertex committed --
                               set by tess_set_tex, applied by tess_verts_commit.  NOT a batch key */
    u32         cur_fx;     /* packed effect word stamped into every vertex committed.  CLEARED at
                               the top of each semantic command (tess_dispatch), so a primitive
                               that wants one sets it and nothing can inherit it afterwards.      */
    u32         cur_tex_bits; /* GUI_TEX_SELF_BIT | GUI_TEX_OP_* -- the per-primitive flags OR'd
                               into the tex word by tess_verts_commit.  Cleared per semantic
                               command exactly like cur_fx: leaking a self bit would blank a
                               textured quad, and leaking an op would reshape the next fill. */

    /* per-slot tesellation context */

    /* Vertex base of the window slot currently being tessellated.  Index values emitted during
       tess are (local_vert - slot_vert_base), making them 0-relative within the slot.  At draw
       time vertex_offset = slot.vert_base shifts them to the correct absolute VB position. */
    u32 slot_vert_base;

    /* Index/command base and tessellation generation of the window slot currently being
       tessellated -- set alongside slot_vert_base by cache_build_frame's retess path.  Volatile
       blocks record their position slot-RELATIVE (never absolute) so a later patch can resolve
       the current absolute position from the live slot table, and stamp slot_tess_gen so a patch
       only ever writes into geometry produced by the exact tessellation pass that captured it
       (see render/pipeline/gui_build_volatile.c). */

    u32 slot_idx_base;
    u32 slot_cmd_base;
    u32 slot_tess_gen;

    /* The slot's LOCAL clip table, written while its commands tessellate: first-seen distinct
       clip rects, appended by tess_clip_local and stored on the slot record itself so cache-hit
       frames replay the exact rects the baked vertex indices mean.  slot_clips/slot_clip_count
       point into the win_geo_slot_t being tessellated (set alongside slot_vert_base); NULL
       between slots.  slot_clip_base is the window's fixed slab origin in the frame clip region
       (stable cache slot * GUI_WIN_CLIP_MAX) -- tess_clip_local folds it in so vertices bake
       ABSOLUTE entry indices.  slot_clip_pending is the slot's upload mask (one bit per
       (frame-in-flight, viewport) region); an append marks all bits so the flush re-uploads the
       slab.  clip_memo_ci memoizes the common run of consecutive same-clip commands (0xFF =
       empty). */
    gui_clip_entry_t* slot_clips;
    u32*              slot_clip_count;
    u8*               slot_clip_pending;
    u32               slot_clip_base;
    u8                clip_memo_ci;
    u32               clip_memo_local;

    /* Set before each cache_tess_window call so tess_ensure_gpu_cmd always opens a fresh
       command for the first primitive of a new slot, even when the previous slot's last
       command shares the same clip/vp (same-position windows would otherwise merge
       across the slot boundary and corrupt elem_count + first_index tracking). */
    bool force_new_cmd;

    bool overflow;   /* set per-primitive when a buffer fills; gates the geometry-drop escape path */

} s_tess;

/*==============================================================================================
    Tessellation diagnostics -- the cold companion to s_tess.

    High-water marks, the sticky overflow flag, and the arena band boundary the dashboard reads.
    Every field here is written at most once per slot placement / band boundary / frame end --
    never on the per-vertex path -- so it lives apart from s_tess to keep the hot write cacheline
    (vert_count / idx_count / cmd_count) small.  Read only by the dashboard capture and the render
    overlay; overflow itself stays in s_tess because it is written per-primitive on buffer-full.
==============================================================================================*/

/* Geometry generation -- bumped ONLY when the whole arena layout moves: the repack retry
   (cache_build_frame), where every slot relocates at once.  Each (frame-in-flight, viewport)
   upload region remembers the generation it was last filled with (gui_render_submit.c); a
   mismatch forces the full span upload.  Everything finer goes through the dirty spans below:
   a single window's retess or a volatile patch touches only its own slot's bytes, so it unions
   a span instead.  Pure command-side changes (z reorder, a window vanishing) record nothing at
   all: draws re-record every flush and never reference bytes outside their own slots' spans. */
static u32 s_geo_gen = 1;

/* Fine dirty spans -- the per-change companion to s_geo_gen.  A window's in-place retess
   (cache_slot_tessellate) and a volatile patch (volatile_patch) each rewrite bytes inside one
   slot; forcing the full span for that would re-upload the whole arena every presented frame
   anything changes (and a stats overlay observing uploads would cause them).  Instead the
   writer unions its rewritten vertex/index ranges into every in-flight region of the window's
   viewport, and a generation-matching flush uploads just its region's accumulated spans and
   clears them (gui_render_submit.c).  A generation-stale flush's full upload covers every
   accumulated byte of its surface, so it clears the entry too. */
/* Kept per ARENA BAND as well as per region: debug-band slots pack at the arena tail, so a
   single union would bridge from a changed app window to a changed overlay and drag the whole
   arena between them.  Separate spans keep the two-band isolation contract intact -- and let
   the flush attribute band-1 upload bytes to the overlay in the stats it displays. */
static struct
{
    u32 v_lo, v_hi;   // pending vertex range, arena-absolute (empty when v_lo >= v_hi)
    u32 i_lo, i_hi;   // pending index range, arena-absolute

} s_patch_pending[ RHI_MAX_FRAMES_IN_FLIGHT * GUI_MAX_VIEWPORTS ][ 2 ];

static void
patch_range_union( u32* lo, u32* hi, u32 nlo, u32 nhi )
{
    if ( nlo >= nhi )
        return;
    if ( *lo >= *hi ) { *lo = nlo; *hi = nhi; return; }
    if ( nlo < *lo )  *lo = nlo;
    if ( nhi > *hi )  *hi = nhi;
}

static void
patch_span_union( u8 vp, u8 band, u32 v_lo, u32 v_hi, u32 i_lo, u32 i_hi )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return;
    u32 b = band != 0 ? 1u : 0u;
    for ( u32 f = 0; f < RHI_MAX_FRAMES_IN_FLIGHT; ++f )
    {
        u32 r = f * GUI_MAX_VIEWPORTS + vp;
        patch_range_union( &s_patch_pending[ r ][ b ].v_lo, &s_patch_pending[ r ][ b ].v_hi,
                           v_lo, v_hi );
        patch_range_union( &s_patch_pending[ r ][ b ].i_lo, &s_patch_pending[ r ][ b ].i_hi,
                           i_lo, i_hi );
    }
}

static struct
{
    u32  vert_hwm, idx_hwm;   /* lifetime peak of the TOTAL write head (both bands) */
    bool overflow_ever;       /* sticky: any frame this run overflowed a buffer     */

    /* Arena band boundary: the write head right after the last MAIN-band slot placed this frame
       (band-major placement packs every debug-band slot after it).  The dashboard's memory map
       reads these as "main arena ends here"; the span up to vert_count/idx_count past them is
       the debug UI's own attributed footprint.  Re-derived by cache_build_frame every build. */
    u32 band0_vert_end, band0_idx_end;

    /* Lifetime peak of the MAIN-band (band 0) write head alone -- the real application's geometry
       ceiling, tracked apart from vert_hwm so the dashboard can show actual use limits with the
       self-measuring debug band filtered out.  Peaks on a different frame than vert_hwm, so it is a
       separate accumulator, not a subtraction. */
    u32 band0_vert_hwm, band0_idx_hwm;

} s_tess_stats;

/*==============================================================================================
    Quantizers -- the two grids geometry lands on.

    Every axis-aligned fill snaps its ORIGIN to the pixel grid so its edges stay crisp; a shape
    with no straight edge (a disc, a rotated box) deliberately does not, since quantizing its
    centre makes an animated dot stutter.  A pattern's cell rides a quarter-pixel grid instead:
    fine enough that a scaled lattice does not visibly step, coarse enough that the fragment's
    packed cell field can carry it.
==============================================================================================*/

/* Round to the nearest whole pixel -- the snap every straight-edged primitive puts its origin
   through.  Named so a call site says WHY it rounds, not merely that it does. */
static f32
tess_snap_px( f32 v )
{
    return floorf( v + 0.5f );
}

/* A pattern cell quantized to quarter-pixels and clamped to what the effect word can describe. */
static f32
tess_snap_cell( f32 cell )
{
    cell = floorf( cell * 4.0f + 0.5f ) * 0.25f;
    if ( cell < 1.0f )             cell = 1.0f;
    if ( cell > GUI_FX_CELL_MAX )  cell = GUI_FX_CELL_MAX;
    return cell;
}

/*==============================================================================================
    Tessellation helpers -- mirrors of the draw_push_* functions in gui_emit_draw.c, but writing
    into s_tess instead of s_draw.  These are the backend half of the command-list split.
    Called from tess_dispatch; not called from anywhere else.
==============================================================================================*/

static void
tess_reset( void )
{
    s_tess.vert_count      = 0;
    s_tess.idx_count       = 0;
    s_tess.cmd_count       = 0;
    s_tess.slot_vert_base  = 0;
    s_tess.slot_idx_base   = 0;
    s_tess.slot_cmd_base   = 0;
    s_tess.slot_tess_gen   = 0;
    s_tess.slot_clips        = NULL;
    s_tess.slot_clip_count   = NULL;
    s_tess.slot_clip_pending = NULL;
    s_tess.slot_clip_base    = 0;
    s_tess.clip_memo_ci      = 0xFF;
    s_tess.cur_clip_local    = 0;
    s_tess.force_new_cmd     = false;
    s_tess.overflow          = false;
}

/* Name the texture the vertices about to be written will CARRY (tess_verts_commit stamps it onto
   each one).  Deliberately NOT part of opening a batch, and separated from it so that reads: the
   texture rides the vertex now, so a texture change costs nothing and must not open a command.
   Pair it with tess_ensure_gpu_cmd below -- order between the two does not matter, only that both
   happen before the primitive's vertices are committed. */
static void
tess_set_tex( u32 tex_idx )
{
    s_tess.cur_tex = tex_idx;
}

/* Resolve the ambient clip to an ABSOLUTE entry index in the frame clip region: the window's
   fixed slab base plus its position in the slot's LOCAL clip table, appending a new entry at
   first sight.  Content-keyed -- (rect, radius) is the whole identity -- and first-seen ordered
   over the window's own commands, so a window whose commands hash identical reproduces identical
   indices: the property that lets cached vertices bake the clip-band bits.  The slab base
   is keyed by the window's id-keyed stable cache slot, so the absolute index survives as long as
   the window does.  An append marks the slot's upload mask -- the flush re-uploads a slab only
   when its content changed.  The memo serves the common run of consecutive same-clip commands.
   A slot past GUI_WIN_CLIP_MAX distinct clips falls back to its slab's entry 0 (asserted -- 16
   distinct clips in one window is a bug, not a budget). */
static u32
tess_clip_local( u8 ci )
{
    if ( s_tess.clip_memo_ci == ci )
        return s_tess.clip_memo_local;
    s_tess.clip_memo_ci = ci;

    if ( !s_tess.slot_clips )
        return s_tess.clip_memo_local = s_tess.slot_clip_base;

    const gui_rect_t* r   = &s_draw.clip_table[ ci ];
    f32               rad = s_draw.clip_radius[ ci ];
    u32               n   = *s_tess.slot_clip_count;
    for ( u32 g = 0; g < n; ++g )
    {
        const gui_clip_entry_t* e = &s_tess.slot_clips[ g ];
        if ( e->rect.x == r->x && e->rect.y == r->y && e->rect.w == r->w && e->rect.h == r->h
          && e->radius == rad )
            return s_tess.clip_memo_local = s_tess.slot_clip_base + g;
    }
    if ( n >= GUI_WIN_CLIP_MAX )
    {
        ORB_ASSERT( false );
        return s_tess.clip_memo_local = s_tess.slot_clip_base;
    }
    s_tess.slot_clips[ n ] = ( gui_clip_entry_t ){ .rect = *r, .radius = rad };
    *s_tess.slot_clip_count = n + 1;
    if ( s_tess.slot_clip_pending )
        *s_tess.slot_clip_pending = 0xFF;
    return s_tess.clip_memo_local = s_tess.slot_clip_base + n;
}

/* Ensure a GPU command is open whose viewport matches the ambient one, opening a new one at a
   mismatch.  THE VIEWPORT IS THE WHOLE BATCH KEY, which is why this takes no arguments: the
   texture, the effect word and the clip all travel per vertex and cannot cut a draw call, and z
   is per-segment rather than per-command (the segment system already guarantees every command in
   one window's tessellation pass shares a z).  A new primitive type therefore batches correctly
   by construction -- there is nothing left to pass in and get wrong.
   Returns false when the command table is full and no matching command is open -- the caller must
   drop its primitive, or its geometry would append to a command with the wrong viewport. */
static bool
tess_ensure_gpu_cmd( void )
{
    if ( s_tess.cmd_count > 0 && !s_tess.force_new_cmd )
    {
        const tess_gpu_cmd_t* prev = &s_tess.gpu_cmds[ s_tess.cmd_count - 1 ];
        if ( prev->vp == s_tess.cur_vp )
            return true;
    }
    if ( s_tess.cmd_count >= GUI_MAX_CMDS )
    {
        s_tess.overflow = true;
        return false;
    }
    s_tess.force_new_cmd = false;
    /* Vertex span of this command starts at the current vert_count; the next command's vbase (or
       the final vert_count for the last) bounds it.  Lets a surface upload only its own vertices.
       ibase records where this command's indices start -- its draw call's first_index.
       tex_idx and clip_rect are the ambient values at the moment the command opened, i.e. the
       FIRST primitive's, and are diagnostic only (the dashboard tooltip) -- both ride the vertex
       now and the command may go on to span several of each. */
    s_tess.gpu_cmds[ s_tess.cmd_count++ ] = ( tess_gpu_cmd_t ){
        .cmd   = { .elem_count = 0, .tex_idx = s_tess.cur_tex, .clip_rect = s_tess.cur_clip },
        .vp    = s_tess.cur_vp,
        .vbase = s_tess.vert_count,
        .ibase = s_tess.idx_count,
    };
    return true;
}

/* Take `n` vertices written at s_tess.verts[vert_count] into the buffer, stamping each with the
   active texture and effect word.

   EVERY vertex writer ends here -- this is the only place vert_count advances, which is what makes
   those two words impossible to forget.  Both are applied from ambient state rather than passed in,
   because both are constant over a primitive while only the position/uv/colour vary: cur_tex is set
   by tess_set_tex (which every writer calls alongside tess_ensure_gpu_cmd, to have a command to
   append to), and cur_fx is cleared per semantic command and set by the few that want one.  A new
   primitive type gets both correct by construction; the alternative -- naming them in each compound
   literal -- fails silently, since a missing trailing initializer is a legal zero, and zero means
   "the empty bindless descriptor" for one and "no effect" for the other. */
static void
tess_verts_commit( u32 n )
{
    /* The clip band (gui.h): the absolute clip entry index rides bits 17..27 of the tex word.  The
       bindless index below never reaches them (2048 slots, 11 bits), asserted because a collision
       here silently re-clips the primitive rather than failing. */
    ORB_ASSERT( ( s_tess.cur_tex & GUI_TEX_CLIP_MASK ) == 0u );
    ORB_ASSERT( ( s_tess.cur_tex & ( GUI_TEX_SELF_BIT | GUI_TEX_OP_MASK ) ) == 0u );
    ORB_ASSERT( s_tess.cur_clip_local < ( GUI_TEX_CLIP_MASK >> GUI_TEX_CLIP_SHIFT ) + 1u );
    u32 tex = s_tess.cur_tex | s_tess.cur_tex_bits | ( s_tess.cur_clip_local << GUI_TEX_CLIP_SHIFT );

    gui_draw_vert_t* v = &s_tess.verts[ s_tess.vert_count ];
    for ( u32 i = 0; i < n; ++i )
    {
        v[ i ].tex = tex;
        v[ i ].fx  = s_tess.cur_fx;
    }
    s_tess.vert_count += n;
}

/*==============================================================================================
    tess_prim_begin / tess_prim_commit -- the reservation every primitive goes through.

    Four steps, in this order, and every one of them is a place a hand-written primitive gets it
    subtly wrong:

      1. Check BOTH budgets before writing anything, and set the sticky overflow flag rather than
         truncating -- a primitive that emits its vertices and then discovers it has no room for
         indices leaves geometry the index buffer never references.
      2. Name the texture and open a GPU command, in that order relative to the writes.
      3. Hand back a base that is SLOT-RELATIVE (vert_count - slot_vert_base).  Indices are
         0-relative within a window slot and shifted at draw time by vertex_offset; an absolute
         base here is the class of bug the force_new_cmd slot boundary was about, and it does not
         show up until two windows land at the same position.
      4. On commit, advance idx_count AND fold ni into the open command's elem_count.  Forgetting
         the second silently drops the primitive -- the vertices are uploaded, the draw call just
         never reads them.

    tex_idx is a parameter rather than assumed because that is what stopped five primitives from
    using this: it hardcoded the coverage atlas, so anything sampling its own texture (a glyph run,
    an image, a nine-slice piece) had to repeat all four steps by hand.  The texture is no longer a
    batch key, so passing it here costs nothing -- it only names what the vertices will carry.
==============================================================================================*/

static bool
tess_prim_begin_tex( u32 nv, u32 ni, u32 tex_idx,
                     gui_draw_vert_t** out_v, u16** out_i, u16* out_base )
{
    if ( s_tess.vert_count + nv > GUI_MAX_VERTS || s_tess.idx_count + ni > GUI_MAX_IDX )
    {
        s_tess.overflow = true;
        return false;
    }
    tess_set_tex( tex_idx );
    if ( !tess_ensure_gpu_cmd() )
        return false;
    *out_base = (u16)( s_tess.vert_count - s_tess.slot_vert_base );
    *out_v    = &s_tess.verts  [ s_tess.vert_count ];
    *out_i    = &s_tess.indices[ s_tess.idx_count  ];
    return true;
}

/* The solid-colour form: the coverage atlas, plus its white texel's UV -- what every primitive
   that paints a flat colour rather than sampling art wants. */
static bool
tess_prim_begin( u32 nv, u32 ni, f32* wu, f32* wv,
                 gui_draw_vert_t** out_v, u16** out_i, u16* out_base )
{
    if ( !tess_prim_begin_tex( nv, ni, res_atlas_idx(), out_v, out_i, out_base ) )
        return false;
    res_atlas_white_uv( wu, wv );
    return true;
}

/* The INDEX half of tess_prim_commit, separable because one primitive can commit its vertices in
   more than one chunk.  Exactly one does: the rounded box stamps a different effect word onto each
   quadrant when the corners differ, and the word is applied by tess_verts_commit -- so the vertices
   go in four passes while the indices, which belong to the single reservation, are accounted once.
   Splitting rather than reserving four times is what keeps the shape ALL-OR-NOTHING: four separate
   reservations could each fail on its own and leave a rounded rect missing a corner. */
static void
tess_prim_commit_idx( u32 ni )
{
    s_tess.idx_count  += ni;
    s_tess.gpu_cmds[ s_tess.cmd_count - 1 ].cmd.elem_count += ni;
}

static void
tess_prim_commit( u32 nv, u32 ni )
{
    tess_verts_commit( nv );
    tess_prim_commit_idx( ni );
}

/* Two triangles over four corners wound TL, TR, BR, BL -- the index pattern of every quad in this
   file, written once.  `base` is the slot-relative index of the quad's first vertex. */
static void
tess_quad_idx( u16* idx, u16 base )
{
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base + 0; idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
}

/* Tessellate a filled quad into s_tess.  abgr has alpha pre-baked by the emit side. */
static void
tess_rect_filled( f32 x, f32 y, f32 w, f32 h,
                  f32 u0, f32 v0, f32 u1, f32 v1,
                  u32 tex_idx, u32 abgr )
{
    /* tex_idx 0 = solid-color convention: route to the font atlas's white texel.  Resolved before
       the reservation, because it decides which texture the vertices will carry. */
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        res_atlas_white_uv( &u0, &v0 );
        u1 = u0; v1 = v0;
    }
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin_tex( 4u, 6u, tex_idx, &v, &idx, &base ) )
        return;

    v[ 0 ] = gui_vert( x,     y,     u0, v0, abgr );
    v[ 1 ] = gui_vert( x + w, y,     u1, v0, abgr );
    v[ 2 ] = gui_vert( x + w, y + h, u1, v1, abgr );
    v[ 3 ] = gui_vert( x,     y + h, u0, v1, abgr );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/* Tessellate a two-color gradient quad: col_a / col_b on opposite edges, sampled at the white
   texel so the GPU's per-vertex color interpolation IS the gradient (one quad, exact blend --
   replaces the old 32-band approximation).  Origin grid-snapped like tess_rect_filled. */
static void
tess_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;

    /* Corner colors walk col_a -> col_b along the chosen axis (TL, TR, BR, BL winding). */
    u32 c0 = col_a;                          /* top-left  */
    u32 c1 = horizontal ? col_b : col_a;     /* top-right */
    u32 c2 = col_b;                          /* bottom-right */
    u32 c3 = horizontal ? col_a : col_b;     /* bottom-left  */

    v[ 0 ] = gui_vert( x,     y,     wu, wv, c0 );
    v[ 1 ] = gui_vert( x + w, y,     wu, wv, c1 );
    v[ 2 ] = gui_vert( x + w, y + h, wu, wv, c2 );
    v[ 3 ] = gui_vert( x,     y + h, wu, wv, c3 );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/*==============================================================================================
    Sprites and the nine-slice expansion

    ONE semantic command becomes 1, 3 or 9 quads here.  The expansion lives at tessellation time
    rather than in the emit layer for two reasons, and they are the same reason twice: only the
    registry knows the source's pixel size and slice insets, and only tessellation runs late enough
    that a sprite-atlas repack (which moves UVs) is already accounted for.  Emitting quads early
    would bake both facts into a command that outlives them.

    The pieces come out of ONE atlas with ONE bindless slot, so a whole nine-slice frame is a single
    GPU batch -- which is what makes an authored border affordable on every panel rather than a
    special occasion.
==============================================================================================*/

/* Repeat cap per axis for a TILEd piece.  Past this the piece stretches instead: a pathological
   pitch (art authored 1px wide, or a scale near zero) would otherwise turn one command into tens
   of thousands of quads, and a stretched fallback is wrong in a way you can see and fix, where an
   exhausted vertex budget is wrong in a way that takes the rest of the frame down with it. */
#define TESS_SPRITE_TILE_MAX  64u

/* One piece of the grid.  pitch <= 0 on an axis means "stretch across that axis" (the default);
   a positive pitch repeats the piece at authored size, trimming the trailing repeat to fit by
   cutting its UV in the same proportion.  UVs may arrive reversed (u0 > u1) -- that is how a flip
   is expressed -- and every interpolation here is a lerp from u0 toward u1, so reversal carries
   through the trim untouched. */
static void
tess_sprite_piece( f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   f32 pitch_x, f32 pitch_y, u32 tex_idx, u32 abgr )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    u32 nx = 1, ny = 1;
    if ( pitch_x > 0.5f )
    {
        f32 n = ceilf( w / pitch_x );
        if ( n < 1.0f || n > (f32)TESS_SPRITE_TILE_MAX ) pitch_x = 0.0f;   /* stretch instead */
        else                                             nx = (u32)n;
    }
    if ( pitch_y > 0.5f )
    {
        f32 n = ceilf( h / pitch_y );
        if ( n < 1.0f || n > (f32)TESS_SPRITE_TILE_MAX ) pitch_y = 0.0f;
        else                                             ny = (u32)n;
    }

    for ( u32 j = 0; j < ny; ++j )
    {
        f32 py = ( pitch_y > 0.0f ) ? y + (f32)j * pitch_y : y;
        f32 ph = ( pitch_y > 0.0f ) ? pitch_y : h;
        if ( py + ph > y + h ) ph = y + h - py;          /* trailing repeat: cut to the piece */
        f32 fv = ( pitch_y > 0.0f ) ? ph / pitch_y : 1.0f;
        f32 pv1 = v0 + ( v1 - v0 ) * fv;

        for ( u32 i = 0; i < nx; ++i )
        {
            f32 px = ( pitch_x > 0.0f ) ? x + (f32)i * pitch_x : x;
            f32 pw = ( pitch_x > 0.0f ) ? pitch_x : w;
            if ( px + pw > x + w ) pw = x + w - px;
            f32 fu = ( pitch_x > 0.0f ) ? pw / pitch_x : 1.0f;
            f32 pu1 = u0 + ( u1 - u0 ) * fu;

            tess_rect_filled( px, py, pw, ph, u0, v0, pu1, pv1, tex_idx, abgr );
        }
    }
}

/* Tessellate one sprite command.  Resolves the sprite through the source contract, then either
   stretches it over the rect as a single quad or lays the nine-slice grid.

   Slice insets are authored in SOURCE pixels and scaled by the command's `scale`, so one piece of
   art serves several UI scales; when the scaled insets no longer fit the destination they are
   shrunk proportionally rather than allowed to overlap, which is what keeps a frame legible when
   its window is dragged smaller than its own corners. */
static void
tess_sprite( const gui_cmd_t* c )
{
    u32 tex = res_sprite_idx();
    if ( tex == 0 )
        return;                       /* no sprite atlas yet -- nothing was ever registered */

    f32       u0, v0, u1, v1;
    u32       sw = 0, sh = 0;
    gui_pad_t sl = { 0 };
    if ( !sprite_get( c->sprite.sprite, &u0, &v0, &u1, &v1, &sw, &sh, &sl ) || sw == 0 || sh == 0 )
        return;

    tex |= GUI_TEX_MODE( GUI_TEX_RGBA );   /* the texel IS the colour; vertex colour tints it */

    const u32 flags  = c->sprite.flags;
    const bool flipx = ( flags & GUI_BRUSH_FLIP_X ) != 0;
    const bool flipy = ( flags & GUI_BRUSH_FLIP_Y ) != 0;
    const bool tile  = ( flags & GUI_BRUSH_TILE   ) != 0;

    const f32 x = c->sprite.x, y = c->sprite.y, w = c->sprite.w, h = c->sprite.h;
    const f32 s = c->sprite.scale;
    const u32 col = c->sprite.abgr;

    /* Scaled slice insets.  A sprite with none (or a command that did not ask for the expansion)
       is one stretched quad -- the flip is then just a reversed UV span. */
    f32 L = sl.l * s, R = sl.r * s, T = sl.t * s, B = sl.b * s;
    if ( !c->sprite.nine || ( L <= 0.0f && R <= 0.0f && T <= 0.0f && B <= 0.0f ) )
    {
        tess_rect_filled( x, y, w, h,
                          flipx ? u1 : u0, flipy ? v1 : v0,
                          flipx ? u0 : u1, flipy ? v0 : v1, tex, col );
        return;
    }

    /* Shrink insets that no longer fit rather than letting opposite corners overlap. */
    if ( L + R > w && L + R > 0.0f ) { f32 k = w / ( L + R ); L *= k; R *= k; }
    if ( T + B > h && T + B > 0.0f ) { f32 k = h / ( T + B ); T *= k; B *= k; }

    /* A flip mirrors the whole sprite, so the destination edge widths swap with the source columns
       they will sample -- do it here, once, and the grid loop below stays flip-agnostic. */
    if ( flipx ) { f32 t2 = L; L = R; R = t2; }
    if ( flipy ) { f32 t2 = T; T = B; B = t2; }

    /* Destination and source boundaries, three tracks each.  su/sv are in UV; the middle source
       track is the stretchable / tileable span between the insets. */
    const f32 upx = ( u1 - u0 ) / (f32)sw;   /* UV per source pixel */
    const f32 vpx = ( v1 - v0 ) / (f32)sh;

    f32 dx[ 4 ] = { x, x + L, x + w - R, x + w };
    f32 dy[ 4 ] = { y, y + T, y + h - B, y + h };
    f32 su[ 4 ] = { u0, u0 + sl.l * upx, u1 - sl.r * upx, u1 };
    f32 sv[ 4 ] = { v0, v0 + sl.t * vpx, v1 - sl.b * vpx, v1 };

    /* Authored pitch of the middle tracks, for TILE.  The corners never tile (they ARE the fixed
       part); the edges tile along their long axis only, and the centre tiles on both. */
    f32 pitch_x = tile ? ( (f32)sw - sl.l - sl.r ) * s : 0.0f;
    f32 pitch_y = tile ? ( (f32)sh - sl.t - sl.b ) * s : 0.0f;

    for ( u32 j = 0; j < 3; ++j )
        for ( u32 i = 0; i < 3; ++i )
        {
            /* Under a flip the destination track samples the mirrored source track, and the span
               comes out reversed (lo > hi) -- which is exactly the mirrored sampling wanted. */
            u32 si = flipx ? ( 3u - i ) : i;
            u32 sj = flipy ? ( 3u - j ) : j;
            f32 pu0 = su[ si ], pu1 = flipx ? su[ si - 1u ] : su[ si + 1u ];
            f32 pv0 = sv[ sj ], pv1 = flipy ? sv[ sj - 1u ] : sv[ sj + 1u ];

            tess_sprite_piece( dx[ i ], dy[ j ], dx[ i + 1 ] - dx[ i ], dy[ j + 1 ] - dy[ j ],
                               pu0, pv0, pu1, pv1,
                               ( i == 1 ) ? pitch_x : 0.0f,
                               ( j == 1 ) ? pitch_y : 0.0f,
                               tex, col );
        }
}

/* Tessellate a hollow rectangle as four edge quads.

   The frame is snapped to whole pixels ONCE, here, and the four quads are cut from those integer
   edges.  Every fill snaps its own origin (tess_rect_filled) and nothing snaps its extent, so
   handing four unsnapped rects to it rounds four origins independently against four fractional
   far edges: the top rail lands on one row and the side rails start on another, and the right
   rail's inner edge misses the top rail's end.  That reads as pixel gaps at the corners and a
   one-pixel overhang on the right and bottom -- on any fractional origin (a scrolled row, a
   fractional layout position) and worse the thicker the stroke.  Snapped first, the shared edges
   are the same integer on both sides and the joins are exact.

   t is rounded to a whole stroke (never below one pixel, so a hairline cannot vanish) and clamped
   to half the shorter side, so a thick border on a small rect degenerates to a filled rect
   instead of inverted side quads. */
static void
tess_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    f32 x0 = tess_snap_px( x ),     y0 = tess_snap_px( y );
    f32 x1 = tess_snap_px( x + w ), y1 = tess_snap_px( y + h );
    f32 bw = x1 - x0,               bh = y1 - y0;
    if ( bw <= 0.0f || bh <= 0.0f )
        return;

    f32 tmax = ( bw < bh ? bw : bh ) * 0.5f;
    t = tess_snap_px( t );
    if ( t < 1.0f ) t = 1.0f;
    if ( t > tmax ) t = tmax;

    tess_rect_filled( x0,     y0,     bw, t, 0,0,1,1, 0, abgr );
    tess_rect_filled( x0,     y1 - t, bw, t, 0,0,1,1, 0, abgr );

    f32 mid = bh - 2.0f * t;   /* the span between the rails; zero once t swallowed the box */
    if ( mid > 0.0f )
    {
        tess_rect_filled( x0,     y0 + t, t, mid, 0,0,1,1, 0, abgr );
        tess_rect_filled( x1 - t, y0 + t, t, mid, 0,0,1,1, 0, abgr );
    }
}

/*==============================================================================================
    The SDF surface -- every rounded shape, in four quads.

    A rounded box used to be tessellated: a cached quarter arc fanned into ~37 vertices with hard
    stair-stepped edges, and a texture could not ride on it at all.  Here the CPU emits only the
    QUADRANTS and the fragment shader resolves the boundary exactly (gui.h, the effect band).

    Why four quads and not one.  The fragment needs `|p| - c`, where p is its offset from the
    shape centre.  The absolute value folds at the centre lines, and a linear interpolator cannot
    reproduce a fold -- but within ONE QUADRANT the sign of p is fixed, so |p| is affine there and
    the hardware interpolates it exactly.  Four quads is the cheapest partition on which the
    interpolation is correct, and it is still less than half the vertices the arc fan cost.  The
    quadrants meet at the centre lines where both sides evaluate to the same value, so there is
    no seam.

    The geometry is grown by `pad` past the box so the falloff has somewhere to land: a feathered
    edge (and a shadow's whole soft skirt) is OUTSIDE the shape's own rect.  The BOUNDARY still
    sits exactly on the authored rect -- pad moves the triangles, never the shape.

    A surface with nothing to paint inside carves its interior out, emitting an L (two quads) per
    quadrant around the hole instead of one quad -- where the 8/32 upper bound below comes from.
    Two modes qualify.  A RING paints a band only `border` px wide, so rasterizing the whole inside
    of a window frame at zero coverage is a real cost on a large panel.  A SKIRT paints nothing at
    all inside its boundary: the elevation shadow under floating chrome is a band of quads around
    the frame rather than a plate spanning the window, roughly a tenth of the area.

    Every SDF surface samples the same atlas as everything else and carries its mode per VERTEX,
    so it merges into whatever GPU command is already open: a soft shadow behind a panel costs
    a batch split of zero.
==============================================================================================*/

/* Emit one SDF surface.  `r4` is the corner radius PER QUADRANT, in the tessellation order
   top-left, top-right, bottom-right, bottom-left (tess_fx_box passes four copies of one radius;
   only tess_round_rect_ex passes four different ones), and `feather` the total width of the falloff
   band straddling the boundary (0 = hard edge); both are read by every mode.  The remaining
   parameters are MODE-SPECIFIC, mirroring the fx word's own re-partitioning -- `border` is the band
   width for GUI_FX_RING, `rate`/`depth` the wave for GUI_FX_PULSE, and each mode ignores the
   other's.  UVs span the AUTHORED box and are clamped over the grown skirt, so a textured rounded
   quad cannot bleed into its atlas neighbour where the coverage has already faded to nothing.

   GUI_FX_SKIRT takes no parameter of its own: it is GUI_FX_BOX with the interior cut, so it reads
   radius and feather exactly as a box does and differs only in the hole below and one line in the
   fragment.

   Per-corner radii are nearly free HERE and nowhere else, which is the whole reason this generalized
   rather than growing a second tessellator: a quadrant quad already covers exactly one corner, so
   the geometry does not change at all -- only which word gets stamped onto its four vertices.  What
   it costs is that the vertices commit in four chunks instead of one (tess_prim_commit_idx).

   Why neighbouring quadrants cannot seam, which is the part that has to be true for any of this to
   work.  Each quadrant measures from its OWN radius, so the obvious worry is that the two sides of a
   shared centre line disagree.  They cannot.  Take the horizontal one (ay = 0): ey is r - hy, and r
   is clamped to lim <= hy, so ey <= 0 for every radius.  The y term therefore drops out of both
   branches of the field and what remains is

       d = max( ax - hx, -hy )

   in which r has cancelled.  The same holds on the vertical centre line.  The fold lines are
   precisely where the corner radius stops contributing, so the two sides agree EXACTLY -- not
   approximately, and not merely because the interior saturates.  This is why per-corner radii place
   no new restriction on feather. */
/* `rot` turns the whole surface about the box CENTRE (radians, screen space; 0 = the common
   axis-aligned path).  Only the four corner POSITIONS rotate -- the effect coordinate stays in
   box-local |p| space, which interpolation preserves because it is affine in the position.  The
   UVs are computed from the UNROTATED position first, so a textured rotated box still maps its
   picture across the authored rect and clamps over the skirt exactly as the upright one does. */
/*----------------------------------------------------------------------------------------------
    tess_grad_t -- a linear colour ramp carried by the box's own VERTICES.

    Colour is affine in position along a linear gradient, and vertex interpolation is affine, so
    evaluating the ramp at each corner reproduces it EXACTLY across every quad -- at any angle,
    for no fx mode, no packed bits and no shader work.  The 16 vertices a rounded box already
    emits are more than the two a gradient needs.

    The effect word could not have expressed this anyway: a box's effect coordinate is |p| - c,
    folded at the vertex, so the fragment cannot tell one side of the shape from the other.  That
    is the same limit that rules out a directional drop shadow, and it is why the sector modes --
    whose coordinate stays signed -- are the ones that carry a gradient in the WORD instead.

    The ramp runs in LINEAR light and is re-encoded to sRGB per vertex.  That is what makes a
    rounded gradient agree with the square draw_gradient at its midpoint: the hardware interpolates
    the DECODED colours, so lerping the sRGB bytes here would put a different colour on the centre
    line than the same two endpoints produce across one quad.
----------------------------------------------------------------------------------------------*/
typedef struct
{
    f32 lin_a[ 4 ], lin_b[ 4 ];   // endpoints in linear light; alpha stays linear (it is coverage)
    f32 dx, dy;                   // gradient axis, box-local, unit length
    f32 inv_len;                  // 1 / the box's extent along that axis (0 for a degenerate box)

} tess_grad_t;

/* The sRGB transfer curve, both directions -- the same pair base/math_color.h states for vec4 and
   both shader twins state for float3.  Kept local as two scalars rather than reached for: the ramp
   needs the curve on a channel, not the vector colour type that header is built around. */
static f32
tess_srgb_to_linear( f32 c )
{
    return ( c <= 0.04045f ) ? c / 12.92f : powf( ( c + 0.055f ) / 1.055f, 2.4f );
}

static f32
tess_linear_to_srgb( f32 c )
{
    return ( c <= 0.0031308f ) ? c * 12.92f : 1.055f * powf( c, 1.0f / 2.4f ) - 0.055f;
}

static void
tess_grad_unpack( u32 abgr, f32* out )
{
    out[ 0 ] = tess_srgb_to_linear( (f32)(   abgr         & 0xFFu ) / 255.0f );
    out[ 1 ] = tess_srgb_to_linear( (f32)( ( abgr >>  8 ) & 0xFFu ) / 255.0f );
    out[ 2 ] = tess_srgb_to_linear( (f32)( ( abgr >> 16 ) & 0xFFu ) / 255.0f );
    out[ 3 ] =                      (f32)( ( abgr >> 24 ) & 0xFFu ) / 255.0f;
}

static void
tess_grad_init( tess_grad_t* g, u32 col_a, u32 col_b, f32 ang, f32 w, f32 h )
{
    f32 cs = cosf( ang ), sn = sinf( ang );
    g->dx = cs;
    g->dy = sn;

    /* The box's own extent along the axis -- the support width of a projected rectangle.  The ramp
       therefore spans the SHAPE at any angle, instead of running off it on the diagonal. */
    f32 len = fabsf( w * cs ) + fabsf( h * sn );
    g->inv_len = ( len > 1e-6f ) ? 1.0f / len : 0.0f;

    tess_grad_unpack( col_a, g->lin_a );
    tess_grad_unpack( col_b, g->lin_b );
}

/* The ramp at a box-LOCAL offset from the centre (pre-rotation, so the gradient turns with the
   box).  t clamps to [0,1]: the ramp spans the box exactly, and the falloff skirt outside it
   carries the end colours rather than extrapolating past them -- which would wrap on the u8 pack. */
static u32
tess_grad_col( const tess_grad_t* g, f32 lx, f32 ly )
{
    f32 t = ( lx * g->dx + ly * g->dy ) * g->inv_len + 0.5f;
    if ( t < 0.0f ) t = 0.0f;
    if ( t > 1.0f ) t = 1.0f;

    u32 out = 0u;
    for ( u32 i = 0; i < 3u; ++i )
    {
        f32 c = tess_linear_to_srgb( g->lin_a[ i ] + ( g->lin_b[ i ] - g->lin_a[ i ] ) * t );
        u32 b = (u32)( c * 255.0f + 0.5f );
        out |= ( ( b > 255u ) ? 255u : b ) << ( i * 8u );
    }
    f32 a  = g->lin_a[ 3 ] + ( g->lin_b[ 3 ] - g->lin_a[ 3 ] ) * t;
    u32 ab = (u32)( a * 255.0f + 0.5f );
    return out | ( ( ( ab > 255u ) ? 255u : ab ) << 24u );
}

/* `grad` NULL is the flat fill in `abgr` -- every caller but the gradient one passes NULL. */
static void
tess_fx_box_core( f32 x, f32 y, f32 w, f32 h, const f32* r4,
                  f32 feather, f32 border, f32 rate, f32 depth, f32 rot,
                  gui_fx_mode_t mode,
                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
                  const tess_grad_t* grad )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    /* Clamp EVERY parameter to what the packed word can carry, here and not only in the packer.
       The packer saturates (gui_fx_fixed), but saturating there alone would silently disagree with
       the geometry this function builds from the same numbers: the effect coordinate is `|p| - k`
       with k = half-extent minus RADIUS, and the falloff skirt is sized from FEATHER, so a value
       the fragment never sees would leave the vertices describing a different shape than the one
       the fragment resolves.  Both sides have to be clamped to the same number.  The radius bound
       bites in practice on a tall "fully round" pill (draw_set_rounding( 9999 ) clamped to half the
       short side), which is the one idiom that reaches past 511.875 px. */
    f32 hx = w * 0.5f, hy = h * 0.5f;
    f32 lim = ( hx < hy ) ? hx : hy;

    f32 rq[ 4 ];
    f32 rmin, rmax;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 r = r4[ i ];
        if ( r > lim ) r = lim;                   /* a radius past half the short side is a capsule */
        if ( r > GUI_FX_RADIUS_MAX ) r = GUI_FX_RADIUS_MAX;
        if ( r < 0.0f ) r = 0.0f;
        rq[ i ] = r;
    }
    rmin = rmax = rq[ 0 ];
    for ( u32 i = 1; i < 4; ++i )
    {
        if ( rq[ i ] < rmin ) rmin = rq[ i ];
        if ( rq[ i ] > rmax ) rmax = rq[ i ];
    }
    if ( feather < 0.0f ) feather = 0.0f;
    if ( feather > GUI_FX_FEATHER_MAX ) feather = GUI_FX_FEATHER_MAX;
    if ( border  < 0.0f ) border  = 0.0f;
    if ( mode == GUI_FX_RING && border > GUI_FX_BORDER_MAX ) border = GUI_FX_BORDER_MAX;
    if ( rate    < 0.0f ) rate  = 0.0f;
    if ( rate    > GUI_FX_RATE_MAX ) rate = GUI_FX_RATE_MAX;
    if ( depth   < 0.0f ) depth = 0.0f;
    if ( depth   > 1.0f ) depth = 1.0f;

    /* Grid-snap the origin like tess_rect_filled -- UNLESS the shape is a circle.
       Snapping exists to keep STRAIGHT edges crisp, and it is derived rather than passed in
       because the condition is a property of the shape, not of the caller: a shape has no straight
       edge in either axis exactly when it is square and its radius reached the half-extent.  That
       is a disc, and it is also a circular RING -- so both fall out of one test, which is what
       keeps them aligned.  It matters that they agree: a filled disc and a ring drawn at the same
       centre would otherwise sit up to half a pixel apart, and concentric marks are precisely how
       these get used.
       Snapping a circle is not merely pointless but harmful.  Its origin is (centre - r), so
       snapping quantizes the CENTRE, and a small dot animating along a path steps instead of
       gliding.  A pill (w != h, r == the short half-extent) still snaps, correctly -- it does have
       two straight edges.  With per-corner radii the test reads rMIN: a shape is a disc only when
       EVERY corner reached the limit, and one square corner is a straight edge worth snapping.
       A ROTATED box never snaps: it has no axis-aligned edge to keep crisp, and quantizing its
       centre is the animated-dot mistake again (tess_quad_xf's rule). */
    if ( rot == 0.0f && !( hx == hy && rmin >= lim ) )
    {
        x = tess_snap_px( x );
        y = tess_snap_px( y );
    }

    /* tex_idx 0 = solid-color convention, same as tess_rect_filled: the atlas white texel. */
    f32 wu, wv;
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        res_atlas_white_uv( &wu, &wv );
        u0 = u1 = wu;
        v0 = v1 = wv;
    }

    f32 pad = feather * 0.5f + 1.0f;              /* room for the falloff, plus a pixel of slack */
    f32 ehx = hx + pad, ehy = hy + pad;           /* grown half-extent (geometry only)           */
    f32 cx  = x + hx,   cy  = y + hy;
    f32 rcs = 1.0f, rsn = 0.0f;                   /* the rotation, hoisted out of the corner loop */
    if ( rot != 0.0f ) { rcs = cosf( rot ); rsn = sinf( rot ); }

    /* `reach`: how deep past the boundary this surface paints nothing, so the geometry need not
       reach there either.  A RING stops at the far side of its band; a SKIRT stops at the boundary
       itself, since its coverage is cut to zero inside (a pixel of slack keeps the hole clear of
       the boundary the rasterizer resolves).  Both are properties of the MODE -- neither makes any
       claim about what is drawn over the shape, so neither can be wrong about it.

       The INSET op is read from ambient state rather than taken as a parameter because it is
       already there: the op rides the tex word, which this function does not otherwise touch, and
       threading a nineteenth argument through both entry points to restate it would be the only
       alternative.  An inset paints from the boundary inward exactly `feather` px, so that IS its
       reach -- the same relationship the ring's band has to its own. */
    f32 reach = ( mode == GUI_FX_RING  ) ? border + feather * 0.5f
              : ( mode == GUI_FX_SKIRT ) ? 1.0f
              : ( s_tess.cur_tex_bits & GUI_TEX_OP_INSET ) ? feather
                                         : 0.0f;

    /* The per-quadrant coverage, in quadrant-local |p| space: one box normally, two forming an L
       when there is an interior worth skipping.

       Sizing the hole is the whole subtlety.  It must lie entirely at d <= -reach -- one pixel of
       it inside the painted band would notch the frame -- and the binding case is its CORNER, which
       pokes diagonally toward the arc.  So the hole is the largest axis-aligned box inscribed in
       the shape ERODED by that reach, whose corner sits on the eroded arc at 45 degrees.  Erode
       until nothing is left and there is simply no hole. */
    f32 lo_x[ 2 ] = { 0.0f, 0.0f }, lo_y[ 2 ] = { 0.0f, 0.0f };
    f32 hi_x[ 2 ] = { ehx,  0.0f }, hi_y[ 2 ] = { ehy,  0.0f };
    u32 nbox = 1;

    /* rmin == rmax gates the hole to a UNIFORM radius: the inscribed box below is derived from one
       radius, and with four it would have to be the intersection of four different erosions.  Not
       worth deriving -- the only per-corner caller is a FILL, which has no hole.  A mixed-radius
       ring would simply pay for interior fragments it does not paint. */
    if ( reach > 0.0f && rmin == rmax )
    {
        f32 er = rmax - reach;                    /* the eroded shape's radius */
        if ( er < 0.0f ) er = 0.0f;
        f32 hix = ( hx - reach - er ) + er * 0.70710678f;
        f32 hiy = ( hy - reach - er ) + er * 0.70710678f;
        if ( hix > 0.0f && hiy > 0.0f )
        {
            lo_x[ 0 ] = hix;  lo_y[ 0 ] = 0.0f;  hi_x[ 0 ] = ehx;  hi_y[ 0 ] = ehy;   /* side  */
            lo_x[ 1 ] = 0.0f; lo_y[ 1 ] = hiy;  hi_x[ 1 ] = hix;  hi_y[ 1 ] = ehy;   /* end   */
            nbox = 2;
        }
    }

    u32              nv = 4u * nbox * 4u, ni = 4u * nbox * 6u;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin_tex( nv, ni, tex_idx, &v, &idx, &base ) )
        return;

    f32 ulo = ( u0 < u1 ) ? u0 : u1, uhi = ( u0 < u1 ) ? u1 : u0;
    f32 vlo = ( v0 < v1 ) ? v0 : v1, vhi = ( v0 < v1 ) ? v1 : v0;

    static const f32 sx[ 4 ] = { -1.0f, 1.0f, 1.0f, -1.0f };   /* quadrant, CCW from top-left */
    static const f32 sy[ 4 ] = { -1.0f, -1.0f, 1.0f, 1.0f };
    /* Corner of one quad, in that quad's own lo/hi box: (0,0) nearest the centre. */
    static const f32 qu[ 4 ] = { 0.0f, 1.0f, 1.0f, 0.0f };
    static const f32 qv[ 4 ] = { 0.0f, 0.0f, 1.0f, 1.0f };

    u32 n = 0;                                     /* quad counter across quadrants x boxes */

    for ( u32 q = 0; q < 4; ++q )
    {
        /* This quadrant's corner, and therefore its own centre rect and its own word.  When every
           corner matches (tess_fx_box, the overwhelmingly common case) all four words are identical
           and this is exactly what a single stamp produced. */
        f32 kx = hx - rq[ q ], ky = hy - rq[ q ];
        u32 fx = ( mode == GUI_FX_PULSE )
                     ? gui_fx_pack_pulse( rq[ q ], feather, rate, depth )
                     : gui_fx_pack( mode, rq[ q ], feather,
                                    ( mode == GUI_FX_RING ) ? border : 0.0f );

        for ( u32 k = 0; k < nbox; ++k, ++n )
        {
            for ( u32 c = 0; c < 4; ++c )
            {
                /* ax/ay ARE |p| at this corner -- the quadrant sign is applied only to the
                   position, which is why the effect coord needs no abs() in the shader. */
                f32 ax = lo_x[ k ] + qu[ c ] * ( hi_x[ k ] - lo_x[ k ] );
                f32 ay = lo_y[ k ] + qv[ c ] * ( hi_y[ k ] - lo_y[ k ] );
                f32 px = cx + sx[ q ] * ax, py = cy + sy[ q ] * ay;
                f32 tu = uhi > ulo ? u0 + ( u1 - u0 ) * ( px - x ) / w : u0;
                f32 tv = vhi > vlo ? v0 + ( v1 - v0 ) * ( py - y ) / h : v0;
                if ( tu < ulo ) tu = ulo;  if ( tu > uhi ) tu = uhi;
                if ( tv < vlo ) tv = vlo;  if ( tv > vhi ) tv = vhi;
                /* The turn, LAST: uv came from the unrotated position, the effect coord is
                   box-local either way, so only the world position rotates about the centre. */
                /* The ramp reads the box-LOCAL offset, which is the pre-rotation one -- so the
                   gradient turns with the box exactly as the uv and the effect coord do. */
                u32 vcol = grad ? tess_grad_col( grad, sx[ q ] * ax, sy[ q ] * ay ) : abgr;
                if ( rot != 0.0f )
                {
                    f32 dx = px - cx, dy = py - cy;
                    px = cx + dx * rcs - dy * rsn;
                    py = cy + dx * rsn + dy * rcs;
                }
                v[ n * 4 + c ] = gui_vert_fxc( px, py, tu, tv, vcol, ax - kx, ay - ky );
            }
            tess_quad_idx( &idx[ n * 6 ], (u16)( base + n * 4 ) );
        }

        /* Commit THIS quadrant's vertices under its own word.  tess_verts_commit stamps at the
           current vert_count and advances it, so successive chunks land on successive quads of the
           one reservation -- which is why the writes above still index `v` from the base. */
        s_tess.cur_fx = fx;
        tess_verts_commit( nbox * 4u );
    }

    tess_prim_commit_idx( ni );
}

/* The uniform-radius entry every rounded shape in the library goes through.  Four copies of one
   radius is not a workaround -- it is the honest statement that a rounded rect is the special case
   of a per-corner one, and it keeps a single tessellator for both. */
static void
tess_fx_box( f32 x, f32 y, f32 w, f32 h, f32 r, f32 feather, f32 border, f32 rate, f32 depth,
             f32 rot, gui_fx_mode_t mode,
             f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr )
{
    const f32 r4[ 4 ] = { r, r, r, r };
    tess_fx_box_core( x, y, w, h, r4, feather, border, rate, depth, rot, mode,
                      u0, v0, u1, v1, tex_idx, abgr, NULL );
}

/* TESS_FX_AA -- the default 1 px antialiasing band -- lives in gui_render.h now: the emit side
   bakes it into commands (a pulse's feather) as well. */

/* Tessellate a solid triangle into s_tess. */
static void
tess_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 3u, 3u, &wu, &wv, &v, &idx, &base ) )
        return;

    v[ 0 ] = gui_vert( ax, ay, wu, wv, abgr );
    v[ 1 ] = gui_vert( bx, by, wu, wv, abgr );
    v[ 2 ] = gui_vert( cx, cy, wu, wv, abgr );
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    tess_prim_commit( 3u, 3u );
}

/*==============================================================================================
    tess_circle_filled -- a disc, which is a rounded box whose radius reached its half-extent.

    There is no circle primitive anywhere in the pipeline and there does not need to be one:
    tess_fx_box clamps the corner radius to half the short side, so a SQUARE box asking for a
    radius of its own half-extent degenerates exactly to a disc -- same field, same four quadrant
    quads, same fragment, 16 verts / 24 idx at any size, antialiased.  The emit side agrees
    (draw_push_circle_filled emits GUI_CMD_RECT_FILLED with rounding = r); this helper survives
    for tess_fx_arc's full-turn PIE route.

    NOT grid-snapped, and it does not have to ask: tess_fx_box derives it -- a square whose radius
    reached its half-extent has no straight edge for snapping to keep crisp, and quantizing a
    circle's centre is exactly what a small moving dot must not do.  A circular RING satisfies the
    same test, so the two stay aligned when drawn concentrically.
==============================================================================================*/

static void
tess_circle_filled( f32 pcx, f32 pcy, f32 r, u32 abgr )
{
    tess_fx_box( pcx - r, pcy - r, r * 2.0f, r * 2.0f,
                 r, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f, GUI_FX_BOX,
                 0, 0, 1, 1, 0, abgr );
}

/*==============================================================================================
    tess_round_rect_ex -- a fill whose four corners have four different radii.

    The tab, the notch, the asymmetric card: shapes that used to walk a per-corner perimeter (up to
    72 sampled points) and fan it into as many separate TRIANGLE commands, with a polygonal boundary
    and no antialiasing at all.  Here it is the same 16 vertices a uniform rounded rect costs, and
    the boundary is exact, because a quadrant quad already sees exactly one corner -- the radii live
    in four packed words, not in the geometry.

        perimeter fan, 4 rounded corners   ~70 verts / ~200 idx, 62 commands, aliased
        the field                           16 verts /   24 idx,  1 command,  antialiased

    Solid colour and the standard 1 px AA band, because that is what the callers want -- not a
    limit of the field (tess_fx_box_core shows the quadrants agree exactly at any feather).
==============================================================================================*/

static void
tess_round_rect_ex( f32 x, f32 y, f32 w, f32 h,
                    f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather,
                    u32 abgr, u32 col_b, f32 grad_ang )
{
    /* Quadrant order: top-left, top-right, bottom-right, bottom-left (sx/sy in tess_fx_box_core),
       which is the order gui_cmd_t.round_rect declares its radii in.  feather below the standard
       AA band clamps up -- 0 means "crisp", never "hard-edged". */
    const f32 r4[ 4 ] = { rtl, rtr, rbr, rbl };

    /* Equal endpoints ARE a flat fill, so the ramp is skipped rather than special-cased: running
       it would land the same colour on every vertex, only after two transfer-curve round trips. */
    tess_grad_t grad;
    if ( col_b != abgr )
        tess_grad_init( &grad, abgr, col_b, grad_ang, w, h );

    tess_fx_box_core( x, y, w, h, r4, ( feather > TESS_FX_AA ) ? feather : TESS_FX_AA,
                      0.0f, 0.0f, 0.0f, 0.0f, GUI_FX_BOX,
                      0, 0, 1, 1, 0, abgr, ( col_b != abgr ) ? &grad : NULL );
}

/*==============================================================================================
    tess_fx_arc -- a circular sector, stroked (ARC) or filled (PIE), in ONE quad.

    The last sampled curve in the library.  An arc used to be up to 66 points from cos/sin fed to
    the polyline ribbon (~130 vertices, and a visible polygon at small radii where sym_arc_segs
    gives a 10 px mark ten segments); a pie fanned the same points from the centre, which cost 65
    separate TRIANGLE commands -- 6% of the entire per-frame command budget for one shape.

        spinner, r = 24    ~90 verts / ~260 idx, 1 cmd,  faceted, ribbon-AA
        pie,     r = 40    ~66 verts / ~195 idx, 65 cmds, faceted, no AA
        the field            4 verts /    6 idx,  1 cmd,  exact, antialiased

    ONE quad, where the rounded box needs four, because a circular shape subtracts no half-extent:
    its effect coordinate is the raw signed offset from the centre, which is affine everywhere, so
    nothing has to fold at the vertex (gui.h).  Keeping the sign is also the only reason an arc is
    expressible at all -- |p| would erase the angle.

    What the CPU does here is the per-shape work the fragment must not repeat: rotate the coordinate
    frame so the sector's bisector points +y.  That turns two absolute angles into one aperture (the
    shape is then symmetric about local x = 0, which the fragment folds itself) and it costs four
    vertices instead of every pixel.  The matrix is a reflection and its own inverse, so the same
    two lines map local -> world here as map world -> local conceptually.

    The quad is the sector's bounding box IN THAT LOCAL FRAME, not the circle's: a 90-degree arc
    covers about a quarter of the disc's area, so the fragment cost tracks the shape rather than the
    circle it belongs to.  A full turn is not a sector at all and routes to the exact ring / disc
    primitives instead -- cheaper, and it sidesteps the aperture = pi degenerate.
==============================================================================================*/

#define TESS_HALF_PI  1.57079632679490f
#define TESS_TAU      6.28318530717959f

/* `mode` is one of the four sector modes: GUI_FX_ARC / GUI_FX_PIE take the classic path, and the
   two SELF-SAMPLED variants (GUI_FX_ARC_DASH / GUI_FX_ARC_GRAD) additionally carry (uvx, uvy) --
   the parameter pair the fragment recovers from the quad's flat uv word (gui.h).  ARC/PIE ignore
   the pair and stamp the white texel as every solid shape does. */
static void
tess_fx_arc( f32 pcx, f32 pcy, f32 r, f32 thickness, f32 a0, f32 a1,
             gui_fx_mode_t mode, f32 uvx, f32 uvy, u32 abgr )
{
    if ( r <= 0.0f )
        return;

    bool pie = ( mode == GUI_FX_PIE );

    /* Normalize the sweep so the bisector/aperture split below is always well formed.  A reversed
       range is the same sector drawn the other way round, which for a symmetric shape is the same
       sector.  (The gradient is NOT symmetric; its emit side pre-normalizes and swaps the colours,
       so by here every reversed range really is harmless.) */
    f32 sweep = a1 - a0;
    if ( sweep < 0.0f ) { f32 t = a0; a0 = a1; a1 = t; sweep = -sweep; }
    if ( sweep <= 0.0f )
        return;
    if ( sweep > TESS_TAU ) { sweep = TESS_TAU; a1 = a0 + TESS_TAU; }

    /* A full turn is not a sector, and the exact primitives are cheaper: a PIE is a disc, and an
       ARC is a closed ring whose interior GUI_FX_RING carves away -- worth real fragments on a
       large one.  The ring is only taken when the band FITS ITS BORDER FIELD, which caps lower
       (GUI_FX_BORDER_MAX) than the tube field a sector carries; a thicker band falls through rather
       than silently drawing thinner.  Falling through is correct, not a fallback: at aperture pi the
       sector formula is the exact full annulus, it merely rasterizes the hole as well.
       This is reachable -- draw_progress_arc at 100% is exactly a full sweep.
       The SELF-SAMPLED variants never reroute: their pattern / gradient lives in the sector decode,
       which the exact ring does not run -- and at aperture pi the sector formula serves them
       exactly, so a closed dashed ring is this same one quad. */
    if ( sweep >= TESS_TAU && ( mode == GUI_FX_ARC || mode == GUI_FX_PIE ) )
    {
        if ( pie )
        {
            tess_circle_filled( pcx, pcy, r, abgr );
            return;
        }
        if ( thickness <= GUI_FX_BORDER_MAX )
        {
            /* The same shape draw_circle's unfilled path asks for, measured from the OUTER boundary
               inward -- so the band still straddles r. */
            f32 outer = r + thickness * 0.5f;
            tess_fx_box( pcx - outer, pcy - outer, outer * 2.0f, outer * 2.0f,
                         outer, TESS_FX_AA, thickness, 0.0f, 0.0f, 0.0f, GUI_FX_RING,
                         0, 0, 1, 1, 0, abgr );
            return;
        }
    }

    /* Clamp to what the word can carry, on BOTH sides as tess_fx_box does: the box below is sized
       from these same numbers, so a value the fragment never sees would leave the vertices
       describing a different shape than the one it resolves. */
    f32 ra = r;
    if ( ra > GUI_FX_RADIUS_MAX ) ra = GUI_FX_RADIUS_MAX;
    f32 rb = pie ? 0.0f : thickness * 0.5f;
    if ( rb < 0.0f ) rb = 0.0f;
    if ( rb > GUI_FX_ARC_TUBE_MAX ) rb = GUI_FX_ARC_TUBE_MAX;

    f32 am = ( a0 + a1 ) * 0.5f;          /* the bisector, which becomes local +y */
    f32 ap = sweep * 0.5f;                /* the half-aperture measured from it   */
    f32 sm = sinf( am ), cm = cosf( am );
    f32 sa = sinf( ap ), ca = cosf( ap );

    /* The sector's bounding box in local space.  x is bounded by the widest point of the sweep
       (sin saturates at 1 once the aperture passes a quarter turn) plus the tube; y runs from the
       far edge of the sweep up to the bisector's own rim.  A PIE also contains its centre, which a
       narrow sweep's box would otherwise sit entirely above. */
    f32 pad  = TESS_FX_AA * 0.5f + 1.0f;
    f32 xext = ( ( ap >= TESS_HALF_PI ) ? ra : ra * sa ) + rb + pad;
    f32 ymax = ra + rb + pad;
    f32 ymin = ra * ca - rb - pad;
    if ( pie && ymin > -pad )
        ymin = -pad;

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;

    s_tess.cur_fx = gui_fx_pack_arc( mode, ra, rb, ap );

    /* These two carry their parameter pair in the uv lanes instead of texcoords, which is what the
       self-sampled bit announces: the fragment forces coverage to 1 and never reads the texel, so
       the white texel is not needed and the atlas stays bound only to keep the index valid. */
    if ( mode == GUI_FX_ARC_DASH || mode == GUI_FX_ARC_GRAD )
    {
        wu = uvx;
        wv = uvy;
        s_tess.cur_tex_bits |= GUI_TEX_SELF_BIT;
    }

    static const f32 lsx[ 4 ] = { -1.0f, 1.0f, 1.0f, -1.0f };
    static const u32 lsy[ 4 ] = {  0u,   0u,   1u,   1u   };

    for ( u32 i = 0; i < 4; ++i )
    {
        f32 lx = lsx[ i ] * xext;
        f32 ly = lsy[ i ] ? ymax : ymin;
        /* local -> world.  Reflection + rotation; the pipeline does not cull, so the winding this
           flips for bisectors in the lower half plane costs nothing. */
        f32 px = pcx - sm * lx + cm * ly;
        f32 py = pcy + cm * lx + sm * ly;
        v[ i ] = gui_vert_fxc( px, py, wu, wv, abgr, lx, ly );
    }
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/*==============================================================================================
    tess_checker / tess_grid -- the framebuffer-tiling pattern quads.

    ONE quad each: the fragment computes the pattern from gl_FragCoord / SV_Position, not from
    the effect coordinate, and the reason is precision where these shapes actually live.  A
    backdrop is the one shape that reaches fullscreen, and there the HALF2 coordinate's ulp is a
    full pixel at the far corners -- a fine lattice line would land half a pixel wrong and blur.
    The rasterizer's own pixel coordinate is exact everywhere at any size.  (It also means the
    pattern assumes the pixel-space ortho mvp, which is the only mvp this pipeline has.)

    The CPU's share is the ANCHOR: quantize the cell pitch EXACTLY as the packed word carries it
    (1/4 px), then derive the phase against that quantized pitch -- deriving it against the raw
    pitch would let phase and pitch disagree by up to 1/8 px per cell, which walks the pattern
    off its anchor across a wide panel.  The checker's phase is a fraction of the TWO-cell
    colour period (one cell of phase would swap the colours); the grid's is a fraction of one
    cell, in the uv lanes the single-colour lattice leaves free.
==============================================================================================*/

static void
tess_checker( f32 x, f32 y, f32 w, f32 h, f32 cell, u32 col_a, u32 col_b )
{
    /* Snap like tess_rect_filled: the pattern anchors at the box origin, so the box must land
       where the plain fill under it does. */
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_snap_cell( cell );

    f32 period = 2.0f * cell;
    f32 phx    = ( x - period * floorf( x / period ) ) / period;
    f32 phy    = ( y - period * floorf( y / period ) ) / period;

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;

    s_tess.cur_fx = gui_fx_pack_checker( cell, phx, phy );

    /* col_b in the ARC_GRAD uv lanes; the fragment never samples there (self-sampled, gui.h). */
    wu = (f32)(   col_b         & 0xFFFFu ) / 65535.0f;
    wv = (f32)( ( col_b >> 16 ) & 0xFFFFu ) / 65535.0f;
    s_tess.cur_tex_bits |= GUI_TEX_SELF_BIT;

    v[ 0 ] = gui_vert( x,     y,     wu, wv, col_a );
    v[ 1 ] = gui_vert( x + w, y,     wu, wv, col_a );
    v[ 2 ] = gui_vert( x + w, y + h, wu, wv, col_a );
    v[ 3 ] = gui_vert( x,     y + h, wu, wv, col_a );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

static void
tess_grid( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 cell, f32 thickness, u32 abgr )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_snap_cell( cell );

    /* The lattice anchor, mod the quantized pitch.  (ox, oy) is a screen-space content origin
       and may be anywhere (a panned canvas sends large negatives); only its residue matters. */
    f32 phx = ( ox - cell * floorf( ox / cell ) ) / cell;
    f32 phy = ( oy - cell * floorf( oy / cell ) ) / cell;

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;

    s_tess.cur_fx = gui_fx_pack_grid( cell, thickness );

    /* The lattice is one colour -- the vertex colour -- so the uv word is free to carry the
       per-axis phase instead of texcoords (self-sampled, gui.h). */
    wu = phx;
    wv = phy;
    s_tess.cur_tex_bits |= GUI_TEX_SELF_BIT;

    v[ 0 ] = gui_vert( x,     y,     wu, wv, abgr );
    v[ 1 ] = gui_vert( x + w, y,     wu, wv, abgr );
    v[ 2 ] = gui_vert( x + w, y + h, wu, wv, abgr );
    v[ 3 ] = gui_vert( x,     y + h, wu, wv, abgr );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/* Tessellate a glyph run from the font atlas into s_tess, hard-clipped to the horizontal pixel
   window [clip_x0, clip_x1].  Glyphs fully outside the window are skipped; glyphs fully inside emit
   whole; the (at most two) straddling glyphs are cut on a pixel boundary with their U remapped by
   the same fraction -- exact, since the glyph quad is an axis-aligned 1:1 atlas sample.  The window
   is monotonic with the left-to-right cursor, so interior glyphs pay only one compare: no clip math.
   The unclipped sentinel (clip_x1 >= GUI_TEXT_NO_CLIP) takes the original whole-run fast path. */
static void
tess_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    bool clipped = ( clip_x1 < GUI_TEXT_NO_CLIP );
    f32  cx      = x;

    /* Hoisted: the active font cannot change mid-run, and this carries the sampling model, so it is
       also what keeps a distance-field run in its own batch without the batcher knowing why. */
    u32  tex = font_tex();
    if ( tex == 0 )
        return;                       /* the font's atlas is not up yet -- nothing to sample */

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( cp, &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
        {
            f32 gx0 = cx + ox;          /* glyph bitmap left/right in screen px */
            f32 gx1 = gx0 + gw;

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                /* Whole glyph (or no clipping): emit as-is -- the hot interior path. */
                tess_rect_filled( gx0, y + oy, gw, gh, u0, v0, u1, v1, tex, abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                /* Straddler: cut to the window and walk U by the same fraction on each cut edge. */
                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                if ( nx0 < clip_x0 )    /* left edge cut  */
                {
                    nu0 = u0 + du * ( ( clip_x0 - gx0 ) / gw );
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )    /* right edge cut */
                {
                    nu1 = u0 + du * ( ( clip_x1 - gx0 ) / gw );
                    nx1 = clip_x1;
                }
                tess_rect_filled( nx0, y + oy, nx1 - nx0, gh, nu0, v0, nu1, v1, tex, abgr );
            }
            /* else: glyph wholly outside the window -- drop it. */
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )   /* cursor past the window: nothing further is visible */
            break;
    }
}

/* One textured quad placed by an affine map: the local rect (lx, ly, lw, lh) is rotated by the
   prebuilt (cs, sn) and translated to the run origin (px, py).  Same four vertices and two
   triangles tess_rect_filled writes -- and one thing it does NOT do: SNAP.  tess_rect_filled
   floors the origin to the pixel grid so straight edges stay crisp, which is right for chrome and
   wrong here twice over.  Snapping only the origin of a rotated quad moves the whole shape without
   straightening anything, and snapping a scaled run's per-glyph origins quantizes the advances --
   the pen drifts by up to half a pixel per glyph and the word visibly breathes as the scale
   animates.  A transformed run is sub-pixel by nature; the distance field is what makes that
   legible (gui.h, GUI_TEX_SDF). */
static void
tess_quad_xf( f32 px, f32 py, f32 cs, f32 sn,
              f32 lx, f32 ly, f32 lw, f32 lh,
              f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr )
{
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin_tex( 4u, 6u, tex_idx, &v, &idx, &base ) )
        return;

    f32 qx[ 4 ] = { lx,      lx + lw, lx + lw, lx      };
    f32 qy[ 4 ] = { ly,      ly,      ly + lh, ly + lh };
    f32 qu[ 4 ] = { u0,      u1,      u1,      u0      };
    f32 qv[ 4 ] = { v0,      v0,      v1,      v1      };

    for ( u32 i = 0; i < 4; ++i )
        v[ i ] = gui_vert( px + qx[ i ] * cs - qy[ i ] * sn,
                           py + qx[ i ] * sn + qy[ i ] * cs,
                           qu[ i ], qv[ i ], abgr );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/* Tessellate a glyph run under a uniform scale and a rotation about its origin (the text_xf
   command).  The run is laid out in its OWN space -- pen at 0, the font's unscaled advances -- and
   the whole of it is mapped once per glyph quad, so the transform never accumulates: 200 glyphs in
   and the pen is still exactly `sum(advance) * scale` from the origin along the rotated axis.

   Nothing about the ATLAS side changes: the same font_glyph UVs, the same tex, the same batch key
   as the 1:1 path, so a rotated run merges into the very same draw call as the upright text beside
   it as long as both are in the same font.  What makes it LOOK right rather than merely be placed
   right is the sampling model -- a coverage font is point-sampled and will show its texels here,
   while a distance-field font resolves its edge in the fragment from a screen-space derivative and
   is therefore indifferent to both the scale and the angle. */
static void
tess_text_xf( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 scale, f32 rot )
{
    u32 tex = font_tex();
    if ( tex == 0 || scale <= 0.0f )
        return;

    f32 cs = cosf( rot ), sn = sinf( rot );
    f32 pen = 0.0f;                      /* run-local, UNSCALED: scale is applied at the map */

    u32 i = 0;
    while ( i < n && str[ i ] )
    {
        u32 adv_b;
        u32 cp = utf8_decode( &str[ i ], &adv_b );
        i += adv_b;

        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( cp, &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
            tess_quad_xf( x, y, cs, sn,
                          ( pen + ox ) * scale, oy * scale, gw * scale, gh * scale,
                          u0, v0, u1, v1, tex, abgr );

        pen += advance;
    }
}

/* Tessellate a dashed / dotted line as one oriented textured quad sampling the atlas dash row.
   U spans 0..len/period so the row tiles along the line under REPEAT-U addressing; V selects the
   baked row whose on-fraction is closest to `duty`.  O(1) geometry regardless of line length --
   the per-dash quad explosion (which used to exhaust the command list) is gone. */
static void
tess_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 period, f32 duty, u32 abgr )
{
    if ( thickness <= 0.0f || period <= 0.0f )
        return;
    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;
    f32 inv  = 1.0f / len;
    f32 ux   = dx * inv, uy = dy * inv;          /* unit vector along the line  */
    f32 nx   = -uy,      ny = ux;                /* unit normal across the line */
    f32 half = thickness * 0.5f;
    f32 umax = len / period;                     /* number of tiled periods -> U span */
    f32 vv   = res_atlas_dash_v( duty );

    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin_tex( 4u, 6u, res_atlas_idx(), &v, &idx, &base ) )
        return;

    /* U runs 0..1 in the VERTEX and is multiplied back up to `umax` periods by the vertex stage:
       the packed UV cannot hold a coordinate past 1, and the sampler's REPEAT-U is what tiles the
       atlas dash row (gui.h, GUI_FX_TILE_U).  Interpolation is unaffected -- both ends are stored
       exactly and a lerp commutes with the scale. */
    s_tess.cur_fx = gui_fx_pack_tile_u( umax );
    v[ 0 ] = gui_vert( x0 + nx * half, y0 + ny * half, 0.0f, vv, abgr );
    v[ 1 ] = gui_vert( x1 + nx * half, y1 + ny * half, 1.0f, vv, abgr );
    v[ 2 ] = gui_vert( x1 - nx * half, y1 - ny * half, 1.0f, vv, abgr );
    v[ 3 ] = gui_vert( x0 - nx * half, y0 - ny * half, 0.0f, vv, abgr );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/*==============================================================================================
    tess_stroke_poly_aa -- antialiased polyline tessellation for the render backend.

    Mirrors the old stroke_poly_aa but writes into
    s_tess via tess_prim_begin/commit.  abgr is pre-baked (alpha folded in at emit time).
    v2 / seg_normal / stroke_center_offset are defined in gui_emit_path.c (included before
    this file in the unity build) so they are visible here without forward declarations.
==============================================================================================*/

static void
tess_stroke_poly_aa( const gui_vec2_t* pts, u32 n, f32 thickness, f32 center_off,
                     bool closed, u32 abgr )
{
    if ( n < 2 )
        return;
    if ( n > GUI_MAX_PATH_PTS )
         n = GUI_MAX_PATH_PTS;

    /* Sub-pixel coverage: hold a 1px footprint, fade peak alpha by the requested thickness. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )
    {
        a_scale   = thickness < 0.0f ? 0.0f : thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );   /* inner / solid color */
    u32 col0 = ( abgr & 0x00FFFFFFu );                     /* outer feather, alpha 0 */

    f32 half     = thickness * 0.5f;
    f32 core_min = ( half < STROKE_CORE_MIN ) ? half : STROKE_CORE_MIN;
    f32 inner    = half - STROKE_AA;
    if ( inner < core_min )
        inner = core_min;

    u32 seg = closed ? n : n - 1;

    /* Per-point miter normal; static avoids an 8K+ stack frame. Single-threaded. */
    static gui_vec2_t nrm[ GUI_MAX_PATH_PTS ];
    for ( u32 i = 0; i < n; ++i )
    {
        gui_vec2_t n0, n1;
        if ( closed )
        {
            n0 = seg_normal( pts[ ( i + n - 1 ) % n ], pts[ i ] );
            n1 = seg_normal( pts[ i ], pts[ ( i + 1 ) % n ] );
        }
        else
        {
            n0 = ( i > 0 )     ? seg_normal( pts[ i - 1 ], pts[ i ] ) : v2( 0.0f, 0.0f );
            n1 = ( i < n - 1 ) ? seg_normal( pts[ i ], pts[ i + 1 ] ) : v2( 0.0f, 0.0f );
        }

        if ( !closed && i == 0 )          nrm[ i ] = n1;
        else if ( !closed && i == n - 1 ) nrm[ i ] = n0;
        else
        {
            gui_vec2_t dm = v2( ( n0.x + n1.x ) * 0.5f, ( n0.y + n1.y ) * 0.5f );
            f32 d2 = dm.x * dm.x + dm.y * dm.y;
            if ( d2 > 1e-6f )
            {
                f32 inv = 1.0f / d2;
                if ( inv > 100.0f ) inv = 100.0f;   /* miter limit */
                dm.x *= inv; dm.y *= inv;
                nrm[ i ] = dm;
            }
            else { nrm[ i ] = n1; }
        }
    }

    u32 nv = 4u * n, ni = 18u * seg;
    f32 wu, wv;
    gui_draw_vert_t* v;
    u16* idx;
    u16  base;
    if ( !tess_prim_begin( nv, ni, &wu, &wv, &v, &idx, &base ) )
        return;

    for ( u32 i = 0; i < n; ++i )
    {
        gui_vec2_t m  = nrm[ i ];
        f32          cx = pts[ i ].x + m.x * center_off;
        f32          cy = pts[ i ].y + m.y * center_off;
        v[ 4*i+0 ] = gui_vert( cx + m.x*half,  cy + m.y*half,  wu, wv, col0 );
        v[ 4*i+1 ] = gui_vert( cx + m.x*inner, cy + m.y*inner, wu, wv, col  );
        v[ 4*i+2 ] = gui_vert( cx - m.x*inner, cy - m.y*inner, wu, wv, col  );
        v[ 4*i+3 ] = gui_vert( cx - m.x*half,  cy - m.y*half,  wu, wv, col0 );
    }

    static const int band[ 3 ][ 2 ] = { { 0, 1 }, { 1, 2 }, { 2, 3 } };
    u32 k = 0;
    for ( u32 s = 0; s < seg; ++s )
    {
        u16 i0 = (u16)( base + 4u * s );
        u16 i1 = (u16)( base + 4u * ( ( s + 1 ) % n ) );
        for ( int q = 0; q < 3; ++q )
        {
            u16 a0 = (u16)( i0 + band[q][0] ), a1 = (u16)( i0 + band[q][1] );
            u16 b0 = (u16)( i1 + band[q][0] ), b1 = (u16)( i1 + band[q][1] );
            idx[k++]=a0; idx[k++]=a1; idx[k++]=b1;
            idx[k++]=a0; idx[k++]=b1; idx[k++]=b0;
        }
    }
    tess_prim_commit( nv, ni );
}

/*==============================================================================================
    tess_fx_segment -- one line segment as a CAPSULE distance field.

    The diagonal stroke, resolved by the fragment instead of approximated in geometry.  What the
    ribbon stroker above does for a single segment is lay three bands across it -- a solid core and
    two outer bands dropped to alpha 0 -- so the hardware's colour interpolation fakes an edge
    gradient.  That is 18 indices, a fixed one-pixel falloff, and square butt caps, and it is an
    approximation on both axes at once.  A capsule is the exact same shape written down: the
    distance from a point to a segment, minus the half-thickness.  Two quads, 12 indices, an edge
    that is correct at any angle, and round caps that cost nothing because they ARE the field.

    TWO quads, not the box's four: the fold is forced by the SUBTRACTION of a half-extent (gui.h),
    and a capsule subtracts only along its axis.  The across-axis offset rides signed and the
    fragment squares it inside length().

    Round caps extend half a thickness past each endpoint, where the ribbon stopped square.  That
    is a real visual change, it is what a stroke is supposed to look like, and at the widths this
    path sees (diagonal hairlines and connector wires) it is a sub-pixel difference.

    Only DIAGONALS come here.  Axis-aligned lines keep the grid-snapped quad below -- 4 verts, 6
    indices, and crisper than any field, since a horizontal edge has nothing to antialias.  And
    POLYLINES keep the ribbon: N capsules would overlap at every joint, and two overlapping
    translucent strokes composite darker than one, so a semi-transparent path would grow a bead at
    each vertex.  The ribbon's miter solve exists precisely to emit each pixel once.
==============================================================================================*/

static void
tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr )
{
    if ( thickness <= 0.0f )
        return;

    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;

    /* Sub-pixel coverage, matched to the ribbon stroker rather than left to the field: hold a 1 px
       footprint and fade peak alpha.  The field would happily render a 0.4 px capsule, but it would
       weigh a hairline differently than every other line in the library, and consistency across the
       two paths is worth more here than the extra correctness. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )                 /* thickness > 0 by the guard above */
    {
        a_scale   = thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    f32 inv = 1.0f / len;
    f32 ux  = dx * inv, uy = dy * inv;      /* unit vector along the segment  */
    f32 nx  = -uy,      ny = ux;            /* unit normal across it          */
    f32 r   = thickness * 0.5f;             /* the capsule radius             */
    /* Clamped for the same reason tess_fx_box clamps its radius: the geometry below is sized from
       `r`, so the fragment's saturated copy of it has to be the same number. */
    if ( r > GUI_FX_RADIUS_MAX ) r = GUI_FX_RADIUS_MAX;
    f32 hl  = len * 0.5f;                   /* half-length: what q.x subtracts */
    f32 mx  = ( x0 + x1 ) * 0.5f, my = ( y0 + y1 ) * 0.5f;

    /* Geometry must clear the boundary by the falloff plus a pixel of slack, exactly as the box
       surface does -- the round cap needs the radius on top, since the cap bulges past the end. */
    f32 pad = TESS_FX_AA * 0.5f + 1.0f;
    f32 ea  = hl + r + pad;                 /* extent along the axis, from the midpoint */
    f32 ec  = r + pad;                      /* extent across it                         */

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 8u, 12u, &wu, &wv, &v, &idx, &base ) )
        return;
    s_tess.cur_fx = gui_fx_pack( GUI_FX_SEG, r, TESS_FX_AA, 0.0f );

    /* Corner of one quad in its own half: `a` runs 0 -> ea away from the midpoint (so |along| is
       simply a, and its sign is constant over the quad -- the whole reason for the split), `b`
       spans the full width signed. */
    static const f32 qa[ 4 ] = {  0.0f, 1.0f, 1.0f,  0.0f };
    static const f32 qb[ 4 ] = { -1.0f, -1.0f, 1.0f, 1.0f };

    for ( u32 s = 0; s < 2; ++s )
    {
        f32 sa = ( s == 0 ) ? 1.0f : -1.0f;     /* which side of the midpoint this quad covers */
        for ( u32 c = 0; c < 4; ++c )
        {
            f32 a = qa[ c ] * ea, b = qb[ c ] * ec;
            v[ s * 4 + c ] = gui_vert_fxc( mx + sa * a * ux + b * nx,
                                           my + sa * a * uy + b * ny,
                                           wu, wv, col, a - hl, b );
        }
        tess_quad_idx( &idx[ s * 6 ], (u16)( base + s * 4u ) );
    }

    tess_prim_commit( 8u, 12u );
}

/* Volatile-widget seam (render/pipeline/gui_build_volatile.c, included right after this file in
   the gui_render.c unity build).  tess_dispatch calls volatile_range_close once a tagged command
   RANGE's vertices/indices/GPU commands are fully written; it records the block's slot-relative
   position, reserves padded headroom past the live geometry (advancing this file's write heads),
   and stamps the slot tessellation generation.  s_volatile_patching is defined HERE (first in the
   TU) and set by volatile_patch around its scratch re-tessellation so the range tracking below
   stays inert during a patch -- a patch must never look like a fresh capture. */
static bool s_volatile_patching;
static void volatile_range_close( gui_id_t id, u32 vb_open, u32 ib_open, u32 cmd_open );

/* Tessellate one frame's semantic command list into s_tess geometry.

   `order` is a permutation of [0,count): the window's visible commands in emission order (built
   by cache_tess_window; clip-empty commands are already dropped).  Nothing about clips shapes it
   any more -- the clip rides the vertex (the clip band) and cannot cut a draw call.
   `win` is the window being tessellated (informational; volatile rows already know their window
   from emit-time stamping).

   The FONT is activated per TEXT COMMAND, from the command's own font id, and only by the two cases
   that read glyphs.  It used to arrive as a `fonts[]` array parallel to `order` -- one entry per
   ordered command, reconstructing a per-segment property after the clip sort had torn the segments
   apart -- and it used to be switched at the top of this loop for every command, text or not.
   Neither was needed: a fill, a line and a sprite never call font_glyph, and the font is per-command
   data now (gui.h).  Activating it changes which atlas the glyph lookups resolve from and nothing
   else; it does NOT split the GPU batch, since it only alters the word tess_set_tex stamps into the
   following vertices.  A bitmap label, an SDF heading and the fill behind them still go out as one
   draw call.  The active font is saved and restored so the BUILD phase leaves the global font state
   (used by the next frame's layout) untouched. */
static void
tess_dispatch( const gui_cmd_t* cmds, const u16* order, u32 count, gui_id_t win )
{
    u32 saved_font = font_active_id();
    u32 cur_font   = saved_font;

    /* Volatile-widget range tracking: cmd_volatile_id tags a contiguous RANGE of commands (not
       just one), so bracket [vb_open, ...) / [ib_open, ...) / [cmd_open, ...) while the tag stays
       the same and hand the finished range to volatile_range_close when it changes (or at the
       end).  force_new_cmd is raised when a range OPENS so the block's geometry lands in its own
       fresh GPU command(s), never merged with a neighbour's -- the block's elem_counts must stay
       independently rewritable by a later patch.  Tracking is inert during a patch's own scratch
       re-tessellation (s_volatile_patching). */
    gui_id_t open_vid = GUI_ID_NONE;
    u32      vb_open = 0, ib_open = 0, cmd_open = 0;
    (void)win;

    for ( u32 oi = 0; oi < count; ++oi )
    {
        u32              ci = order[ oi ];
        const gui_cmd_t* c  = &cmds[ ci ];

        gui_id_t vid = s_volatile_patching ? GUI_ID_NONE : s_draw.cmd_volatile_id[ ci ];
        if ( vid != open_vid )
        {
            if ( open_vid != GUI_ID_NONE )
                volatile_range_close( open_vid, vb_open, ib_open, cmd_open );
            open_vid = vid;
            if ( vid != GUI_ID_NONE )
            {
                s_tess.force_new_cmd = true;   /* block owns its GPU commands from the first primitive */
                vb_open  = s_tess.vert_count;
                ib_open  = s_tess.idx_count;
                cmd_open = s_tess.cmd_count;
            }
        }

        s_tess.cur_clip       = s_draw.clip_table[ c->clip_idx ];
        s_tess.cur_clip_local = tess_clip_local( c->clip_idx );
        s_tess.cur_vp         = c->vp;

        /* The effect word is ambient over ONE command and cleared here, so a case that sets it
           cannot leak the effect onto the next primitive.  That containment is the whole reason it
           can be ambient at all -- it lets an outline reach every glyph of a run, and a shape's
           word reach all 16 of its quadrant vertices, without threading a parameter through
           tess_rect_filled, which every fill in the library shares. */
        s_tess.cur_fx       = 0u;
        s_tess.cur_tex_bits = 0u;

        switch ( c->type )
        {
            /* A square rect keeps the one-quad fast path: it is pixel-aligned by construction, so
               there is no edge for an SDF to resolve and nothing to gain.  Rounding is what turns
               it into a surface -- and routing the TEXTURED case through as well is what finally
               lets a rounded quad carry an image, which the arc fan never could. */
            case GUI_CMD_RECT_FILLED:
                if ( c->rect.rounding > 0.0f )
                    tess_fx_box( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                 c->rect.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f, GUI_FX_BOX,
                                 c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                 c->rect.tex_idx, c->rect.abgr );
                else
                    tess_rect_filled( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                      c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                      c->rect.tex_idx, c->rect.abgr );
                break;

            /* RING measures from the OUTER boundary inward, matching the square path's INSIDE
               band (and the closed AA stroke this replaced). */
            case GUI_CMD_RECT_OUTLINE:
                if ( c->rect_outline.rounding > 0.0f )
                    tess_fx_box( c->rect_outline.x, c->rect_outline.y,
                                 c->rect_outline.w, c->rect_outline.h,
                                 c->rect_outline.rounding, TESS_FX_AA, c->rect_outline.t,
                                 0.0f, 0.0f, 0.0f, GUI_FX_RING,
                                 0, 0, 1, 1, 0, c->rect_outline.abgr );
                else
                    tess_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                       c->rect_outline.w, c->rect_outline.h,
                                       c->rect_outline.t, c->rect_outline.abgr );
                break;

            /* The parameterized surface: a shadow is the wide feather (what used to be six
               stacked rects pretending to be a gaussian is the exact same falloff the corners
               use, only spread out), a pulse the shader-clock word -- geometrically a plain
               rounded fill whose vertices are correct for every frame it runs, so the retained
               slot never invalidates and the breathing costs no re-tessellation.  rate > 0 IS
               the mode: a still box and a zero-rate pulse are the same shape to the fragment. */
            case GUI_CMD_FX_BOX:
                /* The INSET is a plain BOX whose falloff the op turns inward, so it names no mode
                   of its own -- which is the point of an op band.  Set before the tessellator runs
                   because the interior hole is sized from it (see `reach`). */
                if ( c->fx_box.variant == 2u )
                    s_tess.cur_tex_bits |= GUI_TEX_OP_INSET;
                tess_fx_box( c->fx_box.x, c->fx_box.y, c->fx_box.w, c->fx_box.h,
                             c->fx_box.rounding, c->fx_box.feather, 0.0f,
                             c->fx_box.rate, c->fx_box.depth, c->fx_box.rot,
                             ( c->fx_box.rate > 0.0f )    ? GUI_FX_PULSE
                           : ( c->fx_box.variant == 1u ) ? GUI_FX_SKIRT
                                                         : GUI_FX_BOX,
                             0, 0, 1, 1, 0, c->fx_box.abgr );
                break;

            /* Four quadrants, four radii, four words -- and still one surface, one command and no
               batch split, exactly like the uniform fill it generalizes. */
            case GUI_CMD_ROUND_RECT_EX:
                tess_round_rect_ex( c->round_rect.x, c->round_rect.y,
                                    c->round_rect.w, c->round_rect.h,
                                    c->round_rect.rtl, c->round_rect.rtr,
                                    c->round_rect.rbr, c->round_rect.rbl,
                                    c->round_rect.feather, c->round_rect.abgr,
                                    c->round_rect.col_b, c->round_rect.grad_ang );
                break;

            /* The sectors share their geometry and differ only in the field the fragment
               evaluates: round caps on a band, sharp radial edges on a wedge, and the two
               self-sampled variants whose extra word rides the quad's flat uv. */
            case GUI_CMD_ARC:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, c->arc.thickness,
                             c->arc.a0, c->arc.a1, GUI_FX_ARC, 0.0f, 0.0f, c->arc.abgr );
                break;

            case GUI_CMD_PIE:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, 0.0f,
                             c->arc.a0, c->arc.a1, GUI_FX_PIE, 0.0f, 0.0f, c->arc.abgr );
                break;

            /* uv lane packing is the shader contract for the self-sampled pair (gui.h): DASH
               sends (period / TAU, duty), GRAD splits col_b's four bytes across the two unorm16
               lanes.  Both values are k/65535 exact through the pack and back. */
            case GUI_CMD_ARC_DASH:
                tess_fx_arc( c->arc_dash.cx, c->arc_dash.cy, c->arc_dash.r,
                             c->arc_dash.thickness, c->arc_dash.a0, c->arc_dash.a1,
                             GUI_FX_ARC_DASH,
                             c->arc_dash.period / TESS_TAU, c->arc_dash.duty,
                             c->arc_dash.abgr );
                break;

            case GUI_CMD_ARC_GRAD:
                tess_fx_arc( c->arc_grad.cx, c->arc_grad.cy, c->arc_grad.r,
                             c->arc_grad.thickness, c->arc_grad.a0, c->arc_grad.a1,
                             GUI_FX_ARC_GRAD,
                             (f32)(   c->arc_grad.col_b         & 0xFFFFu ) / 65535.0f,
                             (f32)( ( c->arc_grad.col_b >> 16 ) & 0xFFFFu ) / 65535.0f,
                             c->arc_grad.col_a );
                break;

            /* The framebuffer-tiling patterns: the fragment does the tiling, the CPU's share is
               the quantized pitch + anchor phase (see tess_checker). */
            case GUI_CMD_CHECKER:
                tess_checker( c->checker.x, c->checker.y, c->checker.w, c->checker.h,
                              c->checker.cell, c->checker.col_a, c->checker.col_b );
                break;

            case GUI_CMD_GRID:
                tess_grid( c->grid.x, c->grid.y, c->grid.w, c->grid.h,
                           c->grid.ox, c->grid.oy, c->grid.cell, c->grid.thickness,
                           c->grid.abgr );
                break;

            /* One textured quad about its centre -- the glyph-run transform (tess_quad_xf)
               with the pivot every icon caller wants.  No snap, by the transformed-quad rule. */
            case GUI_CMD_IMAGE_XF:
            {
                f32 hx = c->image_xf.w * 0.5f, hy = c->image_xf.h * 0.5f;
                tess_quad_xf( c->image_xf.x + hx, c->image_xf.y + hy,
                              cosf( c->image_xf.rot ), sinf( c->image_xf.rot ),
                              -hx, -hy, c->image_xf.w, c->image_xf.h,
                              c->image_xf.u0, c->image_xf.v0, c->image_xf.u1, c->image_xf.v1,
                              c->image_xf.tex_idx, c->image_xf.abgr );
                break;
            }

            case GUI_CMD_TRIANGLE:
                tess_triangle( c->tri.ax, c->tri.ay, c->tri.bx, c->tri.by,
                               c->tri.cx, c->tri.cy, c->tri.abgr );
                break;

            /* The outline word is set once for the whole run: every glyph quad the loop emits
               carries it, and the fragment resolves fill and outline from the one distance field
               it was already sampling. */
            /* The only two cases that read glyphs, and therefore the only two that care which font
               is active.  Guarded on a change rather than set unconditionally because a run of
               labels in one font is the overwhelmingly common case and font_use rebuilds metrics. */
            case GUI_CMD_TEXT:
                if ( c->text.font != cur_font )
                    font_use( cur_font = c->text.font );
                s_tess.cur_fx = c->text.edge;
                tess_text_n( c->text.x, c->text.y, c->text.abgr, s_draw.text_pool + c->text.off,
                             c->text.len, c->text.clip_x0, c->text.clip_x1 );
                break;

            case GUI_CMD_TEXT_XF:
                if ( c->text_xf.font != cur_font )
                    font_use( cur_font = c->text_xf.font );
                s_tess.cur_fx = c->text_xf.edge;
                tess_text_xf( c->text_xf.x, c->text_xf.y, c->text_xf.abgr,
                              s_draw.text_pool + c->text_xf.off, c->text_xf.len,
                              c->text_xf.scale, c->text_xf.rot );
                break;

            /* Always a CAPSULE, because only diagonals ever arrive: gui_draw_line routes every
               axis-aligned segment through a grid-snapped rect at EMIT (stroke_axis_aligned_rect,
               gui_emit_path.c), and that is the sole producer of GUI_CMD_LINE.  One segment has
               no joints, which is the only thing that kept the ribbon (see tess_fx_segment). */
            case GUI_CMD_LINE:
                tess_fx_segment( c->line.x0, c->line.y0, c->line.x1, c->line.y1,
                                 c->line.thickness, c->line.abgr );
                break;

            case GUI_CMD_POLYLINE:
            {
                const gui_vec2_t* pts = &s_draw.points[ c->polyline.pt_offset ];
                f32 center_off = stroke_center_offset( c->polyline.align, c->polyline.thickness * 0.5f );
                tess_stroke_poly_aa( pts, c->polyline.pt_count, c->polyline.thickness,
                                     center_off, c->polyline.closed, c->polyline.abgr );
                break;
            }

            case GUI_CMD_DASHED_LINE:
                tess_dashed_line( c->dash.x0, c->dash.y0, c->dash.x1, c->dash.y1,
                                  c->dash.thickness, c->dash.period, c->dash.duty, c->dash.abgr );
                break;

            case GUI_CMD_RECT_GRADIENT:
                tess_rect_gradient( c->gradient.x, c->gradient.y, c->gradient.w, c->gradient.h,
                                    c->gradient.col_a, c->gradient.col_b, c->gradient.horizontal );
                break;

            case GUI_CMD_RECT_LIST:
            {
                /* One quad per pooled entry; all share this command's clip/vp so they collapse
                   into the same GPU batch.  Solid color (tex 0 = white texel), never rounded. */
                const gui_rect_col_t* rl = &s_draw.rect_pool[ c->rect_list.offset ];
                for ( u32 k = 0; k < c->rect_list.count; ++k )
                    tess_rect_filled( rl[ k ].x, rl[ k ].y, rl[ k ].w, rl[ k ].h,
                                      0, 0, 1, 1, 0, rl[ k ].abgr );
                break;
            }

            case GUI_CMD_SPRITE:
                /* 1, 3 or 9 quads from this one command -- the whole expansion, plus the sprite
                   lookup it needs, lives in tess_sprite. */
                tess_sprite( c );
                break;
        }
    }

    if ( open_vid != GUI_ID_NONE )
        volatile_range_close( open_vid, vb_open, ib_open, cmd_open );

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/
