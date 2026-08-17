/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess.c -- CPU-side quad-record builder.

    Translates the frame's semantic gui_cmd_t list (s_draw) into quad records in s_tess:
    ONE 48-byte gui_quad_t per shape (gui.h), expanded by SV_VertexID in gui_quad.vs.hlsl --
    there is no vertex buffer and no index buffer.  Everything here reads semantic commands
    and writes quad + style records; nothing here touches the GPU API.

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
   live together in one cache line rather than in parallel arrays keyed on the same index.
   vbase is explicit (not accumulated from elem_counts at flush) so the quad arena may hold
   reserved gaps -- volatile block headroom -- between commands.  Mirrors dash_cmd_t (the snapshot
   type in gui_render.h), which was AOS from the start; this is the live half catching up. */

typedef struct
{
    gui_gpu_cmd_t cmd;      // elem_count (quads), tex_idx, clip_rect -- the GPU draw-call unit
    i32           vp;       // viewport for this command (GUI_VP_INVALID = dormant volatile pad)
    u32           vbase;    // quad slot -- first quad of command (its draw's first_vertex / 6)

} tess_gpu_cmd_t;

/*==============================================================================================
    Tessellation state -- the quad and style arenas populated from the semantic command list.

    cache_build_frame tessellates the frame's gui_cmd_t list into s_tess (per window, via
    tess_dispatch), then gui_render_flush uploads s_tess.quads/prims to the GPU.  s_tess is
    backend-private; nothing above the backend unit touches it.  s_draw holds only semantic
    commands (gui_cmd_t) -- no geometry.

    cur_clip/cur_vp are written by tess_dispatch before each primitive and ARE the batch key, which
    is why tess_ensure_gpu_cmd takes no parameters -- it reads them from here.  cur_clip is
    resolved from s_draw.clip_table[c->clip_idx]; z is per-segment and is not tracked here.
==============================================================================================*/

static struct
{
    gui_quad_t      quads    [ GUI_MAX_QUADS ];    // quad records -- the geometry arena (gui.h)
    gui_prim_t      prims    [ GUI_MAX_PRIMS ];    // style records -- the second arena
    tess_gpu_cmd_t  gpu_cmds [ GUI_MAX_CMDS  ];    // gpu draw commands (AOS: cmd + vp/vbase)

    /* Write head cursors.  vert_count counts QUAD RECORDS -- the geometry element throughout
       the backend (the cache's slot spans, the volatile reservations, the dirty spans and the
       flush all share the vert_* vocabulary for it). */
    u32 vert_count, prim_count, cmd_count;

    gui_rect_t  cur_clip;   /* clip resolved from s_draw.clip_table[c->clip_idx] for each command */
    u32         cur_clip_local; /* the same clip as an ABSOLUTE frame-region entry index (the slot's
                                   slab base + its local first-seen index) -- what every quad's
                                   clip lane carries                                              */
    i32         cur_vp;     /* viewport baked from the current semantic command                    */
    u32         cur_tex;    /* GUI_TEX_MODE | bindless slot the style record carries -- set by
                               tess_set_tex, folded by tess_quad_push.  NOT a batch key           */
    u32         cur_ops;    /* GUI_OP_* -- the per-primitive modifiers (gui.h).  Cleared per
                               semantic command alongside the record: leaking a self flag would
                               blank a textured quad, and leaking an op would reshape the next
                               fill.  tess_quad_push folds these into the style record. */
    f32         cur_corner_pow; /* corner profile exponent for the box family, ambient over one
                               command for the same reason cur_ops is: it reaches all four corners
                               of a shape without threading a parameter through every fill in the
                               library, and cannot leak onto the next command.  0 = circular arcs */

    /* The ambient STYLE RECORD -- filled by whichever tess_* emitter is running and appended
       (deduplicated) by tess_quad_push via tess_prim_local.  Only the fields the ambient FIELD
       actually reads are written; the rest stay zero, which is what lets consecutive flat fills
       collapse onto one record (tess_prim_local).  Cleared per semantic command.

       cur_prim_local is the slot-local index of the record the last commit resolved to.
       prim_dedup_floor is the lowest record index the memo may reach BACK to: every record in
       [max(floor, slot_prim_base), prim_count) was written by this pass and is a live dedup
       candidate.  The scan looks at the last few of those (TESS_PRIM_MEMO_DEPTH), which is what
       collapses the A-B-A interleave ordinary chrome emits -- a flat-fill record, a widget's box
       record, the flat record again -- where a last-record-only memo re-appended A per widget.
       The floor rises wherever the records behind it stop being reusable: a new slot (previous
       slot's indices are relative to its own base), a volatile block's boundary in either
       direction (its records are a patch-rewritable reservation), and a patch's scratch. */
    gui_prim_t  cur_prim;
    u32         cur_prim_local;
    u32         prim_dedup_floor;

    /* QUAD BACKEND ambient glyph addressing, set by the text tessellator around each glyph's
       fill: the glyph table entry the quad names by stable id (~0u = none, bake uvs), and the
       horizontal cut pair for a clip straddler (0 = the whole glyph).  Cleared per command. */
    u32         cur_glyph;
    u32         cur_glyph_cut;

    /* per-slot tesellation context */

    /* Quad base of the window slot currently being tessellated -- the origin volatile blocks
       measure their slot-relative positions against. */
    u32 slot_vert_base;

    /* Command base and tessellation generation of the window slot currently being tessellated --
       set alongside slot_vert_base by cache_build_frame's retess path.  Volatile blocks record
       their position slot-RELATIVE (never absolute) so a later patch can resolve the current
       absolute position from the live slot table, and stamp slot_tess_gen so a patch only ever
       writes into geometry produced by the exact tessellation pass that captured it
       (see render/pipeline/gui_build_volatile.c). */

    u32 slot_cmd_base;
    u32 slot_tess_gen;

    /* Style-record base of the window slot being tessellated -- the counterpart of
       slot_vert_base for the style arena.  Quads bake SLOT-LOCAL style indices
       (prim_count - slot_prim_base) and the flush adds this base back through pc.prim_base, so
       a repack that moves the slot's records leaves every cached quad's index correct. */
    u32 slot_prim_base;

    /* The slot's LOCAL clip table, written while its commands tessellate: first-seen distinct
       clip rects, appended by tess_clip_local and stored on the slot record itself so cache-hit
       frames replay the exact rects the baked vertex indices mean.  slot_clips/slot_clip_count
       point into the win_geo_slot_t being tessellated (set alongside slot_vert_base); NULL
       between slots.  slot_clip_base is the window's fixed slab origin in the frame clip region
       (stable cache slot * GUI_WIN_CLIP_MAX) -- tess_clip_local folds it in so quads bake
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

/* The geometry element copy: the cache's in-place relocation and the volatile patch both move
   quad spans by element index. */
static inline void
tess_geo_copy( u32 dst, u32 src, u32 count )
{
    memcpy( &s_tess.quads[ dst ], &s_tess.quads[ src ], count * sizeof( gui_quad_t ) );
}

/*==============================================================================================
    Tessellation diagnostics -- the cold companion to s_tess.

    High-water marks, the sticky overflow flag, and the arena band boundary the dashboard reads.
    Every field here is written at most once per slot placement / band boundary / frame end --
    never on the per-vertex path -- so it lives apart from s_tess to keep the hot write cacheline
    (vert_count / cmd_count) small.  Read only by the dashboard capture and the render
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
    u32 v_lo, v_hi;   // pending quad range, arena-absolute (empty when v_lo >= v_hi)

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
patch_span_union( u8 vp, u8 band, u32 v_lo, u32 v_hi )
{
    if ( vp >= GUI_MAX_VIEWPORTS )
        return;
    u32 b = band != 0 ? 1u : 0u;
    for ( u32 f = 0; f < RHI_MAX_FRAMES_IN_FLIGHT; ++f )
    {
        u32 r = f * GUI_MAX_VIEWPORTS + vp;
        patch_range_union( &s_patch_pending[ r ][ b ].v_lo, &s_patch_pending[ r ][ b ].v_hi,
                           v_lo, v_hi );
    }
}

static struct
{
    u32  vert_hwm, prim_hwm;   /* lifetime peak of the TOTAL write head (both bands) */
    bool overflow_ever;        /* sticky: any frame this run overflowed a buffer     */

    /* Arena band boundary: the write head right after the last MAIN-band slot placed this frame
       (band-major placement packs every debug-band slot after it).  The dashboard's memory map
       reads this as "main arena ends here"; the span up to vert_count past it is the debug UI's
       own attributed footprint.  Re-derived by cache_build_frame every build. */
    u32 band0_vert_end;

    /* Lifetime peak of the MAIN-band (band 0) write head alone -- the real application's geometry
       ceiling, tracked apart from vert_hwm so the dashboard can show actual use limits with the
       self-measuring debug band filtered out.  Peaks on a different frame than vert_hwm, so it is a
       separate accumulator, not a subtraction. */
    u32 band0_vert_hwm;

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
    s_tess.prim_count      = 0;
    s_tess.cmd_count       = 0;
    s_tess.slot_vert_base  = 0;
    s_tess.slot_cmd_base   = 0;
    s_tess.slot_prim_base  = 0;
    s_tess.slot_tess_gen   = 0;
    s_tess.cur_prim_local  = 0;
    s_tess.prim_dedup_floor = 0;
    s_tess.slot_clips        = NULL;
    s_tess.slot_clip_count   = NULL;
    s_tess.slot_clip_pending = NULL;
    s_tess.slot_clip_base    = 0;
    s_tess.clip_memo_ci      = 0xFF;
    s_tess.cur_clip_local    = 0;
    s_tess.force_new_cmd     = false;
    s_tess.overflow          = false;
    s_tess.cur_glyph         = ~0u;
    s_tess.cur_glyph_cut     = 0;
}

/* Name the texture the next quad's style will CARRY (tess_quad_push folds it into the record).
   Deliberately NOT part of opening a batch, and separated from it so that reads: the texture
   rides the style record, so a texture change costs nothing and must not open a command. */
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
   texture, the style and the clip all travel per quad and cannot cut a draw call, and z is
   per-segment rather than per-command (the segment system already guarantees every command in
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
    /* Quad span of this command starts at the current vert_count; the next command's vbase (or
       the final vert_count for the last) bounds it.  Lets a surface upload only its own quads.
       tex_idx and clip_rect are the ambient values at the moment the command opened, i.e. the
       FIRST primitive's, and are diagnostic only (the dashboard tooltip) -- both ride the quad
       now and the command may go on to span several of each. */
    s_tess.gpu_cmds[ s_tess.cmd_count++ ] = ( tess_gpu_cmd_t ){
        .cmd   = { .elem_count = 0, .tex_idx = s_tess.cur_tex, .clip_rect = s_tess.cur_clip },
        .vp    = s_tess.cur_vp,
        .vbase = s_tess.vert_count,
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
/* How far back the dedup scan reaches.  1 collapses a homogeneous run (a glyph run, consecutive
   flat fills); the extra depth collapses the ALTERNATION chrome actually emits -- text, a rounded
   widget's own record, text again -- which a 1-deep memo re-appended on every return.  Four
   112-byte compares against L1-hot records is noise next to the tessellation around it. */
#define TESS_PRIM_MEMO_DEPTH  4u

static u32
tess_prim_local( void )
{
    u32 hi = s_tess.prim_count;
    u32 lo = ( s_tess.prim_dedup_floor > s_tess.slot_prim_base )
             ? s_tess.prim_dedup_floor : s_tess.slot_prim_base;
    if ( hi > lo )
    {
        u32 n = hi - lo;
        if ( n > TESS_PRIM_MEMO_DEPTH ) n = TESS_PRIM_MEMO_DEPTH;
        for ( u32 k = 1; k <= n; ++k )
            if ( memcmp( &s_tess.prims[ hi - k ], &s_tess.cur_prim,
                         sizeof( gui_prim_t ) ) == 0 )
                return s_tess.cur_prim_local = ( hi - k ) - s_tess.slot_prim_base;
    }

    if ( hi >= GUI_MAX_PRIMS )
    {
        s_tess.overflow         = true;
        s_tess.prim_dedup_floor = hi;
        return s_tess.cur_prim_local = 0u;
    }

    s_tess.prims[ hi ]    = s_tess.cur_prim;
    s_tess.cur_prim_local = hi - s_tess.slot_prim_base;
    s_tess.prim_count++;
    return s_tess.cur_prim_local;
}

/*==============================================================================================
    tess_quad_push -- the ONE geometry writer.  Resolves the ambient style (placement and clip
    live on the quad, never the style), appends the quad, and folds one element into the open
    GPU command.

    `rule_flags` is the expansion rule (GUI_QUAD_RULE_*) plus GUI_QUAD_GLYPH when the ambient
    glyph id below should ride the uv lanes.  Placement is the SHAPE's, by the rule's convention
    (gui.h); uv0/uv1 are packed texcoord corners (ignored under the glyph flag).
==============================================================================================*/

static void
tess_quad_push( f32 qcx, f32 qcy, f32 qhw, f32 qhh, u32 rule_flags,
                u32 uv0, u32 uv1, u32 tex_idx, u32 abgr )
{
    if ( s_tess.vert_count + 1u > GUI_MAX_QUADS )
    {
        s_tess.overflow = true;
        return;
    }
    tess_set_tex( tex_idx );
    if ( !tess_ensure_gpu_cmd() )
        return;

    /* Fold the ambient texture and ops into the style; the clip entry rides the quad below,
       never the style, so a style compares equal across scroll regions. */
    s_tess.cur_prim.tex = s_tess.cur_tex;
    s_tess.cur_prim.ops = s_tess.cur_ops;

    u32 style = tess_prim_local();
    u32 cut   = 0;
    if ( s_tess.cur_glyph != ~0u )
    {
        rule_flags |= GUI_QUAD_GLYPH;
        uv0         = s_tess.cur_glyph;
        cut         = s_tess.cur_glyph_cut;
    }

    s_tess.quads[ s_tess.vert_count++ ] = ( gui_quad_t ){
        .cx    = qcx,
        .cy    = qcy,
        .hw    = qhw,
        .hh    = qhh,
        .uv0   = uv0,
        .uv1   = uv1,
        .abgr  = abgr,
        .style = style,
        .clip  = s_tess.cur_clip_local,
        .flags = rule_flags,
        .cut   = cut,
    };

    /* elem_count counts QUADS under this backend; the flush multiplies by six at the draw. */
    s_tess.gpu_cmds[ s_tess.cmd_count - 1 ].cmd.elem_count += 1;
}

/* Tessellate a filled quad into s_tess.  abgr has alpha pre-baked by the emit side. */
static void
tess_rect_filled( f32 x, f32 y, f32 w, f32 h,
                  f32 u0, f32 v0, f32 u1, f32 v1,
                  u32 tex_idx, u32 abgr )
{
    /* tex_idx 0 = solid-color convention: GUI_OP_SELF says "do not consult the texel", which
       cuts the fill's last tie to atlas PLACEMENT (the texture INDEX it still carries is
       repack-stable), so a plain fill's style never goes stale. */
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        s_tess.cur_ops |= GUI_OP_SELF;
        u0 = v0 = u1 = v1 = 0.0f;
    }
    x = tess_snap_px( x );
    y = tess_snap_px( y );
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr );
}

/* Tessellate a two-color gradient quad: a GRAD style -- a quad record carries ONE colour, and
   the fragment's linear ramp is the same photometric blend the old per-vertex corner
   interpolation produced.  The axis is stored pre-divided by the extent (the tess_fx_box_core
   convention), so the style dedups across same-size ramps.  Origin grid-snapped like
   tess_rect_filled.  rot_cos is written explicitly: the ramp is evaluated in the shape-local
   frame, and a zeroed rot pair would collapse it to a flat 50/50 blend. */
static void
tess_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;
    x = tess_snap_px( x );
    y = tess_snap_px( y );

    s_tess.cur_ops |= GUI_OP_SELF | GUI_OP_GRAD | GUI_OP_DITHER;
    s_tess.cur_prim.col_b   = col_b;
    s_tess.cur_prim.grad_x  = horizontal ? 1.0f / w : 0.0f;
    s_tess.cur_prim.grad_y  = horizontal ? 0.0f : 1.0f / h;
    s_tess.cur_prim.rot_cos = 1.0f;
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), col_a );
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

    The CPU emits a covering quad and the fragment shader resolves the boundary exactly (gui.h,
    the effect band).  The covering is grown past the box by the falloff pad (the SKIRT rule) so
    a feathered edge -- and a shadow's whole soft skirt -- has somewhere to land; the BOUNDARY
    still sits exactly on the authored rect.

    A wide BAND/CUT/INSET surface rasterizes its interior at zero coverage (the fields early-out
    cheaply); the hole-carving the old vertex path did for those has no home in a one-rectangle
    record, and UI fill is nowhere near bound.

    Every SDF surface samples the same atlas as everything else and names its shape through a
    STYLE record, so it merges into whatever GPU command is already open: a soft shadow behind a
    panel costs a batch split of zero.
==============================================================================================*/

/* Emit one SDF surface.  `r4` is the corner radius PER QUADRANT, in the tessellation order
   top-left, top-right, bottom-right, bottom-left (tess_fx_box passes four copies of one radius;
   only tess_round_rect_ex passes four different ones), and `feather` the total width of the falloff
   band straddling the boundary (0 = hard edge); both are always read.  The remaining parameters are
   OP-SPECIFIC, mirroring the record's own re-partitioning -- `border` is the border width under
   GUI_OP_BAND, `rate`/`depth` the wave under GUI_OP_PULSE, and each ignores the other's.
   UVs span the AUTHORED box and are clamped over the grown skirt, so a textured rounded quad cannot
   bleed into its atlas neighbour where the coverage has already faded to nothing.

   The surface is always GUI_FX_BOX; which of the four ops it carries comes in on ambient state
   (s_tess.cur_ops), set by the caller BEFORE this runs.  GUI_OP_CUT and GUI_OP_INSET take no
   parameter of their own -- they read radius and feather exactly as a plain fill does.

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
    u32 frame_col;        // GUI_OP_FRAME: the border band's colour (col_b -- never with GRAD)
    u32 grad_col;         // GUI_OP_GRAD: the ramp's far colour
    f32 grad_ang;         // GUI_OP_GRAD: axis, radians, box-local, 0 points +x (linear ramp only)
    f32 grad_mid;         // GUI_OP_GRAD: midpoint bend, already the exponent (0 = linear)
    f32 cut_dx, cut_dy;   // GUI_OP_CUT: the cut boundary's centre, offset from this shape's
    f32 anim_rate;        // GUI_OP_DASH: pattern scroll px/sec; GUI_OP_PULSE: unused (rate = param_a)
    f32 anim_phase;       // GUI_OP_DASH: static px offset; GUI_OP_PULSE: cycle offset
    f32 dash_period;      // GUI_OP_DASH: px per on+off cycle, already snapped to the perimeter
    f32 dash_duty;        // GUI_OP_DASH: on-fraction of the period

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

    /* tex_idx 0 = solid-color convention, same as tess_rect_filled: GUI_OP_SELF, no texel
       consulted at all. */
    if ( tex_idx == 0 )
    {
        tex_idx = res_atlas_idx();
        s_tess.cur_ops |= GUI_OP_SELF;
        u0 = v0 = u1 = v1 = 0.0f;
    }

    f32 cx  = x + hx,   cy  = y + hy;
    f32 rcs = 1.0f, rsn = 0.0f;                   /* the rotation, computed once per shape */
    if ( rot != 0.0f ) { rcs = cosf( rot ); rsn = sinf( rot ); }

    /* The style: the AUTHORED shape, after the clamps and the grid snap above.  The falloff
       skirt is a rasterization detail the vertex stage grows the covering by (GUI_QUAD_RULE_
       SKIRT) -- the field the fragment resolves is measured from the real boundary, so the
       record states that one.  All four radii travel. */
    s_tess.cur_prim.field   = (u32)GUI_FX_BOX;
    s_tess.cur_prim.r_tl    = rq[ 0 ];
    s_tess.cur_prim.r_tr    = rq[ 1 ];
    s_tess.cur_prim.r_br    = rq[ 2 ];
    s_tess.cur_prim.r_bl    = rq[ 3 ];
    s_tess.cur_prim.feather = feather;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & ( GUI_OP_BAND | GUI_OP_FRAME ) ) ? border : 0.0f;
    s_tess.cur_prim.rot_cos = rcs;
    s_tess.cur_prim.rot_sin = rsn;
    s_tess.cur_prim.param_a = ( s_tess.cur_ops & GUI_OP_PULSE ) ? rate  : 0.0f;
    s_tess.cur_prim.param_b = ( s_tess.cur_ops & GUI_OP_PULSE ) ? depth : 0.0f;

    /* The corner profile -- ambient over the command, like the ops, and applied only where there
       is a corner to profile: a square box has no arc to reshape, and leaving the lane at zero is
       what keeps square fills deduping onto one record. */
    s_tess.cur_prim.param_c = ( rmax > 0.0f ) ? s_tess.cur_corner_pow : 0.0f;

    /* DITHER, derived rather than asked for: a wide falloff and a colour ramp are the two shapes
       that band on an 8-bit target, and half a step of screen noise is invisible everywhere else
       it could apply.  The 1 px AA feather stays clean -- there is no ramp to band. */
    if ( feather > 2.0f || ( s_tess.cur_ops & GUI_OP_GRAD ) )
        s_tess.cur_ops |= GUI_OP_DITHER;

    /* The animation lane and the perimeter dash, written only under the op that reads each --
       the zero-when-unused rule that keeps plain fills deduping onto one record. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_DASH ) )
    {
        s_tess.cur_prim.dash_period = aux->dash_period;
        s_tess.cur_prim.dash_duty   = aux->dash_duty;
    }
    if ( aux && ( s_tess.cur_ops & ( GUI_OP_DASH | GUI_OP_PULSE | GUI_OP_SPIN ) ) )
    {
        s_tess.cur_prim.anim_rate  = aux->anim_rate;
        s_tess.cur_prim.anim_phase = aux->anim_phase;
    }

    /* GUI_OP_FRAME -- the border band's colour.  col_b is free here by construction: the frame
       is a solid fill, and the op never pairs with GRAD (gui.h). */
    if ( aux && ( s_tess.cur_ops & GUI_OP_FRAME ) )
        s_tess.cur_prim.col_b = aux->frame_col;

    /* GUI_OP_GRAD -- the ramp's far colour and its axis.  The axis is stored ALREADY DIVIDED by
       the box's extent along it (the support width of a projected rectangle), so the ramp spans
       the shape at any angle and the fragment recovers t with one dot product instead of
       repeating this per pixel.  A conic ramp has no extent to divide by -- it measures an ANGLE
       from the axis -- so it stores the unit direction it peaks toward. */
    if ( aux && ( s_tess.cur_ops & GUI_OP_GRAD ) )
    {
        s_tess.cur_prim.col_b    = aux->grad_col;
        s_tess.cur_prim.grad_mid = aux->grad_mid;

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

    /* The COVERING: one quad, the shape's true extents under the SKIRT rule (the vertex stage
       grows them by the style's feather pad).  The old vertex path carved an interior hole out
       of wide BAND/CUT/INSET surfaces; a record stores one rectangle, so the interior rasterizes
       at zero coverage instead -- the fields early-out cheaply and UI fill is nowhere near
       bound.  The uv rect is the authored span; the vertex stage scales it over the skirt and
       clamps at the corners, so a textured rounded quad shows its picture at authored size. */
    tess_quad_push( cx, cy, hx, hy, GUI_QUAD_RULE_SKIRT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr );
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

/* Tessellate a solid triangle into s_tess: the GUI_FX_TRI field -- one quad over the bbox,
   three points about its centre in the style's radius + param lanes.  Centre-relative points
   are what lets repeated arrow glyphs share one style; the edges antialias through the shared
   feather, which the old rasterized triangle never had. */
static void
tess_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    f32 lox = fminf( ax, fminf( bx, cx ) ), hix = fmaxf( ax, fmaxf( bx, cx ) );
    f32 loy = fminf( ay, fminf( by, cy ) ), hiy = fmaxf( ay, fmaxf( by, cy ) );
    if ( hix <= lox || hiy <= loy )
        return;
    f32 qx = ( lox + hix ) * 0.5f, qy = ( loy + hiy ) * 0.5f;

    s_tess.cur_ops         |= GUI_OP_SELF;
    s_tess.cur_prim.field   = (u32)GUI_FX_TRI;
    s_tess.cur_prim.r_tl    = ax - qx;
    s_tess.cur_prim.r_tr    = ay - qy;
    s_tess.cur_prim.r_br    = bx - qx;
    s_tess.cur_prim.r_bl    = by - qy;
    s_tess.cur_prim.param_a = cx - qx;
    s_tess.cur_prim.param_b = cy - qy;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_prim.rot_cos = 1.0f;
    tess_quad_push( qx, qy, ( hix - lox ) * 0.5f, ( hiy - loy ) * 0.5f,
                    GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr );
}

/*==============================================================================================
    tess_circle_filled -- a disc, which is a rounded box whose radius reached its half-extent.

    There is no circle primitive anywhere in the pipeline and there does not need to be one:
    tess_fx_box clamps the corner radius to half the short side, so a SQUARE box asking for a
    radius of its own half-extent degenerates exactly to a disc -- same field, same one quad,
    same fragment, antialiased at any size.  The emit side agrees (draw_push_circle_filled emits
    GUI_CMD_RECT_FILLED with rounding = r); this helper survives for tess_fx_arc's full-turn PIE
    route.

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
    tess_fx_ngon -- a regular polygon (GUI_FX_NGON) in one quad.

    The polyline fan this replaces sampled up to 64 perimeter points and strung them through the
    ribbon; the field resolves the exact boundary at any size, and a stroked form is the same
    quad under GUI_OP_BAND -- set by the replay case before this runs, the ambient-ops rule every
    shape follows.  The record's row [2] re-partitions for the field: r_tl is the corner
    rounding, r_tr the side count (gui.h).
==============================================================================================*/

static void
tess_fx_ngon( f32 pcx, f32 pcy, f32 r, u32 sides, f32 rot, f32 rounding,
              f32 border, u32 abgr )
{
    if ( r <= 0.0f )
        return;

    s_tess.cur_prim.field   = (u32)GUI_FX_NGON;
    s_tess.cur_prim.r_tl    = rounding;
    s_tess.cur_prim.r_tr    = (f32)sides;
    s_tess.cur_prim.feather = TESS_FX_AA;
    s_tess.cur_prim.border  = ( s_tess.cur_ops & GUI_OP_BAND ) ? border : 0.0f;
    s_tess.cur_prim.rot_cos = cosf( rot );
    s_tess.cur_prim.rot_sin = sinf( rot );

    /* The circumcircle covering under SKIRT: r on both axes, grown by the pad the vertex stage
       derives from the feather.  Rotation-safe -- a rotated square covering of a circle is
       still a covering. */
    s_tess.cur_ops |= GUI_OP_SELF;
    tess_quad_push( pcx, pcy, r, r, GUI_QUAD_RULE_SKIRT, 0, 0, res_atlas_idx(), abgr );
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
                    u32 abgr, u32 col_b, f32 grad_ang, u32 grad_kind, f32 grad_mid )
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
        aux.grad_mid = grad_mid;
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
             gui_fx_mode_t mode, f32 uvx, f32 uvy, f32 spin_rate, f32 spin_phase, u32 abgr )
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

    /* A SPINNING sector sweeps the whole disc over time while its retained vertices never move,
       so the quad must cover every orientation the fragment will ever resolve -- the disc's own
       bounding box, not the sector's. */
    if ( spin_rate != 0.0f )
    {
        xext = ra + rb + pad;
        ymax = ra + rb + pad;
        ymin = -ymax;
    }

    /* The record.  (cm, sm) is the sector's own frame -- the bisector direction the local
       coordinate above is expressed in -- so it goes where every other field's turn goes. */
    s_tess.cur_prim.field   = (u32)mode;
    s_tess.cur_prim.rot_cos = cm;
    s_tess.cur_prim.rot_sin = sm;
    s_tess.cur_prim.param_a = ra;
    s_tess.cur_prim.param_b = rb;
    s_tess.cur_prim.param_c = ap;

    /* GUI_OP_SPIN -- the whole frame (aperture, dashes, everything the record states) rotates at
       anim_rate turns/sec on pc.time.  The record is byte-identical every frame it runs, which is
       the point: the spinner joins the pulse in re-tessellating nothing. */
    if ( spin_rate != 0.0f )
    {
        s_tess.cur_ops |= GUI_OP_SPIN;
        s_tess.cur_prim.anim_rate  = spin_rate;
        s_tess.cur_prim.anim_phase = spin_phase;
    }

    /* These two carry their parameter pair in the uv lanes instead of texcoords, which is what the
       self-sampled bit announces: the fragment forces coverage to 1 and never reads the texel, so
       the white texel is not needed and the atlas stays bound only to keep the index valid. */
    f32 wu = 0.0f, wv = 0.0f;
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

    /* The sector's frame is a REFLECTION, which the vertex stage's rotation cannot reproduce,
       so the covering goes out under the BBOX rule -- axis-aligned half-extents that reach
       every reflected corner from the SHAPE centre (the fragment's rotation origin).  The fold
       to the centre wastes the asymmetric slack a tight bbox would trim; sectors are small. */
    s_tess.cur_ops |= GUI_OP_SELF;
    f32 bhx = 0.0f, bhy = 0.0f;
    for ( u32 i = 0; i < 4; ++i )
    {
        f32 lx = lsx[ i ] * xext;
        f32 ly = lsy[ i ] ? ymax : ymin;
        f32 rx = -sm * lx + cm * ly;
        f32 ry =  cm * lx + sm * ly;
        if ( fabsf( rx ) > bhx ) bhx = fabsf( rx );
        if ( fabsf( ry ) > bhy ) bhy = fabsf( ry );
    }
    tess_quad_push( pcx, pcy, bhx, bhy, GUI_QUAD_RULE_BBOX,
                    gui_uv_pack( wu, wv ), gui_uv_pack( wu, wv ),
                    res_atlas_idx(), abgr );
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

    s_tess.cur_prim.field   = (u32)GUI_FX_CHECKER;
    s_tess.cur_prim.param_a = cell;
    s_tess.cur_prim.param_b = phx;
    s_tess.cur_prim.param_c = phy;
    s_tess.cur_prim.col_b   = col_b;
    s_tess.cur_ops |= GUI_OP_SELF;

    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), col_a );
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

    s_tess.cur_prim.field   = (u32)GUI_FX_GRID;
    s_tess.cur_prim.param_a = cell;
    s_tess.cur_prim.param_b = thickness;
    s_tess.cur_prim.param_c = angle;
    s_tess.cur_prim.r_tl    = phx;
    s_tess.cur_prim.r_tr    = phy;
    if ( stripes )
        s_tess.cur_ops |= GUI_OP_STRIPES;
    s_tess.cur_ops |= GUI_OP_SELF;

    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), abgr );
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

            /* Address the glyph by its stable table id (gui_glyph_table.c) so the quad's uv
               resolves at draw time -- an atlas repack rewrites the table in place and retained
               text never re-tessellates for it.  No table (or no entry) leaves the ambient id
               unset and the baked-uv fallback below still renders. */
            if ( glyph_table_idx() != 0 )
                s_tess.cur_glyph = glyph_table_slot( font_active_id(), cp );

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                /* Whole glyph (or no clipping): emit as-is -- the hot interior path. */
                tess_rect_filled( gx0, y + oy, gw, gh, u0, v0, u1, v1, tex, abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                /* Straddler: cut to the window and walk U by the same fraction on each cut edge.
                   The same fractions ride the quad's cut lane, so an id-addressed straddler cuts
                   the TABLE's rect identically. */
                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                f32 f0   = 0.0f, f1 = 1.0f;
                if ( nx0 < clip_x0 )    /* left edge cut  */
                {
                    f0  = ( clip_x0 - gx0 ) / gw;
                    nu0 = u0 + du * f0;
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )    /* right edge cut */
                {
                    f1  = ( clip_x1 - gx0 ) / gw;
                    nu1 = u0 + du * f1;
                    nx1 = clip_x1;
                }
                s_tess.cur_glyph_cut = (u32)( f0 * 65535.0f + 0.5f )
                                     | ( (u32)( f1 * 65535.0f + 0.5f ) << 16 );
                tess_rect_filled( nx0, y + oy, nx1 - nx0, gh, nu0, v0, nu1, v1, tex, abgr );
            }
            /* else: glyph wholly outside the window -- drop it. */

            s_tess.cur_glyph     = ~0u;
            s_tess.cur_glyph_cut = 0;
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )   /* cursor past the window: nothing further is visible */
            break;
    }
}

/* One textured quad placed by an affine map: the local rect (lx, ly, lw, lh) is rotated by the
   prebuilt (cs, sn) and translated to the run origin (px, py) -- centre mapped through the
   transform, half-extents stored true, the style's rot pair doing the turn in the vertex stage.
   One style per (angle x scale) run: every glyph of a transformed run shares it.
   One thing this does NOT do: SNAP.  tess_rect_filled floors the origin to the pixel grid so
   straight edges stay crisp, which is right for chrome and wrong here twice over.  Snapping only
   the origin of a rotated quad moves the whole shape without straightening anything, and
   snapping a scaled run's per-glyph origins quantizes the advances -- the pen drifts by up to
   half a pixel per glyph and the word visibly breathes as the scale animates.  A transformed run
   is sub-pixel by nature; the distance field is what makes that legible (gui.h, GUI_TEX_SDF). */
static void
tess_quad_xf( f32 px, f32 py, f32 cs, f32 sn,
              f32 lx, f32 ly, f32 lw, f32 lh,
              f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr )
{
    f32 ccx = lx + lw * 0.5f, ccy = ly + lh * 0.5f;
    s_tess.cur_prim.rot_cos = cs;
    s_tess.cur_prim.rot_sin = sn;
    tess_quad_push( px + ccx * cs - ccy * sn, py + ccx * sn + ccy * cs,
                    lw * 0.5f, lh * 0.5f, GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr );
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
        {
            /* Stable glyph id, exactly as the 1:1 run does -- the uv rect is scale-independent,
               so a transformed run is repack-safe too. */
            if ( glyph_table_idx() != 0 )
                s_tess.cur_glyph = glyph_table_slot( font_active_id(), cp );
            tess_quad_xf( x, y, cs, sn,
                          ( pen + ox ) * scale, oy * scale, gw * scale, gh * scale,
                          u0, v0, u1, v1, tex, abgr );
            s_tess.cur_glyph = ~0u;
        }

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
    f32 half = thickness * 0.5f;
    f32 umax = len / period;                     /* number of tiled periods -> U span */
    f32 vv   = res_atlas_dash_v( duty );

    /* U runs 0..1 in the quad's uv lanes and is multiplied back up to `umax` periods by the
       fragment: the packed UV cannot hold a coordinate past 1, and the sampler's REPEAT-U is
       what tiles the atlas dash row (gui.h, GUI_FX_TILE_U).  The line's direction is the style's
       rot pair -- a style per direction; dashed lines are rare enough that the dedup loss is
       noise. */
    s_tess.cur_prim.field   = (u32)GUI_FX_TILE_U;
    s_tess.cur_prim.param_a = umax;
    s_tess.cur_prim.rot_cos = ux;
    s_tess.cur_prim.rot_sin = uy;
    tess_quad_push( ( x0 + x1 ) * 0.5f, ( y0 + y1 ) * 0.5f, len * 0.5f, half,
                    GUI_QUAD_RULE_EXACT,
                    gui_uv_pack( 0.0f, vv ), gui_uv_pack( 1.0f, vv ),
                    res_atlas_idx(), abgr );
}

/*==============================================================================================
    tess_stroke_poly_aa -- the polyline as a CAPSULE CHAIN: one SEG quad per segment, endpoints
    offset along the miter normals so alignment matches the old ribbon stroker.  Every quad
    resolves to ONE shared style (the direction rides the quad, tess_fx_segment), so a long path
    costs segments, not records.  Joins are the round caps overlapping, which composites darker
    on a translucent stroke -- the accepted trade for retiring the miter ribbon.
    abgr is pre-baked (alpha folded in at emit time).  v2 / seg_normal / stroke_center_offset
    are defined in gui_emit_path.c (included before this file in the unity build).
==============================================================================================*/

static void tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 border,
                             u32 abgr );   /* the quad backend's polyline expansion, defined below */

static void
tess_stroke_poly_aa( const gui_vec2_t* pts, u32 n, f32 thickness, f32 center_off,
                     bool closed, u32 abgr )
{
    if ( n < 2 )
        return;
    if ( n > GUI_MAX_PATH_PTS )
         n = GUI_MAX_PATH_PTS;

    /* Sub-pixel coverage: hold a 1px footprint, fade peak alpha by the requested thickness.
       Done here rather than left to tess_fx_segment's own clamp so the fold below is already
       final -- the segment's re-fold is then a no-op and every segment shares one colour. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )
    {
        a_scale   = thickness < 0.0f ? 0.0f : thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

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

    for ( u32 s2 = 0; s2 < seg; ++s2 )
    {
        u32 j = ( s2 + 1u ) % n;
        tess_fx_segment( pts[ s2 ].x + nrm[ s2 ].x * center_off,
                         pts[ s2 ].y + nrm[ s2 ].y * center_off,
                         pts[ j ].x + nrm[ j ].x * center_off,
                         pts[ j ].y + nrm[ j ].y * center_off,
                         thickness, 0.0f, col );
    }
}

/*==============================================================================================
    tess_fx_segment -- one line segment as a CAPSULE distance field: the distance from a point
    to a segment, minus the half-thickness.  One quad under the CAPSULE rule, an edge that is
    correct at any angle, and round caps that cost nothing because they ARE the field.

    Round caps extend half a thickness past each endpoint.  On a polyline (the capsule chain,
    tess_stroke_poly_aa) they are also the JOINS: neighbouring capsules overlap there, which
    composites darker on a translucent stroke -- the accepted cost of per-segment records.

    Axis-aligned single lines never come here: gui_draw_line routes them through a grid-snapped
    rect at EMIT (stroke_axis_aligned_rect, gui_emit_path.c), which is crisper than any field
    since a horizontal edge has nothing to antialias.
==============================================================================================*/

static void
tess_fx_segment( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 border, u32 abgr )
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
    f32 r   = thickness * 0.5f;             /* the capsule radius             */
    f32 hl  = len * 0.5f;                   /* half-length: what q.x subtracts */
    f32 mx  = ( x0 + x1 ) * 0.5f, my = ( y0 + y1 ) * 0.5f;

    /* The style states the capsule's radius in the corner-radius lane.  The DIRECTION rides the
       quad's uv lanes (a unorm16-mapped unit vector), NOT the style's rot pair, so every segment
       of every stroke at one thickness shares ONE style -- the dedup a per-direction rot pair
       would forfeit on the shape polylines expand into.  The half-length rides the quad
       (rect.z); the fragment reads the direction back from the interpolated uv. */
    s_tess.cur_prim.field   = (u32)GUI_FX_SEG;
    s_tess.cur_prim.r_tl    = r;
    s_tess.cur_prim.feather = TESS_FX_AA;

    /* A HOLLOW capsule is the same field under GUI_OP_BAND -- the op that makes a rounded outline
       out of a filled box, reaching this shape because an op modifies whatever field arrived.  A
       border at or past the radius has no interior left to remove, so it stays filled rather than
       inverting into one. */
    if ( border > 0.0f && border < r )
    {
        s_tess.cur_ops         |= GUI_OP_BAND;
        s_tess.cur_prim.border  = border;
    }

    s_tess.cur_ops |= GUI_OP_SELF;
    u32 dir = gui_uv_pack( ux * 0.5f + 0.5f, uy * 0.5f + 0.5f );
    tess_quad_push( mx, my, hl, r, GUI_QUAD_RULE_CAPSULE, dir, dir,
                    res_atlas_idx(), col );
}

/* Volatile-widget seam (render/pipeline/gui_build_volatile.c, included right after this file in
   the gui_render.c unity build).  tess_dispatch calls volatile_range_close once a tagged command
   RANGE's vertices/indices/GPU commands are fully written; it records the block's slot-relative
   position, reserves padded headroom past the live geometry (advancing this file's write heads),
   and stamps the slot tessellation generation.  s_volatile_patching is defined HERE (first in the
   TU) and set by volatile_patch around its scratch re-tessellation so the range tracking below
   stays inert during a patch -- a patch must never look like a fresh capture. */
static bool s_volatile_patching;
static void volatile_range_close( gui_id_t id, u32 vb_open, u32 pb_open, u32 cmd_open );

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
    u32      vb_open = 0, pb_open = 0, cmd_open = 0;
    (void)win;

    for ( u32 oi = 0; oi < count; ++oi )
    {
        u32              ci = order[ oi ];
        const gui_cmd_t* c  = &cmds[ ci ];

        gui_id_t vid = s_volatile_patching ? GUI_ID_NONE : s_draw.cmd_volatile_id[ ci ];
        if ( vid != open_vid )
        {
            if ( open_vid != GUI_ID_NONE )
                volatile_range_close( open_vid, vb_open, pb_open, cmd_open );
            open_vid = vid;

            /* The dedup floor rises at BOTH sides of a volatile boundary.  Entering: the block's
               first primitive must append a record INSIDE its own range instead of reusing the
               window's preceding one -- a reused record would sit outside the reservation the
               patch is allowed to rewrite, and the patch (which always starts cold) would then
               disagree with the capture about how many it needs.  Leaving: the block's records
               ARE that rewritable reservation, so a later command deduping onto one would be
               corrupted by the next patch. */
            s_tess.prim_dedup_floor = s_tess.prim_count;

            if ( vid != GUI_ID_NONE )
            {
                s_tess.force_new_cmd = true;   /* block owns its GPU commands from the first primitive */
                vb_open  = s_tess.vert_count;
                pb_open  = s_tess.prim_count;
                cmd_open = s_tess.cmd_count;
            }
        }

        s_tess.cur_clip       = s_draw.clip_table[ c->clip_idx ];
        s_tess.cur_clip_local = tess_clip_local( c->clip_idx );
        s_tess.cur_vp         = c->vp;

        /* The op word is ambient over ONE command and cleared here, so a case that sets it
           cannot leak the effect onto the next primitive.  That containment is the whole reason
           it can be ambient at all -- it lets an outline reach every glyph of a run without
           threading a parameter through tess_rect_filled, which every fill in the library
           shares. */
        s_tess.cur_ops        = 0u;
        s_tess.cur_corner_pow = 0.0f;
        s_tess.cur_glyph      = ~0u;
        s_tess.cur_glyph_cut  = 0u;

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
                {
                    s_tess.cur_corner_pow = c->rect.corner_pow;
                    tess_fx_box( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                 c->rect.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, 0.0f,
                                 c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                 c->rect.tex_idx, c->rect.abgr, NULL );
                }
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
                    s_tess.cur_ops       |= GUI_OP_BAND;
                    s_tess.cur_corner_pow = c->rect_outline.corner_pow;
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

            /* Body + border in one surface.  A square frame runs the field with feather 0 -- a
               hard cut on the snapped boundary, matching the crisp edges the fill + four-rail
               pair drew -- and a rounded one takes the standard AA band. */
            case GUI_CMD_FRAME:
                s_tess.cur_ops       |= GUI_OP_FRAME;
                s_tess.cur_corner_pow = c->frame.corner_pow;
                {
                    tess_fx_aux_t aux = { 0 };
                    aux.frame_col     = c->frame.col_border;
                    tess_fx_box( c->frame.x, c->frame.y, c->frame.w, c->frame.h,
                                 c->frame.rounding,
                                 ( c->frame.rounding > 0.0f ) ? TESS_FX_AA : 0.0f,
                                 c->frame.t, 0.0f, 0.0f, 0.0f,
                                 0, 0, 1, 1, 0, c->frame.abgr, &aux );
                }
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
                    aux.cut_dx     = c->fx_box.cut_dx;
                    aux.cut_dy     = c->fx_box.cut_dy;
                    aux.anim_phase = c->fx_box.phase;
                    s_tess.cur_corner_pow = c->fx_box.corner_pow;
                    tess_fx_box( c->fx_box.x, c->fx_box.y, c->fx_box.w, c->fx_box.h,
                                 c->fx_box.rounding, c->fx_box.feather, 0.0f,
                                 c->fx_box.rate, c->fx_box.depth, c->fx_box.rot,
                                 0, 0, 1, 1, 0, c->fx_box.abgr, &aux );
                }
                break;

            /* Four radii and a ramp -- and still one surface, one command and no batch split,
               exactly like the uniform fill it generalizes. */
            case GUI_CMD_ROUND_RECT_EX:
                s_tess.cur_corner_pow = c->round_rect.corner_pow;
                tess_round_rect_ex( c->round_rect.x, c->round_rect.y,
                                    c->round_rect.w, c->round_rect.h,
                                    c->round_rect.rtl, c->round_rect.rtr,
                                    c->round_rect.rbr, c->round_rect.rbl,
                                    c->round_rect.feather, c->round_rect.abgr,
                                    c->round_rect.col_b, c->round_rect.grad_ang,
                                    c->round_rect.grad_kind, c->round_rect.grad_mid );
                break;

            /* The sectors share their geometry and differ only in the field the fragment
               evaluates: round caps on a band, sharp radial edges on a wedge, and the two
               self-sampled variants whose extra word rides the quad's flat uv. */
            case GUI_CMD_ARC:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, c->arc.thickness,
                             c->arc.a0, c->arc.a1, GUI_FX_ARC, 0.0f, 0.0f,
                             c->arc.spin_rate, c->arc.spin_phase, c->arc.abgr );
                break;

            case GUI_CMD_PIE:
                tess_fx_arc( c->arc.cx, c->arc.cy, c->arc.r, 0.0f,
                             c->arc.a0, c->arc.a1, GUI_FX_PIE, 0.0f, 0.0f,
                             c->arc.spin_rate, c->arc.spin_phase, c->arc.abgr );
                break;

            /* uv lane packing is the shader contract for the self-sampled pair (gui.h): DASH
               sends (period / TAU, duty), GRAD splits col_b's four bytes across the two unorm16
               lanes.  Both values are k/65535 exact through the pack and back. */
            case GUI_CMD_ARC_DASH:
                tess_fx_arc( c->arc_dash.cx, c->arc_dash.cy, c->arc_dash.r,
                             c->arc_dash.thickness, c->arc_dash.a0, c->arc_dash.a1,
                             GUI_FX_ARC_DASH,
                             c->arc_dash.period / TESS_TAU, c->arc_dash.duty,
                             0.0f, 0.0f, c->arc_dash.abgr );
                break;

            case GUI_CMD_ARC_GRAD:
                tess_fx_arc( c->arc_grad.cx, c->arc_grad.cy, c->arc_grad.r,
                             c->arc_grad.thickness, c->arc_grad.a0, c->arc_grad.a1,
                             GUI_FX_ARC_GRAD,
                             (f32)(   c->arc_grad.col_b         & 0xFFFFu ) / 65535.0f,
                             (f32)( ( c->arc_grad.col_b >> 16 ) & 0xFFFFu ) / 65535.0f,
                             0.0f, 0.0f, c->arc_grad.col_a );
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

            /* The regular polygon: filled, or stroked under GUI_OP_BAND -- the op set here for
               the reason every shape's is (the record's band width is sized from it). */
            case GUI_CMD_NGON:
                if ( c->ngon.thickness > 0.0f )
                    s_tess.cur_ops |= GUI_OP_BAND;
                tess_fx_ngon( c->ngon.cx, c->ngon.cy, c->ngon.r, c->ngon.sides,
                              c->ngon.rot, c->ngon.rounding, c->ngon.thickness,
                              c->ngon.abgr );
                break;

            /* The dashed border: a BAND box whose coverage the fragment cuts on the perimeter
               coordinate (GUI_OP_DASH).  The CPU's share is the SNAP: fit a whole number of
               dash cycles to the perimeter, computed from the same clamped radius the record
               will state, so the pattern meets itself where the walk closes. */
            case GUI_CMD_BOX_DASH:
            {
                f32 hw  = c->box_dash.w * 0.5f, hh = c->box_dash.h * 0.5f;
                f32 lim = ( hw < hh ) ? hw : hh;
                f32 r   = c->box_dash.rounding;
                if ( r > lim )  r = lim;
                if ( r < 0.0f ) r = 0.0f;

                f32 L      = 4.0f * ( hw + hh ) - 8.0f * r + TESS_TAU * r;
                f32 period = c->box_dash.dash + c->box_dash.gap;
                f32 n      = ( period > 0.0f ) ? floorf( L / period + 0.5f ) : 1.0f;
                if ( n < 1.0f ) n = 1.0f;

                tess_fx_aux_t aux = { 0 };
                aux.dash_period = L / n;
                aux.dash_duty   = c->box_dash.dash / period;
                aux.anim_rate   = c->box_dash.rate;
                aux.anim_phase  = c->box_dash.phase;

                s_tess.cur_ops |= GUI_OP_BAND | GUI_OP_DASH;
                tess_fx_box( c->box_dash.x, c->box_dash.y, c->box_dash.w, c->box_dash.h,
                             c->box_dash.rounding, TESS_FX_AA, c->box_dash.t,
                             0.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0, c->box_dash.abgr, &aux );
                break;
            }

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
                                 c->line.thickness, c->line.border, c->line.abgr );
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
        volatile_range_close( open_vid, vb_open, pb_open, cmd_open );

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/
