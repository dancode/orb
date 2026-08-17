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
    gui_prim_t      prims    [ GUI_MAX_PRIMS ];    // primitive records (gui.h) -- the third arena
    tess_gpu_cmd_t  gpu_cmds [ GUI_MAX_CMDS  ];    // gpu draw commands (AOS: cmd + vp/vbase/ibase)

    u32 vert_count, idx_count, prim_count, cmd_count;   // write head cursors

    gui_rect_t  cur_clip;   /* clip resolved from s_draw.clip_table[c->clip_idx] for each command */
    u32         cur_clip_local; /* the same clip as an ABSOLUTE frame-region entry index (the slot's
                                   slab base + its local first-seen index) -- the clip-band bits
                                   tess_verts_commit folds into every vertex's tex word           */
    i32         cur_vp;     /* viewport baked from the current semantic command                    */
    u32         cur_tex;    /* GUI_TEX_MODE | bindless slot stamped into every vertex committed --
                               set by tess_set_tex, applied by tess_verts_commit.  NOT a batch key */
    u32         cur_ops;    /* GUI_OP_* -- the per-primitive modifiers (gui.h).  Cleared per
                               semantic command alongside the record: leaking a self flag would
                               blank a textured quad, and leaking an op would reshape the next
                               fill.  tess_verts_commit maps these onto the tex word's op band
                               while that word still exists, and copies them into the record. */

    /* The ambient PRIMITIVE RECORD -- filled by
       whichever tess_* emitter is running and appended (deduplicated) by tess_verts_commit.  Only
       the fields the ambient FIELD actually reads are written; the rest stay zero, which is what
       lets consecutive flat fills collapse onto one record (tess_prim_local).  Cleared per
       semantic command.

       cur_prim_local is the slot-local index of the record the last commit resolved to, and
       prim_memo_valid says whether the record at prim_count-1 is that record and therefore
       reusable -- the memo that keeps a glyph run to one entry. */
    gui_prim_t  cur_prim;
    u32         cur_prim_local;
    bool        prim_memo_valid;

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

    /* Record base of the window slot being tessellated -- the counterpart of slot_vert_base for
       the third arena.  Records bake SLOT-LOCAL indices (prim_count - slot_prim_base) and the
       flush adds this base back through pc.prim_base, so a repack that moves the slot's records
       leaves every cached vertex's index correct. */
    u32 slot_prim_base;

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
    u32  vert_hwm, idx_hwm, prim_hwm;   /* lifetime peak of the TOTAL write head (both bands) */
    bool overflow_ever;                 /* sticky: any frame this run overflowed a buffer     */

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

/* A pattern cell floored at one pixel.  The quarter-pixel quantize and the upper bound that used
   to live here were the packed cell field's; the record carries an exact float.  The floor stays,
   and it is not about storage: a sub-pixel lattice is aliasing, not a pattern. */
static f32
tess_clamp_cell( f32 cell )
{
    return ( cell < 1.0f ) ? 1.0f : cell;
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
    s_tess.prim_count      = 0;
    s_tess.cmd_count       = 0;
    s_tess.slot_vert_base  = 0;
    s_tess.slot_idx_base   = 0;
    s_tess.slot_cmd_base   = 0;
    s_tess.slot_prim_base  = 0;
    s_tess.slot_tess_gen   = 0;
    s_tess.cur_prim_local  = 0;
    s_tess.prim_memo_valid = false;
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
   A slot past GUI_WIN_CLIP_MAX distinct clips falls back to its slab's entry 0 -- degrading INSIDE
   the window (its own first clip, usually the window rect) rather than borrowing a neighbour's
   slab.  Asserted, because 16 distinct clips in one window is a bug, not a budget. */
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

/* Resolve the ambient primitive record to a SLOT-LOCAL index in the frame's record arena,
   appending a new entry when the ambient state has moved.  The counterpart of tess_clip_local, and
   deliberately the same shape -- but keyed on CONTENT rather than on a table index, because the
   record is assembled from half a dozen ambient fields and nothing upstream has a name for the
   combination.

   The memo is the whole performance story.  A glyph run is one semantic command emitting hundreds
   of quads under one unchanging record, and a run of flat fills sharing a texture and a clip is
   the same: comparing against the last appended record collapses each to ONE entry.  That only
   works because emitters leave the fields their field does not read at zero (gui.h) -- a writer
   that stamped its rect into a GUI_FX_NONE record would give every fill an entry of its own.

   Past the arena a slot degrades to its own first record, mirroring tess_clip_local's fallback to
   slab entry 0: a wrong shape is bad, but a wild index into a storage buffer is worse.  The sticky
   overflow flag is what actually reports it, the same way a full vertex buffer does. */
static u32
tess_prim_local( void )
{
    if ( s_tess.prim_memo_valid && s_tess.prim_count > s_tess.slot_prim_base
      && memcmp( &s_tess.prims[ s_tess.prim_count - 1 ], &s_tess.cur_prim,
                 sizeof( gui_prim_t ) ) == 0 )
        return s_tess.cur_prim_local;

    if ( s_tess.prim_count >= GUI_MAX_PRIMS )
    {
        s_tess.overflow        = true;
        s_tess.prim_memo_valid = false;
        return s_tess.cur_prim_local = 0u;
    }

    s_tess.prims[ s_tess.prim_count ] = s_tess.cur_prim;
    s_tess.cur_prim_local             = s_tess.prim_count - s_tess.slot_prim_base;
    s_tess.prim_count++;
    s_tess.prim_memo_valid = true;
    return s_tess.cur_prim_local;
}

/* Take `n` vertices written at s_tess.verts[vert_count] into the buffer, stamping each with the
   active texture and effect word.

   EVERY vertex writer ends here -- this is the only place vert_count advances, which is what makes
   the record impossible to forget.  It is applied from ambient state rather than passed in because
   it is constant over a primitive while only the position/uv/colour vary: cur_tex is set by
   tess_set_tex (which every writer calls alongside tess_ensure_gpu_cmd, to have a command to append
   to), and the rest is cleared per semantic command and filled by the few emitters that want a
   shape.  A new primitive type gets it correct by construction; the alternative -- naming the index
   in each compound literal -- fails silently, since a missing trailing initializer is a legal zero
   and zero is a valid record. */
static void
tess_verts_commit( u32 n )
{
    /* The record's three AMBIENT members are folded in here rather than at the emitters: an
       emitter that has to remember them is an emitter that can forget.  What an emitter does set
       is its FIELD and the geometry that field reads. */
    s_tess.cur_prim.tex  = s_tess.cur_tex;
    s_tess.cur_prim.ops  = s_tess.cur_ops;
    s_tess.cur_prim.clip = s_tess.cur_clip_local;

    u32 prim = tess_prim_local();

    gui_draw_vert_t* v = &s_tess.verts[ s_tess.vert_count ];
    for ( u32 i = 0; i < n; ++i )
        v[ i ].prim = prim;
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

/* Close the reservation tess_prim_begin_* opened: stamp and account the vertices, then fold the
   indices into the open command's element count.  The index half used to be callable on its own,
   because the rounded box committed its vertices in four chunks -- one per quadrant, each under a
   different packed word.  All four corners are in one record now, so every primitive commits once
   and the split has nothing left to serve. */
static void
tess_prim_commit( u32 nv, u32 ni )
{
    tess_verts_commit( nv );
    s_tess.idx_count += ni;
    s_tess.gpu_cmds[ s_tess.cmd_count - 1 ].cmd.elem_count += ni;
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
    The SDF surface -- every rounded shape, in ONE quad.

    A rounded box used to be tessellated: a cached quarter arc fanned into ~37 vertices with hard
    stair-stepped edges, and a texture could not ride on it at all.  Here the CPU emits a covering
    and the fragment shader resolves the boundary exactly (gui.h, the effect band).

    The covering is one quad.  It was four -- one per quadrant -- while the fragment received
    `|p| - c` interpolated from the vertices: an absolute value folds at the centre lines and no
    linear interpolator reproduces a fold, so the shape had to be cut into pieces on which the sign
    of p was fixed.  The fragment derives that coordinate from its own pixel position now, so the
    partition has nothing left to buy.

    The geometry is grown by `pad` past the box so the falloff has somewhere to land: a feathered
    edge (and a shadow's whole soft skirt) is OUTSIDE the shape's own rect.  The BOUNDARY still
    sits exactly on the authored rect -- pad moves the triangles, never the shape.

    A surface with nothing to paint inside carves its interior out, covered by a FRAME of four
    quads around the hole instead of one quad spanning it -- where the 16/24 upper bound below
    comes from.  Three ops qualify.  BAND paints a border only `border` px wide, so rasterizing the
    whole inside of a window frame at zero coverage is a real cost on a large panel.  CUT paints
    nothing at all inside its boundary: the elevation shadow under floating chrome is a band of
    quads around the frame rather than a plate spanning the window, roughly a tenth of the area.
    INSET paints `feather` px in from the boundary, so an inner shadow on a full panel costs the
    rim.

    Every SDF surface samples the same atlas as everything else and names its shape through a
    RECORD, so it merges into whatever GPU command is already open: a soft shadow behind a panel
    costs a batch split of zero.
==============================================================================================*/

/* Emit one SDF surface.  `r4` is the corner radius PER QUADRANT, in the tessellation order
   top-left, top-right, bottom-right, bottom-left (tess_fx_box passes four copies of one radius;
   only tess_round_rect_ex passes four different ones), and `feather` the total width of the falloff
   band straddling the boundary (0 = hard edge); both are always read.  The remaining parameters are
   OP-SPECIFIC, mirroring the fx word's own re-partitioning -- `border` is the border width under
   GUI_OP_BAND, `rate`/`depth` the wave under GUI_OP_PULSE, and each ignores the other's.
   UVs span the AUTHORED box and are clamped over the grown skirt, so a textured rounded quad cannot
   bleed into its atlas neighbour where the coverage has already faded to nothing.

   The surface is always GUI_FX_BOX; which of the four ops it carries comes in on ambient state
   (s_tess.cur_ops), set by the caller BEFORE this runs because the interior hole is sized from
   it.  GUI_OP_CUT and GUI_OP_INSET take no parameter of their own -- they read radius and
   feather exactly as a plain fill does and differ only in the hole below and one line in the
   fragment.

   Per-corner radii are FREE: all four ride the record and the fragment picks the one its own
   quadrant wants, so the geometry does not change at all.

   Why neighbouring quadrants cannot seam, which is the part that has to be true for any of this to
   work.  Each quadrant measures from its OWN radius, so the obvious worry is that the two sides of
   a shared centre line disagree.  They cannot.  Take the horizontal one (local.y = 0): q.y is
   r - hy, and r is clamped to lim <= hy, so q.y <= 0 for every radius.  The y term therefore drops
   out of both branches of the field and what remains is

       d = max( |local.x| - hx, -hy )

   in which r has cancelled.  The same holds on the vertical centre line.  The selection lines are
   precisely where the corner radius stops contributing, so the two sides agree EXACTLY -- not
   approximately, and not merely because the interior saturates.  That was the load-bearing claim
   when the quadrants were separate QUADS and it is the same claim now that they are separate
   BRANCHES of one fragment, which is why the shape survived the fold moving. */
/* `rot` turns the whole surface about the box CENTRE (radians, screen space; 0 = the common
   axis-aligned path).  Only the corner POSITIONS rotate; the fragment un-rotates by the same pair
   out of the record to recover its box-local coordinate.  The UVs are computed from the UNROTATED
   position first, so a textured rotated box still maps its picture across the authored rect and
   clamps over the skirt exactly as the upright one does. */
/*----------------------------------------------------------------------------------------------
    tess_fx_aux_t -- the two extras a box surface can carry, absent from every plain fill.

    One pointer rather than four more parameters, because that is what they are: a rarely-taken
    branch off a call that already states sixteen things.  Both are read only when the op that owns
    them is set, the same rule `border` and `rate`/`depth` follow.
----------------------------------------------------------------------------------------------*/
typedef struct
{
    u32 grad_col;         // GUI_OP_GRAD: the ramp's far colour
    f32 grad_ang;         // GUI_OP_GRAD: axis, radians, box-local, 0 points +x (linear ramp only)
    f32 cut_dx, cut_dy;   // GUI_OP_CUT: the cut boundary's centre, offset from this shape's

} tess_fx_aux_t;

/* `aux` NULL is a plain fill with neither extra -- almost every caller. */
static void
tess_fx_box_core( f32 x, f32 y, f32 w, f32 h, const f32* r4,
                  f32 feather, f32 border, f32 rate, f32 depth, f32 rot,
                  f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
                  const tess_fx_aux_t* aux )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    /* Clamp what is GEOMETRICALLY meaningless, and only that.  The long list that used to live
       here was the packed word's doing: every field had a fixed-point ceiling, and the geometry
       below is built from the same numbers, so a value the fragment could not see would leave the
       vertices describing a different shape than the one it resolved.  The record has no
       ceilings, so what remains is the one bound that is about the SHAPE rather than the storage
       -- a corner radius past half the short side is a capsule -- plus the negatives, which are
       nonsense in every field. */
    f32 hx = w * 0.5f, hy = h * 0.5f;
    f32 lim = ( hx < hy ) ? hx : hy;

    f32 rq[ 4 ];
    f32 rmin, rmax;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 r = r4[ i ];
        if ( r > lim ) r = lim;                   /* a radius past half the short side is a capsule */
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
    if ( border  < 0.0f ) border  = 0.0f;
    if ( rate    < 0.0f ) rate  = 0.0f;
    if ( depth   < 0.0f ) depth = 0.0f;
    if ( depth   > 1.0f ) depth = 1.0f;   /* a fraction, not a pixel count -- a real upper bound */

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

    /* The record: the AUTHORED shape, after the clamps and the grid snap above but before the
       geometry grows by `pad`.  The skirt is a rasterization detail -- the field the fragment
       resolves is measured from the real boundary, so the record must state that one.  All four
       radii travel, which is why the four quadrants below share a single record even when their
       packed words differ. */
    s_tess.cur_prim.field   = (u32)GUI_FX_BOX;
    s_tess.cur_prim.cx      = cx;
    s_tess.cur_prim.cy      = cy;
    s_tess.cur_prim.hw      = hx;
    s_tess.cur_prim.hh      = hy;
    s_tess.cur_prim.r_tl    = rq[ 0 ];
    s_tess.cur_prim.r_tr    = rq[ 1 ];
    s_tess.cur_prim.r_br    = rq[ 2 ];
    s_tess.cur_prim.r_bl    = rq[ 3 ];
    s_tess.cur_prim.feather = feather;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & GUI_OP_BAND ) ? border : 0.0f;
    s_tess.cur_prim.rot_cos = rcs;
    s_tess.cur_prim.rot_sin = rsn;
    s_tess.cur_prim.param_a = ( s_tess.cur_ops & GUI_OP_PULSE ) ? rate  : 0.0f;
    s_tess.cur_prim.param_b = ( s_tess.cur_ops & GUI_OP_PULSE ) ? depth : 0.0f;

    /* GUI_OP_GRAD -- the ramp's far colour and its axis.  The axis is stored ALREADY DIVIDED by
       the box's extent along it (the support width of a projected rectangle), so the ramp spans
       the shape at any angle and the fragment recovers t with one dot product instead of
       repeating this per pixel.  A conic ramp has no extent to divide by -- it measures an ANGLE
       from the axis -- so it stores the unit direction it peaks toward. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_GRAD ) )
    {
        s_tess.cur_prim.col_b = aux->grad_col;

        /* A radial ramp has no axis, so it stays ZERO rather than carrying an angle the fragment
           will not read -- otherwise two identical radial fills authored at different angles take
           two records for no reason (tess_prim_local memos on the record's bytes). */
        if ( !( s_tess.cur_ops & GUI_OP_GRAD_RADIAL ) )
        {
            f32 cs = cosf( aux->grad_ang ), sn = sinf( aux->grad_ang );
            if ( !( s_tess.cur_ops & GUI_OP_GRAD_CONIC ) )
            {
                f32 len = fabsf( w * cs ) + fabsf( h * sn );
                f32 inv = ( len > 1e-6f ) ? 1.0f / len : 0.0f;
                cs *= inv;
                sn *= inv;
            }
            s_tess.cur_prim.grad_x = cs;
            s_tess.cur_prim.grad_y = sn;
        }
    }

    /* GUI_OP_CUT -- where the cut boundary sits.  Zero is the shape cutting itself, which is every
       caller that wants a shadow cast straight down onto the ground under its subject; a non-zero
       offset is the DIRECTIONAL cast, the falloff measured from this outline while the hole is
       taken against the caster's. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_CUT ) )
    {
        s_tess.cur_prim.cut_dx = aux->cut_dx;
        s_tess.cur_prim.cut_dy = aux->cut_dy;
    }

    /* `reach`: how deep past the boundary this surface paints nothing, so the geometry need not
       reach there either.  A BAND stops at the far side of its border; a CUT stops at the boundary
       itself, since its coverage is zero inside (a pixel of slack keeps the hole clear of the
       boundary the rasterizer resolves); an INSET paints inward exactly `feather` px.  All three
       are properties of the surface itself -- none makes any claim about what is drawn OVER it, so
       none can be wrong about it.

       The ops are read from ambient state rather than taken as parameters because they are already
       there: they ride the tex word, which this function does not otherwise touch, and threading
       them back in as arguments to restate what the vertices will carry anyway would be the only
       alternative.

       Two ops on one surface take the WIDEST reach, which is the conservative direction and not the
       exact one: reach only sizes the interior hole, and a larger reach carves a SMALLER hole (the
       eroded radius shrinks with it).  Erring large costs interior fragments nobody sees; erring
       small would notch the frame, which is visible.  Each op alone still lands on exactly the
       number it had as a mode. */
    f32 reach = 0.0f;
    if ( s_tess.cur_ops & GUI_OP_BAND )  reach = border + feather * 0.5f;
    if ( s_tess.cur_ops & GUI_OP_INSET ) reach = ( feather > reach ) ? feather : reach;
    if ( s_tess.cur_ops & GUI_OP_CUT )   reach = ( reach > 1.0f )    ? reach : 1.0f;

    /* The COVERING: which rectangles have to be rasterized for the fragment to resolve the shape.
       One quad normally -- the whole grown extent -- and a frame of four when there is an interior
       worth skipping.  This is pure geometry now.  It used to be four QUADRANT quads whether or not
       there was a hole, because the effect coordinate was |p| - c and the absolute value is not
       affine across the centre lines; the fragment folds for itself, so the quadrant split has
       nothing left to buy and a plain fill costs 4 vertices instead of 16.

       Sizing the hole is the whole subtlety.  It must lie entirely at d <= -reach -- one pixel of
       it inside the painted band would notch the frame -- and the binding case is its CORNER, which
       pokes diagonally toward the arc.  So the hole is the largest axis-aligned box inscribed in
       the shape ERODED by that reach, whose corner sits on the eroded arc at 45 degrees.  Erode
       until nothing is left and there is simply no hole.

       The erosion is derived from the LARGEST radius, which is the conservative direction: a bigger
       radius rounds harder and inscribes a smaller box, so one number is safe for four different
       corners.  It used to be gated on all four matching for want of that argument. */
    f32 qlo_x[ 4 ], qlo_y[ 4 ], qhi_x[ 4 ], qhi_y[ 4 ];
    u32 nq = 1;
    qlo_x[ 0 ] = -ehx;  qlo_y[ 0 ] = -ehy;  qhi_x[ 0 ] = ehx;  qhi_y[ 0 ] = ehy;

    if ( reach > 0.0f )
    {
        f32 er = rmax - reach;                    /* the eroded shape's radius */
        if ( er < 0.0f ) er = 0.0f;
        f32 hix = ( hx - reach - er ) + er * 0.70710678f;
        f32 hiy = ( hy - reach - er ) + er * 0.70710678f;

        /* The hole sits around the shape's own centre -- unless the cut boundary has moved off it,
           in which case what paints nothing is the interior of BOTH, and the intersection of two
           axis-aligned boxes is one axis-aligned box.  Intersecting is conservative where only the
           cut is in play (the hole could be the whole offset box), and a few pixels of extra
           interior costs less than a second rule. */
        f32 hlo_x = -hix, hhi_x = hix;
        f32 hlo_y = -hiy, hhi_y = hiy;
        f32 cdx   = s_tess.cur_prim.cut_dx, cdy = s_tess.cur_prim.cut_dy;
        if ( cdx != 0.0f || cdy != 0.0f )
        {
            if ( cdx - hix > hlo_x ) hlo_x = cdx - hix;
            if ( cdx + hix < hhi_x ) hhi_x = cdx + hix;
            if ( cdy - hiy > hlo_y ) hlo_y = cdy - hiy;
            if ( cdy + hiy < hhi_y ) hhi_y = cdy + hiy;
        }

        if ( hhi_x > hlo_x && hhi_y > hlo_y )
        {
            qlo_x[ 0 ] = -ehx;   qlo_y[ 0 ] = -ehy;    qhi_x[ 0 ] = ehx;     qhi_y[ 0 ] = hlo_y; /* top   */
            qlo_x[ 1 ] = -ehx;   qlo_y[ 1 ] =  hhi_y;  qhi_x[ 1 ] = ehx;     qhi_y[ 1 ] = ehy;   /* base  */
            qlo_x[ 2 ] = -ehx;   qlo_y[ 2 ] =  hlo_y;  qhi_x[ 2 ] = hlo_x;   qhi_y[ 2 ] = hhi_y; /* left  */
            qlo_x[ 3 ] =  hhi_x; qlo_y[ 3 ] =  hlo_y;  qhi_x[ 3 ] = ehx;     qhi_y[ 3 ] = hhi_y; /* right */
            nq = 4;
        }
    }

    u32              nv = nq * 4u, ni = nq * 6u;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin_tex( nv, ni, tex_idx, &v, &idx, &base ) )
        return;

    f32 ulo = ( u0 < u1 ) ? u0 : u1, uhi = ( u0 < u1 ) ? u1 : u0;
    f32 vlo = ( v0 < v1 ) ? v0 : v1, vhi = ( v0 < v1 ) ? v1 : v0;

    /* Corner of one quad in its own lo/hi box, counter-clockwise from the low corner. */
    static const f32 qu[ 4 ] = { 0.0f, 1.0f, 1.0f, 0.0f };
    static const f32 qv[ 4 ] = { 0.0f, 0.0f, 1.0f, 1.0f };

    for ( u32 n = 0; n < nq; ++n )
    {
        for ( u32 c = 0; c < 4; ++c )
        {
            f32 lx = qlo_x[ n ] + qu[ c ] * ( qhi_x[ n ] - qlo_x[ n ] );
            f32 ly = qlo_y[ n ] + qv[ c ] * ( qhi_y[ n ] - qlo_y[ n ] );
            f32 px = cx + lx, py = cy + ly;
            f32 tu = uhi > ulo ? u0 + ( u1 - u0 ) * ( px - x ) / w : u0;
            f32 tv = vhi > vlo ? v0 + ( v1 - v0 ) * ( py - y ) / h : v0;
            if ( tu < ulo ) tu = ulo;  if ( tu > uhi ) tu = uhi;
            if ( tv < vlo ) tv = vlo;  if ( tv > vhi ) tv = vhi;
            /* The turn, LAST: uv came from the unrotated position, so only the world position
               rotates about the centre.  The fragment un-rotates by the same pair to recover the
               box-local coordinate it needs. */
            if ( rot != 0.0f )
            {
                px = cx + lx * rcs - ly * rsn;
                py = cy + lx * rsn + ly * rcs;
            }
            v[ n * 4 + c ] = gui_vert( px, py, tu, tv, abgr );
        }
        tess_quad_idx( &idx[ n * 6 ], (u16)( base + n * 4 ) );
    }

    tess_prim_commit( nv, ni );
}

/* The uniform-radius entry every rounded shape in the library goes through.  Four copies of one
   radius is not a workaround -- it is the honest statement that a rounded rect is the special case
   of a per-corner one, and it keeps a single tessellator for both. */
static void
tess_fx_box( f32 x, f32 y, f32 w, f32 h, f32 r, f32 feather, f32 border, f32 rate, f32 depth,
             f32 rot, f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr,
             const tess_fx_aux_t* aux )
{
    const f32 r4[ 4 ] = { r, r, r, r };
    tess_fx_box_core( x, y, w, h, r4, feather, border, rate, depth, rot,
                      u0, v0, u1, v1, tex_idx, abgr, aux );
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
                 r, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                 0, 0, 1, 1, 0, abgr, NULL );
}

/*==============================================================================================
    tess_round_rect_ex -- a fill whose four corners have four different radii.

    The tab, the notch, the asymmetric card: shapes that used to walk a per-corner perimeter (up to
    72 sampled points) and fan it into as many separate TRIANGLE commands, with a polygonal boundary
    and no antialiasing at all.  Here it is the same FOUR vertices a uniform rounded rect costs, and
    the boundary is exact, because all four radii ride the record and the fragment picks the one
    its own quadrant wants -- the radii are data, not geometry.

        perimeter fan, 4 rounded corners   ~70 verts / ~200 idx, 62 commands, aliased
        the field                            4 verts /    6 idx,  1 command,  antialiased

    The RAMP rides the same record.  A linear one could be carried by the four vertices instead --
    colour is affine along the axis and so is interpolation -- but only a linear one, and only
    approximately: the corners it would be evaluated at are the FALLOFF SKIRT's, a pixel or more
    outside the shape, so the ramp arrives stretched by however wide the skirt is.  Resolved in the
    fragment it spans the shape exactly, and the two ramps a rectangle's corners cannot describe at
    all -- radial and conic -- cost the same one branch.
==============================================================================================*/

static void
tess_round_rect_ex( f32 x, f32 y, f32 w, f32 h,
                    f32 rtl, f32 rtr, f32 rbr, f32 rbl, f32 feather,
                    u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind )
{
    /* Corner order: top-left, top-right, bottom-right, bottom-left -- the order gui_cmd_t
       .round_rect declares its radii in, and the order the record's r_tl/r_tr/r_br/r_bl carry
       them, which is what the fragment indexes by the sign of its own position.  feather below
       the standard AA band clamps up -- 0 means "crisp", never "hard-edged". */
    const f32 r4[ 4 ] = { rtl, rtr, rbr, rbl };

    /* Equal endpoints ARE a flat fill, so the op is left off rather than special-cased: a ramp
       between one colour and itself is that colour, and the fragment should not pay for it. */
    tess_fx_aux_t aux = { 0 };
    if ( col_b != abgr )
    {
        s_tess.cur_ops |= GUI_OP_GRAD;
        if ( grad_kind == (u32)GUI_GRAD_RADIAL ) s_tess.cur_ops |= GUI_OP_GRAD_RADIAL;
        if ( grad_kind == (u32)GUI_GRAD_CONIC  ) s_tess.cur_ops |= GUI_OP_GRAD_CONIC;
        aux.grad_col = col_b;
        aux.grad_ang = grad_ang;
    }

    tess_fx_box_core( x, y, w, h, r4, ( feather > TESS_FX_AA ) ? feather : TESS_FX_AA,
                      0.0f, 0.0f, 0.0f, 0.0f,
                      0, 0, 1, 1, 0, abgr, &aux );
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

#define TESS_PI       3.14159265358979f
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
       ARC is a closed ring, which is a BOX under GUI_OP_BAND whose interior the band carves away
       -- worth real fragments on a large one.  It is reachable: draw_progress_arc at 100% is
       exactly a full sweep.  The reroute used to be gated on the band fitting the packed `border`
       field, so a thick ring fell through to the sector formula (exact at aperture pi, it merely
       rasterizes the hole); the record has no such ceiling, so every full-turn ring takes the
       cheaper path now.
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
        /* The same shape draw_circle's unfilled path asks for, measured from the OUTER boundary
           inward -- so the band still straddles r. */
        f32 outer = r + thickness * 0.5f;
        s_tess.cur_ops |= GUI_OP_BAND;
        tess_fx_box( pcx - outer, pcy - outer, outer * 2.0f, outer * 2.0f,
                     outer, TESS_FX_AA, thickness, 0.0f, 0.0f, 0.0f,
                     0, 0, 1, 1, 0, abgr, NULL );
        return;
    }

    f32 ra = r;
    f32 rb = pie ? 0.0f : thickness * 0.5f;
    if ( rb < 0.0f ) rb = 0.0f;

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

    /* The record.  (cm, sm) is the sector's own frame -- the bisector direction the local
       coordinate above is expressed in -- so it goes where every other field's turn goes. */
    s_tess.cur_prim.field   = (u32)mode;
    s_tess.cur_prim.cx      = pcx;
    s_tess.cur_prim.cy      = pcy;
    s_tess.cur_prim.rot_cos = cm;
    s_tess.cur_prim.rot_sin = sm;
    s_tess.cur_prim.param_a = ra;
    s_tess.cur_prim.param_b = rb;
    s_tess.cur_prim.param_c = ap;

    /* These two carry their parameter pair in the uv lanes instead of texcoords, which is what the
       self-sampled bit announces: the fragment forces coverage to 1 and never reads the texel, so
       the white texel is not needed and the atlas stays bound only to keep the index valid. */
    if ( mode == GUI_FX_ARC_DASH || mode == GUI_FX_ARC_GRAD )
    {
        wu = uvx;
        wv = uvy;
        s_tess.cur_ops |= GUI_OP_SELF;

        /* Unpacked, the pair stops being a uv payload: a dash is a period and a duty, a gradient
           is a second colour.  Both come in already encoded the way the uv lanes wanted them --
           the emit side owns that encoding until the packed path goes (gui_emit_draw.c). */
        if ( mode == GUI_FX_ARC_DASH )
        {
            s_tess.cur_prim.r_tl = uvx;                       /* period, as a fraction of a turn */
            s_tess.cur_prim.r_tr = uvy;                       /* on-duty fraction                */
        }
        else
        {
            u32 bu = (u32)( uvx * 65535.0f + 0.5f );
            u32 bv = (u32)( uvy * 65535.0f + 0.5f );
            s_tess.cur_prim.col_b = ( bu & 0xFFu ) | ( ( bu >> 8 ) << 8 )
                                  | ( ( bv & 0xFFu ) << 16 ) | ( ( bv >> 8 ) << 24 );
        }
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
        v[ i ] = gui_vert( px, py, wu, wv, abgr );
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

    cell = tess_clamp_cell( cell );

    f32 period = 2.0f * cell;
    f32 phx    = ( x - period * floorf( x / period ) ) / period;
    f32 phy    = ( y - period * floorf( y / period ) ) / period;

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;

    s_tess.cur_prim.field   = (u32)GUI_FX_CHECKER;
    s_tess.cur_prim.param_a = cell;
    s_tess.cur_prim.param_b = phx;
    s_tess.cur_prim.param_c = phy;
    s_tess.cur_prim.col_b   = col_b;

    /* col_b in the ARC_GRAD uv lanes; the fragment never samples there (self-sampled, gui.h). */
    wu = (f32)(   col_b         & 0xFFFFu ) / 65535.0f;
    wv = (f32)( ( col_b >> 16 ) & 0xFFFFu ) / 65535.0f;
    s_tess.cur_ops |= GUI_OP_SELF;

    v[ 0 ] = gui_vert( x,     y,     wu, wv, col_a );
    v[ 1 ] = gui_vert( x + w, y,     wu, wv, col_a );
    v[ 2 ] = gui_vert( x + w, y + h, wu, wv, col_a );
    v[ 3 ] = gui_vert( x,     y + h, wu, wv, col_a );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

static void
tess_grid( f32 x, f32 y, f32 w, f32 h, f32 ox, f32 oy, f32 cell, f32 thickness,
           f32 angle, bool stripes, u32 abgr )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    cell = tess_clamp_cell( cell );

    /* WRAP rather than clamp: a lattice at `angle` and at angle + pi are the same lattice, so an
       animated rotation must roll over rather than stick at pi, which is what a clamp would do.
       The wrapped value is used for BOTH the packed word and the phase below -- the fragment
       rotates the pixel coordinate by exactly this angle, so a disagreement would slide the
       pattern off its anchor. */
    angle -= TESS_PI * floorf( angle / TESS_PI );

    /* The lattice anchor, mod the quantized pitch.  (ox, oy) is a screen-space content origin
       and may be anywhere (a panned canvas sends large negatives); only its residue matters.
       The anchor is rotated INTO lattice space first, because that is the space the fragment
       does its mod in -- rotation is linear, so R(px - o) is R(px) - R(o), and the phase is the
       residue of R(o).  Taking the residue before the rotation would anchor the wrong point. */
    f32 acs = cosf( angle ), asn = sinf( angle );
    f32 rx  =  ox * acs + oy * asn;
    f32 ry  = -ox * asn + oy * acs;

    f32 phx = ( rx - cell * floorf( rx / cell ) ) / cell;
    f32 phy = ( ry - cell * floorf( ry / cell ) ) / cell;

    f32              wu, wv;
    gui_draw_vert_t* v;
    u16*             idx;
    u16              base;
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;

    s_tess.cur_prim.field   = (u32)GUI_FX_GRID;
    s_tess.cur_prim.param_a = cell;
    s_tess.cur_prim.param_b = thickness;
    s_tess.cur_prim.param_c = angle;
    s_tess.cur_prim.r_tl    = phx;
    s_tess.cur_prim.r_tr    = phy;
    if ( stripes )
        s_tess.cur_ops |= GUI_OP_STRIPES;

    /* The lattice is one colour -- the vertex colour -- so the uv word is free to carry the
       per-axis phase instead of texcoords (self-sampled, gui.h). */
    wu = phx;
    wv = phy;
    s_tess.cur_ops |= GUI_OP_SELF;

    v[ 0 ] = gui_vert( x,     y,     wu, wv, abgr );
    v[ 1 ] = gui_vert( x + w, y,     wu, wv, abgr );
    v[ 2 ] = gui_vert( x + w, y + h, wu, wv, abgr );
    v[ 3 ] = gui_vert( x,     y + h, wu, wv, abgr );
    tess_quad_idx( idx, base );
    tess_prim_commit( 4u, 6u );
}

/* The ambient TEXT_EDGE, straight onto the record: a band `width` px outside the glyph boundary,
   painted in `abgr`.  A zero width is no edge at all and leaves the field NONE, which is what
   every plain run wants. */
static void
tess_text_edge_prim( f32 width, u32 abgr )
{
    if ( width <= 0.0f )
        return;

    s_tess.cur_prim.field   = (u32)GUI_FX_TEXT_EDGE;
    s_tess.cur_prim.param_a = width;
    s_tess.cur_prim.col_b   = abgr;
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
    s_tess.cur_prim.field   = (u32)GUI_FX_TILE_U;
    s_tess.cur_prim.param_a = umax;

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
    if ( !tess_prim_begin( 4u, 6u, &wu, &wv, &v, &idx, &base ) )
        return;
    /* The record states the capsule outright: midpoint, half-length, the axis as the shape's turn,
       and the radius in the corner-radius lane.  That is what collapsed the geometry to one quad --
       the along-axis fold that used to force two lives in the fragment now. */
    s_tess.cur_prim.field   = (u32)GUI_FX_SEG;
    s_tess.cur_prim.cx      = mx;
    s_tess.cur_prim.cy      = my;
    s_tess.cur_prim.hw      = hl;
    s_tess.cur_prim.r_tl    = r;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_prim.rot_cos = ux;
    s_tess.cur_prim.rot_sin = uy;

    /* One quad, oriented along the segment: `a` spans the full length signed, `b` the full width. */
    static const f32 qa[ 4 ] = { -1.0f, 1.0f, 1.0f, -1.0f };
    static const f32 qb[ 4 ] = { -1.0f, -1.0f, 1.0f, 1.0f };

    for ( u32 c = 0; c < 4; ++c )
    {
        f32 a = qa[ c ] * ea, b = qb[ c ] * ec;
        v[ c ] = gui_vert( mx + a * ux + b * nx, my + a * uy + b * ny, wu, wv, col );
    }
    tess_quad_idx( idx, base );

    tess_prim_commit( 4u, 6u );
}

/* Volatile-widget seam (render/pipeline/gui_build_volatile.c, included right after this file in
   the gui_render.c unity build).  tess_dispatch calls volatile_range_close once a tagged command
   RANGE's vertices/indices/GPU commands are fully written; it records the block's slot-relative
   position, reserves padded headroom past the live geometry (advancing this file's write heads),
   and stamps the slot tessellation generation.  s_volatile_patching is defined HERE (first in the
   TU) and set by volatile_patch around its scratch re-tessellation so the range tracking below
   stays inert during a patch -- a patch must never look like a fresh capture. */
static bool s_volatile_patching;
static void volatile_range_close( gui_id_t id, u32 vb_open, u32 ib_open, u32 pb_open,
                                  u32 cmd_open );

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
    u32      vb_open = 0, ib_open = 0, pb_open = 0, cmd_open = 0;
    (void)win;

    for ( u32 oi = 0; oi < count; ++oi )
    {
        u32              ci = order[ oi ];
        const gui_cmd_t* c  = &cmds[ ci ];

        gui_id_t vid = s_volatile_patching ? GUI_ID_NONE : s_draw.cmd_volatile_id[ ci ];
        if ( vid != open_vid )
        {
            if ( open_vid != GUI_ID_NONE )
                volatile_range_close( open_vid, vb_open, ib_open, pb_open, cmd_open );
            open_vid = vid;
            if ( vid != GUI_ID_NONE )
            {
                s_tess.force_new_cmd = true;   /* block owns its GPU commands from the first primitive */
                vb_open  = s_tess.vert_count;
                ib_open  = s_tess.idx_count;
                pb_open  = s_tess.prim_count;
                cmd_open = s_tess.cmd_count;

                /* Cold memo, so the block's first primitive appends a record INSIDE its own range
                   instead of reusing the window's preceding one.  A reused record would sit outside
                   the reservation the patch is allowed to rewrite, and the patch -- which always
                   starts cold -- would then disagree with the capture about how many it needs. */
                s_tess.prim_memo_valid = false;
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
        s_tess.cur_ops = 0u;

        /* The record is cleared WHOLE, and it matters for two reasons: a leftover rect or radius
           does not merely paint wrong, it defeats the memo -- a run of flat fills carrying stale
           geometry would take one record each. */
        s_tess.cur_prim = ( gui_prim_t ){ 0 };

        switch ( c->type )
        {
            /* A square rect keeps the one-quad fast path: it is pixel-aligned by construction, so
               there is no edge for an SDF to resolve and nothing to gain.  Rounding is what turns
               it into a surface -- and routing the TEXTURED case through as well is what finally
               lets a rounded quad carry an image, which the arc fan never could. */
            case GUI_CMD_RECT_FILLED:
                if ( c->rect.rounding > 0.0f )
                    tess_fx_box( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                 c->rect.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                                 c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                 c->rect.tex_idx, c->rect.abgr, NULL );
                else
                    tess_rect_filled( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                      c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                      c->rect.tex_idx, c->rect.abgr );
                break;

            /* The band measures from the OUTER boundary inward, matching the square path's
               INSIDE band (and the closed AA stroke this replaced). */
            case GUI_CMD_RECT_OUTLINE:
                if ( c->rect_outline.rounding > 0.0f )
                {
                    s_tess.cur_ops |= GUI_OP_BAND;
                    tess_fx_box( c->rect_outline.x, c->rect_outline.y,
                                 c->rect_outline.w, c->rect_outline.h,
                                 c->rect_outline.rounding, TESS_FX_AA, c->rect_outline.t,
                                 0.0f, 0.0f, 0.0f,
                                 0, 0, 1, 1, 0, c->rect_outline.abgr, NULL );
                }
                else
                    tess_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                       c->rect_outline.w, c->rect_outline.h,
                                       c->rect_outline.t, c->rect_outline.abgr );
                break;

            /* The parameterized surface: a shadow is the wide feather (what used to be six
               stacked rects pretending to be a gaussian is the exact same falloff the corners
               use, only spread out), a pulse the shader-clock word -- geometrically a plain
               rounded fill whose vertices are correct for every frame it runs, so the retained
               slot never invalidates and the breathing costs no re-tessellation. */
            case GUI_CMD_FX_BOX:
                /* All four of these are the one GUI_FX_BOX mode; the variant and the rate pick
                   which ops ride the tex word.  They are INDEPENDENT flags rather than a choice of
                   one, which is what lets a cut or inset surface breathe -- as a mode number the
                   pulse had to displace whichever shape it was applied to.  Set before the
                   tessellator runs because the interior hole is sized from them (see `reach`). */
                if ( c->fx_box.variant == 1u )   s_tess.cur_ops |= GUI_OP_CUT;
                if ( c->fx_box.variant == 2u )   s_tess.cur_ops |= GUI_OP_INSET;
                if ( c->fx_box.rate    > 0.0f )  s_tess.cur_ops |= GUI_OP_PULSE;
                {
                    /* The cut boundary, for the DIRECTIONAL cast: the command states where the
                       shadow is drawn, and this says where the caster it belongs to sits relative
                       to it.  Zero for every other variant, and the aux is read only under the op
                       that owns it. */
                    tess_fx_aux_t aux = { 0 };
                    aux.cut_dx = c->fx_box.cut_dx;
                    aux.cut_dy = c->fx_box.cut_dy;
                    tess_fx_box( c->fx_box.x, c->fx_box.y, c->fx_box.w, c->fx_box.h,
                                 c->fx_box.rounding, c->fx_box.feather, 0.0f,
                                 c->fx_box.rate, c->fx_box.depth, c->fx_box.rot,
                                 0, 0, 1, 1, 0, c->fx_box.abgr, &aux );
                }
                break;

            /* Four radii and a ramp -- and still one surface, one command and no batch split,
               exactly like the uniform fill it generalizes. */
            case GUI_CMD_ROUND_RECT_EX:
                tess_round_rect_ex( c->round_rect.x, c->round_rect.y,
                                    c->round_rect.w, c->round_rect.h,
                                    c->round_rect.rtl, c->round_rect.rtr,
                                    c->round_rect.rbr, c->round_rect.rbl,
                                    c->round_rect.feather, c->round_rect.abgr,
                                    c->round_rect.col_b, c->round_rect.grad_ang,
                                    c->round_rect.grad_kind );
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
                           c->grid.angle, c->grid.stripes != 0u, c->grid.abgr );
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
                tess_text_edge_prim( c->text.edge_w, c->text.edge_col );
                tess_text_n( c->text.x, c->text.y, c->text.abgr, s_draw.text_pool + c->text.off,
                             c->text.len, c->text.clip_x0, c->text.clip_x1 );
                break;

            case GUI_CMD_TEXT_XF:
                if ( c->text_xf.font != cur_font )
                    font_use( cur_font = c->text_xf.font );
                tess_text_edge_prim( c->text_xf.edge_w, c->text_xf.edge_col );
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
        volatile_range_close( open_vid, vb_open, ib_open, pb_open, cmd_open );

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/
