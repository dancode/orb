/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess.c -- CPU-side tessellation engine.

    Translates the frame's semantic gui_cmd_t list (s_draw) into packed vertex/index
    geometry in s_tess.  This is the CPU half of the command-list split: everything here
    reads semantic commands and writes gui_draw_vert_t / u16 index data; nothing here
    touches the GPU API.

    s_tess is read only by the two files included after: gui_build_cache.c (the BUILD phase
    fills it via tess_dispatch) and gui_submit.c (gui_render_flush uploads it and emits draw
    calls).  No file above the backend unit touches it.

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
    u32           vp;       // viewport index for this command
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
    u32         cur_vp;     /* viewport baked from the current semantic command                    */
    u32         cur_tex;    /* GUI_TEX_MODE | bindless slot stamped into every vertex committed --
                               set by tess_set_tex, applied by tess_verts_commit.  NOT a batch key */
    u32         cur_fx;     /* packed effect word stamped into every vertex committed.  CLEARED at
                               the top of each semantic command (tess_dispatch), so a primitive
                               that wants one sets it and nothing can inherit it afterwards.      */

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
    s_tess.force_new_cmd   = false;
    s_tess.overflow        = false;
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

/* Ensure a GPU command is open whose (clip, viewport) match the ambient pair, opening a new one at
   any mismatch.  THAT PAIR IS THE WHOLE BATCH KEY, which is why this takes no arguments: the
   texture and the effect word both travel per vertex and cannot cut a draw call, and z is
   per-segment rather than per-command (the segment system already guarantees every command in one
   window's tessellation pass shares a z).  A new primitive type therefore batches correctly by
   construction -- there is nothing left to pass in and get wrong.
   Returns false when the command table is full and no matching command is open -- the caller must
   drop its primitive, or its geometry would append to a command with the wrong clip. */
static bool
tess_ensure_gpu_cmd( void )
{
    if ( s_tess.cmd_count > 0 && !s_tess.force_new_cmd )
    {
        const tess_gpu_cmd_t* prev = &s_tess.gpu_cmds[ s_tess.cmd_count - 1 ];
        const gui_gpu_cmd_t*  cur  = &prev->cmd;
        if ( prev->vp == s_tess.cur_vp
          && cur->clip_rect.x   == s_tess.cur_clip.x
          && cur->clip_rect.y   == s_tess.cur_clip.y
          && cur->clip_rect.w   == s_tess.cur_clip.w
          && cur->clip_rect.h   == s_tess.cur_clip.h )
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
       tex_idx is the ambient texture at the moment the command opened, i.e. the FIRST primitive's,
       and is diagnostic only (the dashboard tooltip) -- the command may go on to span several. */
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
    gui_draw_vert_t* v = &s_tess.verts[ s_tess.vert_count ];
    for ( u32 i = 0; i < n; ++i )
    {
        v[ i ].tex = s_tess.cur_tex;
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

static void
tess_prim_commit( u32 nv, u32 ni )
{
    tess_verts_commit( nv );
    s_tess.idx_count  += ni;
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
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );

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
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );

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

/* Tessellate a hollow rectangle as four edge quads.  t is clamped to half the shorter side so a
   thick border on a small rect degenerates to a filled rect instead of inverted side quads. */
static void
tess_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    f32 tmax = ( w < h ? w : h ) * 0.5f;
    if ( t > tmax ) t = tmax;
    tess_rect_filled( x,         y,         w, t,     0,0,1,1, 0, abgr );
    tess_rect_filled( x,         y + h - t, w, t,     0,0,1,1, 0, abgr );
    tess_rect_filled( x,         y + t,     t, h-2*t, 0,0,1,1, 0, abgr );
    tess_rect_filled( x + w - t, y + t,     t, h-2*t, 0,0,1,1, 0, abgr );
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

    A RING carves its interior out: the band it paints is only `border` px wide, and rasterizing
    the whole inside of a window frame at zero coverage is a real cost on a large panel.  Each
    quadrant then emits an L (two quads) around the hole instead of one, which is where the 8/32
    upper bound below comes from.

    Every SDF surface samples the same atlas as everything else and carries its mode per VERTEX,
    so it merges into whatever GPU command is already open: a soft shadow behind a panel costs
    a batch split of zero.
==============================================================================================*/

/* Emit one SDF surface.  `r` is the corner radius and `feather` the total width of the falloff band
   straddling the boundary (0 = hard edge); both are read by every mode.  The remaining parameters
   are MODE-SPECIFIC, mirroring the fx word's own re-partitioning -- `border` is the band width for
   GUI_FX_RING, `rate`/`depth` the wave for GUI_FX_PULSE, and each mode ignores the other's.  UVs
   span the AUTHORED box and are clamped over the grown skirt, so a textured rounded quad cannot
   bleed into its atlas neighbour where the coverage has already faded to nothing. */
static void
tess_fx_box( f32 x, f32 y, f32 w, f32 h, f32 r, f32 feather, f32 border, f32 rate, f32 depth,
             gui_fx_mode_t mode,
             f32 u0, f32 v0, f32 u1, f32 v1, u32 tex_idx, u32 abgr )
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
    if ( r > lim ) r = lim;                       /* a radius past half the short side is a capsule */
    if ( r > GUI_FX_RADIUS_MAX ) r = GUI_FX_RADIUS_MAX;
    if ( r < 0.0f ) r = 0.0f;
    if ( feather < 0.0f ) feather = 0.0f;
    if ( feather > GUI_FX_FEATHER_MAX ) feather = GUI_FX_FEATHER_MAX;
    if ( border  < 0.0f ) border  = 0.0f;
    if ( border  > GUI_FX_BORDER_MAX  ) border  = GUI_FX_BORDER_MAX;
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
       two straight edges. */
    if ( !( hx == hy && r >= lim ) )
    {
        x = floorf( x + 0.5f );
        y = floorf( y + 0.5f );
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
    f32 kx  = hx - r,   ky  = hy - r;             /* the centre rect: |p| - k is the effect coord */
    u32 fx  = ( mode == GUI_FX_PULSE ) ? gui_fx_pack_pulse( r, feather, rate, depth )
                                       : gui_fx_pack( mode, r, feather, border );

    /* The per-quadrant coverage, in quadrant-local |p| space: one box normally, two forming an L
       when a RING has an interior worth skipping.

       Sizing the hole is the whole subtlety.  It must lie entirely at d <= -(border + feather/2)
       -- one pixel of it inside the painted band would notch the frame -- and the binding case is
       its CORNER, which pokes diagonally toward the arc.  So the hole is the largest axis-aligned
       box inscribed in the shape ERODED by that reach, whose corner sits on the eroded arc at 45
       degrees.  Erode until nothing is left and there is simply no hole. */
    f32 lo_x[ 2 ] = { 0.0f, 0.0f }, lo_y[ 2 ] = { 0.0f, 0.0f };
    f32 hi_x[ 2 ] = { ehx,  0.0f }, hi_y[ 2 ] = { ehy,  0.0f };
    u32 nbox = 1;

    if ( mode == GUI_FX_RING )
    {
        f32 reach = border + feather * 0.5f;
        f32 er    = r - reach;                    /* the eroded shape's radius */
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
    s_tess.cur_fx = fx;      /* the shape's word, stamped onto all 16 (or 32) quadrant vertices */

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
                v[ n * 4 + c ] = gui_vert_fxc( px, py, tu, tv, abgr, ax - kx, ay - ky );
            }
            tess_quad_idx( &idx[ n * 6 ], (u16)( base + n * 4 ) );
        }
    }

    tess_prim_commit( nv, ni );
}

/* The antialiasing band a shape gets when nothing asked for a softer one -- one pixel, centred on
   the boundary.  Named because it is the difference between "rounded" and "rounded and crisp",
   and every rounded fill and frame in the library goes through it. */
#define TESS_FX_AA  1.0f

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

    There is no circle primitive here and there does not need to be one.  tess_fx_box clamps the
    corner radius to half the short side, so a SQUARE box asking for a radius of its own half-extent
    degenerates exactly to a disc -- same field, same four quadrant quads, same fragment.  What used
    to be here was a triangle fan, and it was wrong in the way every tessellated curve is wrong: the
    boundary was a polygon, so its smoothness was bought with segment count and its edge was not
    antialiased at all (fan vertices carry fx 0, so the fragment applied no coverage).  Callers paid
    for that in the only currency the fan understood -- sym_arc_segs scales with radius, up to 64
    segments for a large disc, and even a bullet point spent 12.

        fan, 12 seg   13 verts / 36 idx    faceted, aliased
        fan, 64 seg   65 verts / 192 idx   smooth, aliased
        the field     16 verts /  24 idx   exact at any size, antialiased

    It is cheaper in indices than even the smallest fan, and it is the only one of the three whose
    edge is actually correct.  `segs` is therefore ignored -- kept on the command (and in the public
    signature) because it costs nothing and every call site passing a count is still saying something
    true about the shape it wants, just no longer something this layer needs to be told.

    NOT grid-snapped, unlike every other shape that goes through tess_fx_box -- it does not have to
    ask for that, because tess_fx_box derives it: a square whose radius reached its half-extent has
    no straight edge for snapping to keep crisp, and quantizing a circle's centre is exactly what a
    small moving dot must not do.  A circular RING satisfies the same test, so the two stay aligned
    when drawn concentrically.
==============================================================================================*/

static void
tess_circle_filled( f32 pcx, f32 pcy, f32 r, u32 segs, u32 abgr )
{
    (void)segs;
    tess_fx_box( pcx - r, pcy - r, r * 2.0f, r * 2.0f,
                 r, TESS_FX_AA, 0.0f, 0.0f, 0.0f, GUI_FX_BOX,
                 0, 0, 1, 1, 0, abgr );
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

    for ( u32 i = 0; i < n && str[ i ]; ++i )
    {
        u8  ch = (u8)str[ i ];
        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( ch, &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );

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

    for ( u32 i = 0; i < n && str[ i ]; ++i )
    {
        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( (u8)str[ i ], &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );

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

/* Fast path for an axis-aligned line: a horizontal (y0==y1) or vertical (x0==x1) line has no
   diagonal edge to feather, so a crisp grid-snapped quad beats any field -- 4 verts / 6 idx against
   the capsule's 8 / 12, and crisper, because there is nothing to antialias.  This is the common
   case (separators, frame borders, table grid lines, underlines).  Sub-pixel thickness fades the
   alpha exactly as tess_stroke_poly_aa does so a hairline keeps its weight.  Returns false for a
   diagonal, which falls through to tess_fx_segment.

   BACKSTOP, not the live path: gui_draw_line already routes every H/V segment through
   stroke_axis_aligned_rect at EMIT (gui_emit_path.c), and that is the only producer of
   GUI_CMD_LINE, so this currently never fires.  It is kept because it is the tessellator's own
   guarantee -- a future producer that pushes GUI_CMD_LINE directly gets the crisp quad without
   having to know the emit layer's fast path exists. */
static bool
tess_axis_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr )
{
    bool horizontal = ( y0 == y1 );
    bool vertical   = ( x0 == x1 );
    if ( !horizontal && !vertical )
        return false;            /* diagonal -- needs the AA stroker */
    if ( thickness <= 0.0f )
        return true;             /* axis-aligned but nothing to draw -- consumed */

    /* Sub-pixel coverage: hold a 1px footprint and fade peak alpha (mirrors the AA stroker). */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f ) { a_scale = thickness; thickness = 1.0f; }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    /* Centre the band on the line, exactly as the stroker does (CMD_LINE passes center_off 0).
       tess_rect_filled grid-snaps the origin, so the quad lands crisp on the pixel grid. */
    f32 half = thickness * 0.5f;
    if ( horizontal )
    {
        f32 xa = x0 < x1 ? x0 : x1;
        f32 xb = x0 < x1 ? x1 : x0;
        tess_rect_filled( xa, y0 - half, xb - xa, thickness, 0, 0, 1, 1, 0, col );
    }
    else /* vertical */
    {
        f32 ya = y0 < y1 ? y0 : y1;
        f32 yb = y0 < y1 ? y1 : y0;
        tess_rect_filled( x0 - half, ya, thickness, yb - ya, 0, 0, 1, 1, 0, col );
    }
    return true;
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

   `order` is a permutation of [0,count): the indices grouped by clip within each z-run (built by
   cache_tess_window) so equal-clip commands tessellate contiguously and collapse into one GPU batch.
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

        s_tess.cur_clip = s_draw.clip_table[ c->clip_idx ];
        s_tess.cur_vp   = c->vp;

        /* The effect word is ambient over ONE command and cleared here, so a case that sets it
           cannot leak the effect onto the next primitive.  That containment is the whole reason it
           can be ambient at all -- it lets an outline reach every glyph of a run, and a shape's
           word reach all 16 of its quadrant vertices, without threading a parameter through
           tess_rect_filled, which every fill in the library shares. */
        s_tess.cur_fx = 0u;

        switch ( c->type )
        {
            /* A square rect keeps the one-quad fast path: it is pixel-aligned by construction, so
               there is no edge for an SDF to resolve and nothing to gain.  Rounding is what turns
               it into a surface -- and routing the TEXTURED case through as well is what finally
               lets a rounded quad carry an image, which the arc fan never could. */
            case GUI_CMD_RECT_FILLED:
                if ( c->rect.rounding > 0.0f )
                    tess_fx_box( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                 c->rect.rounding, TESS_FX_AA, 0.0f, 0.0f, 0.0f, GUI_FX_BOX,
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
                                 0.0f, 0.0f, GUI_FX_RING,
                                 0, 0, 1, 1, 0, c->rect_outline.abgr );
                else
                    tess_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                       c->rect_outline.w, c->rect_outline.h,
                                       c->rect_outline.t, c->rect_outline.abgr );
                break;

            /* One surface with a wide feather.  What used to be six stacked rects pretending to
               be a gaussian is now the exact same falloff the corners use, only spread out. */
            case GUI_CMD_SHADOW:
                tess_fx_box( c->shadow.x, c->shadow.y, c->shadow.w, c->shadow.h,
                             c->shadow.rounding, c->shadow.feather, 0.0f, 0.0f, 0.0f, GUI_FX_BOX,
                             0, 0, 1, 1, 0, c->shadow.abgr );
                break;

            /* Geometrically a plain rounded fill -- the only thing PULSE changes is the packed
               word, and therefore the fragment.  That is the point: the vertices this produces are
               correct for every frame the pulse runs, so the window's retained slot is never
               invalidated and the breathing costs no re-tessellation at all. */
            case GUI_CMD_PULSE:
                tess_fx_box( c->pulse.x, c->pulse.y, c->pulse.w, c->pulse.h,
                             c->pulse.rounding, TESS_FX_AA, 0.0f,
                             c->pulse.rate, c->pulse.depth, GUI_FX_PULSE,
                             0, 0, 1, 1, 0, c->pulse.abgr );
                break;

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

            /* A disc is a square SDF box whose radius reached its half-extent -- 16 verts, exactly
               antialiased, and `segs` no longer means anything (see tess_circle_filled). */
            case GUI_CMD_CIRCLE_FILLED:
                tess_circle_filled( c->circle.cx, c->circle.cy, c->circle.r,
                                    c->circle.segs, c->circle.abgr );
                break;

            /* Two paths, and neither is the ribbon stroker any more.  Axis-aligned takes the
               grid-snapped quad -- crisper than a field, because there is no diagonal edge to
               antialias.  Everything else is a CAPSULE: one segment has no joints, which is the
               only thing that kept the ribbon (see tess_fx_segment). */
            case GUI_CMD_LINE:
                if ( !tess_axis_line( c->line.x0, c->line.y0, c->line.x1, c->line.y1,
                                      c->line.thickness, c->line.abgr ) )
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
