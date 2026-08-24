/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess_quad.c -- the quad-record core.

    Part 2 of 7 of the CPU-side quad-record builder (see gui_build_tess_state.c for the family
    overview and the s_tess state every file here reads).  Owns tess_quad_push -- the ONE geometry
    writer every shape tessellator in the later files funnels through -- plus its supporting
    machinery: the pixel/cell quantizers, tess_reset, the ambient texture/clip/gpu-command
    resolvers, the style-record and fx-record dedup (tess_prim_local, tess_fx_local), the glyph
    table lookup, and the band-covering trade-off (tess_band_worth_it).  Also the three plain
    fills built directly on tess_quad_push: tess_rect_filled, tess_rect_glyph, tess_rect_gradient.

    Included right after gui_build_tess_state.c.

==============================================================================================*/
// clang-format off

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
    Tessellation helpers -- mirrors of the draw_push_* functions in the gui_emit_* files, but writing
    into s_tess instead of s_draw.  These are the backend half of the command-list split.
    Called from tess_dispatch; not called from anywhere else.
==============================================================================================*/

static void
tess_reset( void )
{
    s_tess.quad_count           = 0;
    s_tess.prim_count           = 0;
    s_tess.cmd_count            = 0;
    s_tess.slot_quad_base       = 0;
    s_tess.slot_cmd_base        = 0;
    s_tess.slot_prim_base       = 0;
    s_tess.slot_tess_gen        = 0;
    s_tess.cur_prim_local       = 0;
    s_tess.prim_dedup_floor     = 0;
    s_tess.prim_memo_valid      = false;  /* base/floor return to 0, the arena does not */
    s_tess.cmd_hint             = GUI_PAL_NONE;
    s_tess.fx_page              = s_tess.fx_page_used = s_tess.fx_memo_row = 0;
    s_tess.fx_page_count        = 0;
    s_tess.fx_row_count         = 0;
    s_tess.slot_clips           = NULL;
    s_tess.slot_clip_count      = NULL;
    s_tess.slot_clip_pending    = NULL;
    s_tess.clip_memo_ci         = 0xFF;
    s_tess.cur_clip_local       = 0;
    s_tess.cur_is_text          = false;
    s_tess.slot_text_quads      = 0;
    s_tess.slot_text_runs       = 0;
    s_tess.force_new_cmd        = false;
    s_tess.overflow             = 0u;
    s_tess.cur_col_border       = 0;
    s_tess.cur_rot_c            = 1.0f;
    s_tess.cur_rot_s            = 0.0f;
    s_tess.cur_phase            = 0.0f;
    s_tess.cur_swell            = 0.0f;
}

/* Name the texture the next quad's style will CARRY (tess_quad_push folds it into the record).
   Deliberately NOT part of opening a batch, and separated from it so that reads: the texture
   rides the prim record, so a texture change costs nothing and must not open a command. */

static void
tess_set_tex( u32 tex_idx )
{
    s_tess.cur_tex = tex_idx;
}

/* Resolve the ambient clip to an ABSOLUTE entry index in the frame clip region: the window's
   fixed slab base plus its position in the slot's LOCAL clip table, appending a new entry at
   first sight.  Content-keyed -- the full entry (rect, radius, feather, flags, value) is the
   identity -- and first-seen ordered
   over the window's own commands, so a window whose commands hash identical reproduces identical
   indices: the property that lets a cached quad bake its clip entry.  The slab base
   is keyed by the window's id-keyed stable cache slot, so the absolute index survives as long as
   the window does.  An append marks the slot's upload mask -- the flush re-uploads a slab only
   when its content changed.  The memo serves the common run of consecutive same-clip commands.
   A slot past GUI_WIN_CLIP_MAX distinct clips falls back to its slab's entry 0 -- degrading INSIDE
   the window (its own first clip, usually the window rect) rather than borrowing a neighbour's
   slab.  Reported through the same overflow path as the arenas, because that many distinct clips
   in one window is a bug, not a budget. */

static u32
tess_clip_local( u8 ci )
{
    if ( s_tess.clip_memo_ci == ci )
        return s_tess.clip_memo_local;
    s_tess.clip_memo_ci = ci;

    if ( !s_tess.slot_clips )
        return s_tess.clip_memo_local = 0u;

    const gui_rect_t* r   = &s_draw.clip_table[ ci ];
    f32               rad = s_draw.clip_radius[ ci ];
    f32               fea = s_draw.clip_feather[ ci ];
    u32               flg = s_draw.clip_flags[ ci ];
    u32               val = s_draw.clip_value[ ci ];
    u32               n   = *s_tess.slot_clip_count;
    for ( u32 g = 0; g < n; ++g )
    {
        const gui_clip_entry_t* e = &s_tess.slot_clips[ g ];
        if ( e->rect.x == r->x && e->rect.y == r->y && e->rect.w == r->w && e->rect.h == r->h
          && e->radius == rad && e->feather == fea && e->flags == flg && e->value == val )
            return s_tess.clip_memo_local = g;
    }
    if ( n >= GUI_WIN_CLIP_MAX )
    {
        s_tess.overflow |= TESS_OVF_WIN_CLIPS;
        return s_tess.clip_memo_local = 0u;
    }
    s_tess.slot_clips[ n ] = ( gui_clip_entry_t ){
        .rect    = *r,
        .radius  = rad,
        .feather = fea,
        .flags   = flg,
        .value   = val,
    };
    *s_tess.slot_clip_count = n + 1;
    if ( s_tess.slot_clip_pending )
        *s_tess.slot_clip_pending = 0xFF;
    return s_tess.clip_memo_local = n;
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
        s_tess.overflow |= TESS_OVF_CMDS;
        return false;
    }
    s_tess.force_new_cmd = false;

    /* Quad span of this command starts at the current quad_count; the next command's qbase (or
       the final quad_count for the last) bounds it.  Lets a surface upload only its own quads.
       tex_idx is the ambient value at the moment the command opened, i.e. the FIRST primitive's,
       and is diagnostic only (the dashboard tooltip) -- it rides the quad now and the command
       may go on to span several. */

    s_tess.gpu_cmds[ s_tess.cmd_count++ ] = ( tess_gpu_cmd_t ){
        .elem_count = 0,
        .tex_idx    = s_tess.cur_tex,
        .vp         = (i16)s_tess.cur_vp,
        .qbase      = (u16)s_tess.quad_count,
    };
    return true;
}

/* Resolve the ambient primitive record to a SLOT-LOCAL index in the frame's record arena,
   appending a new entry when the ambient state has moved.  The counterpart of tess_clip_local, and
   deliberately the same shape -- but keyed on CONTENT rather than on a table index, because the
   record is assembled from half a dozen ambient fields and nothing upstream has a name for the
   combination.

   The memos are the whole performance story, and they are read in order of what each can
   cover: the last ANSWER, then the last few ARENA APPENDS, then the palette.  A glyph run is
   one semantic command emitting hundreds of quads under one unchanging record, and a run of
   flat fills sharing a texture and a clip is the same, so each collapses onto one entry and
   most quads never reach past the first compare.  That only works because emitters leave the
   fields their field does not read at zero (gui.h) -- a writer that stamped its rect into a
   GUI_FX_NONE record would give every fill an entry of its own.

   Past the arena a slot degrades to its own first record, mirroring tess_clip_local's fallback to
   slab entry 0: a wrong shape is bad, but a wild index into a storage buffer is worse.  The
   TESS_OVF_PRIMS flag is what actually reports it. */

/* How far back the dedup scan reaches.  1 collapses a homogeneous run (a glyph run, consecutive
   flat fills); the extra depth collapses the ALTERNATION chrome actually emits -- text, a rounded
   widget's own record, text again -- which a 1-deep memo re-appended on every return.  Four
   128-byte compares against L1-hot records is noise next to the tessellation around it, and a HIT
   costs one compare at any depth -- only a miss pays the full scan.

   4 sits one step past a sharp knee, and everything above it is flat.  Measured on sb_gui's main
   window with the debug overlay up: depth 3 -> 35 records, 4 -> 25, 6 -> 23, 8 -> 21.  Past 4 a
   step buys about two records, so there is nothing to chase by going deeper.

   The residual will not yield to depth at all: dedup is slot-scoped, so each retained-cache window
   slot holds its own copy of an identical style and no walk can reach across that boundary.  A
   frame-global intern region is the lever for those, not this number. */

#define TESS_PRIM_MEMO_DEPTH  4u

/* Drop the open fx page and its memo.  Called wherever prim_dedup_floor rises: past that line the
   page belongs to another slot or to a reservation only a patch may rewrite, and appending a ninth
   row into it would write bytes this pass does not own. */

static inline void
tess_fx_page_reset( void )
{
    s_tess.fx_page = s_tess.fx_page_used = s_tess.fx_memo_row = 0;
}

/* Keep an answer as the memo the next quad tests against, and hand it back.  Every exit from
   tess_prim_local that produced a real index goes through here; the overflow fallback
   deliberately does not, since 0 there is a degraded answer rather than a resolved one. */

static inline u32
tess_prim_answer( u32 local )
{
    s_tess.prim_memo_rec   = s_tess.cur_prim;
    s_tess.prim_memo_base  = s_tess.slot_prim_base;
    s_tess.prim_memo_floor = s_tess.prim_dedup_floor;
    s_tess.prim_memo_valid = true;
    s_tess.cmd_prim_out   = local;
    return s_tess.cur_prim_local = local;
}

static u32
tess_prim_resolve( void )
{
    /* Census before the memo, so the count is of quads that WANT this record rather than of the
       ones the memo happened to miss; the append site below counts the arena entries. */

    PRIM_CENSUS_QUAD( &s_tess.cur_prim );

    /* The last ANSWER, ahead of everything (gui_build_tess_state.c, prim_memo_rec).  One
       compare against bytes this function itself wrote, and it serves the repeat whatever
       produced the answer -- an arena entry, a palette entry, or this memo again.  That is
       the case the two memos below cannot cover between them: both are keyed on where a
       record LANDED, and an answer that landed nowhere leaves neither of them holding it. */

    if ( s_tess.prim_memo_valid
      && s_tess.prim_memo_base  == s_tess.slot_prim_base
      && s_tess.prim_memo_floor == s_tess.prim_dedup_floor
      && memcmp( &s_tess.prim_memo_rec, &s_tess.cur_prim, sizeof( gui_prim_t ) ) == 0 )
        return s_tess.cmd_prim_out = s_tess.cur_prim_local;

    /* Then the answer this COMMAND gave the last time it tessellated (gui_build_tess_state.c,
       cmd_hint).  Confirmed against the entry's own bytes, so an epoch reset, a shifted command
       list or a hash collision all land on a failed compare and the ordinary path below --
       never on a wrong shape.  What it buys over the memo above is the ALTERNATION: a dozen
       commands cycling through a dozen styles keep a dozen live answers instead of evicting
       one. */

    if ( s_tess.cmd_hint != GUI_PAL_NONE )
    {
        const gui_prim_t* e = pal_entry( s_tess.cmd_hint );
        if ( e && memcmp( e, &s_tess.cur_prim, sizeof( gui_prim_t ) ) == 0 )
        {
            pal_cmd_hit();
            return tess_prim_answer( gui_prim_pal( s_tess.cmd_hint ) );
        }
    }

    u32 hi = s_tess.prim_count;
    u32 lo = ( s_tess.prim_dedup_floor > s_tess.slot_prim_base )
             ? s_tess.prim_dedup_floor : s_tess.slot_prim_base;
    u32 n  = ( hi > lo ) ? hi - lo : 0u;
    if ( n > TESS_PRIM_MEMO_DEPTH ) n = TESS_PRIM_MEMO_DEPTH;

    /* Depth 1 next, alone: the record the last APPEND landed, which the answer memo above
       holds only while that append was also the last answer.  An alternation between a
       palette style and an arena one breaks that, and this catches the arena half without
       paying for the lookup. */

    if ( n >= 1u && memcmp( &s_tess.prims[ hi - 1u ], &s_tess.cur_prim,
                            sizeof( gui_prim_t ) ) == 0 )
        return tess_prim_answer( ( hi - 1u ) - s_tess.slot_prim_base );

    /* Then the PALETTE, ahead of the deeper memo walk.  A hit costs this slot nothing at all --
       no arena entry, and the same entry serves every other window drawing the same shape, which
       is the duplication no memo depth can reach (see TESS_PRIM_MEMO_DEPTH above).  A hit returns
       an ABSOLUTE index the flush resolves against pc.pal_base rather than a slot-local one; both
       ride the same field and the shader tells them apart by range (gui.h, GUI_PAL_FIRST). */

    u32 entry = pal_find( &s_tess.cur_prim );
    if ( entry < (u32)GUI_PAL_MAX )
        return tess_prim_answer( gui_prim_pal( entry ) );

    for ( u32 k = 2; k <= n; ++k ) {
        if ( memcmp( &s_tess.prims[ hi - k ], &s_tess.cur_prim, sizeof( gui_prim_t )) == 0 ) {
            return tess_prim_answer( ( hi - k ) - s_tess.slot_prim_base );
        }
    }

    /* Nothing anywhere holds this record.  Before spending an arena entry on it, offer it to
       the palette: a record the frame has drawn before earns a shared entry here and stops
       costing one per slot from now on, which is how a UI layer the engine has never seen
       gets covered (pal_intern, gui_render_intern.c).  Declining is the common answer, and the
       cost of declining is one fold. */

    u32 in = pal_intern( &s_tess.cur_prim );
    if ( in < (u32)GUI_PAL_MAX )
        return tess_prim_answer( gui_prim_pal( in ) );

    if ( hi >= GUI_MAX_PRIMS )
    {
        s_tess.overflow |= TESS_OVF_PRIMS;
        s_tess.prim_dedup_floor = hi;
        tess_fx_page_reset();
        return s_tess.cur_prim_local = 0u;
    }

    PRIM_CENSUS_APPEND( &s_tess.cur_prim );

    s_tess.prims[ hi ] = s_tess.cur_prim;
    s_tess.prim_count++;
    return tess_prim_answer( hi - s_tess.slot_prim_base );
}

/*  The counted wrapper, and the one every caller uses.  Both halves of the slot's style
    accounting are taken here rather than at any single resolution path, because tess_prim_resolve
    has six exits (two memos, the parked command answer, the probe, a fresh intern, the arena
    append) plus an overflow fallback, and a tally spread across those drifts the first time one
    is added.

    One call == one SHAPE that wants a style, which is the denominator the fold rate needs. */

static inline u32
tess_prim_local( void )
{
    ++s_tess.slot_style_refs;

    u32 local = tess_prim_resolve();

    if ( gui_prim_is_pal( local ) )
    {
        u32 e = local - GUI_PAL_FIRST;
        if ( e < (u32)GUI_PAL_MAX )
            s_tess.slot_stored_mask[ e >> 5 ] |= 1u << ( e & 31u );
    }
    return local;
}

/* Resolve the ambient turn / phase / border colour plus this quad's texture rect to an fx record,
   returning its SLOT-LOCAL row index in the prim arena (0 = the quad needs none, which the shader
   reads as identity turn, zero phase, no border and no texture rect).

   Records pack four to a prim-arena slot, so a page costs one gui_prim_t and serves four
   instances.  The memo is one deep and that is enough: the instance lanes are ambient over a
   semantic command, so the quads that share them arrive consecutively -- a framed row, a polyline
   of one direction, a set of glyph quads wanting nothing at all.  A uv rect breaks the memo by
   nature (consecutive icons sample different cells), which is the honest cost of the sprite path
   and still one QUARTER of a record each. */

#define TESS_FX_PER_PAGE   ( GUI_PRIM_ROWS / GUI_FX_ROWS )   /* a prim record is four fx records */

/* True when the ambient instance extras pack to the all-default fx record -- exactly the case
   tess_fx_local( 0, 0 ) answers 0 for.  A caller that only needs the ANSWER must test this
   instead of calling tess_fx_local: that function APPENDS a row when the ambient is non-empty,
   so using it as a predicate leaks an fx row per probe. */
static inline bool
tess_fx_ambient_empty( void )
{
    return gui_xform_pack( s_tess.cur_rot_c, s_tess.cur_rot_s ) == 0u
        && gui_phase_pack( s_tess.cur_phase ) == 0u
        && s_tess.cur_col_border == 0u
        && s_tess.cur_swell == 0.0f;
}

static u32
tess_fx_local( u32 uv0, u32 uv1 )
{
    gui_fx_t fx = {
        .xform      = gui_xform_pack( s_tess.cur_rot_c, s_tess.cur_rot_s ),
        .phase      = gui_phase_pack( s_tess.cur_phase ),
        .col_border = s_tess.cur_col_border,
        .swell      = s_tess.cur_swell,
        .uv0        = uv0,
        .uv1        = uv1,
    };
    if ( fx.xform == 0u && fx.phase == 0u && fx.col_border == 0u && fx.swell == 0.0f
      && fx.uv0 == 0u && fx.uv1 == 0u )
        return 0u;      /* the whole record is the default -- the majority of quads, text included */

    /* The page is a prim-arena record read as rows; the rows are addressed as bytes rather than
       through a second struct pointer, so the two record types never alias one another. */
    u8* page_p = (u8*)&s_tess.prims[ s_tess.slot_prim_base + s_tess.fx_page ];
    if ( s_tess.fx_memo_row
      && memcmp( page_p + ( s_tess.fx_page_used - 1u ) * GUI_FX_BYTES, &fx, sizeof fx ) == 0 )
        return s_tess.fx_memo_row;

    if ( s_tess.fx_page == 0u || s_tess.fx_page_used >= TESS_FX_PER_PAGE )
    {
        /* A fresh page. The floor rises past it for the same reason a volatile boundary raises it:
           the record now holds fx rows, and a style comparing equal to those bytes would be handed
           an index into them. */
        if ( s_tess.prim_count >= GUI_MAX_PRIMS )
        {
            s_tess.overflow |= TESS_OVF_PRIMS;
            return 0u;
        }

        /* Record 0 of a slot may never be an fx page -- row 0 is the index the quad spends on
           "no record".  Two kinds of quad reach here with the slot still empty: a GLYPH, which
           resolves no style at all, and any shape whose style the PALETTE answered, which claims
           no arena record either.  Leave record 0 unwritten in both cases. */
        if ( s_tess.prim_count == s_tess.slot_prim_base )
        {
            if ( s_tess.prim_count + 1u >= GUI_MAX_PRIMS )
            {
                s_tess.overflow |= TESS_OVF_PRIMS;
                return 0u;
            }
            memset( &s_tess.prims[ s_tess.prim_count++ ], 0, sizeof( gui_prim_t ) );
        }

        u32 page = s_tess.prim_count - s_tess.slot_prim_base;
        if ( page * GUI_PRIM_ROWS + ( TESS_FX_PER_PAGE - 1u ) * GUI_FX_ROWS > GUI_QUAD_FX_MASK )
        {
            s_tess.overflow |= TESS_OVF_FX_FIELD;   /* past what the quad's fx field can name */
            return 0u;
        }
        memset( &s_tess.prims[ s_tess.prim_count ], 0, sizeof( gui_prim_t ) );
        s_tess.prim_count++;
        s_tess.fx_page_count++;
        s_tess.prim_dedup_floor = s_tess.prim_count;

        s_tess.fx_page      = page;
        s_tess.fx_page_used = 0;
        page_p              = (u8*)&s_tess.prims[ s_tess.slot_prim_base + page ];
    }

    memcpy( page_p + s_tess.fx_page_used * GUI_FX_BYTES, &fx, sizeof fx );
    s_tess.fx_memo_row = s_tess.fx_page * GUI_PRIM_ROWS + s_tess.fx_page_used * GUI_FX_ROWS;
    s_tess.fx_page_used++;
    s_tess.fx_row_count++;
    return s_tess.fx_memo_row;
}

/* The glyph table's rect for an ID, for the quads that cannot carry the ID itself.  Reading the
   table CPU-side costs the same lookup the vertex stage would have done and gives up only the
   repack stability -- which is exactly the trade a straddling glyph already makes. */
static void
tess_glyph_uv( u32 glyph_id, u32* uv0, u32* uv1 )
{
    if ( glyph_id >= glyph_table_count() )
    {
        *uv0 = *uv1 = 0u;
        return;
    }
    const gui_glyph_uv_t* g = &glyph_table_data()[ glyph_id ];
    *uv0 = g->uv0;
    *uv1 = g->uv1;
}

/*==============================================================================================
    tess_quad_push -- the ONE geometry writer.  Resolves the ambient style (placement and clip
    live on the quad, never the style), appends the quad, and folds one element into the open
    GPU command.

    `rule` is the expansion rule (GUI_QUAD_RULE_*).  Placement is the SHAPE's, by the rule's
    convention (gui.h), in pixels -- quantized to the record's quarter-pixel grid here, which is
    the only place that conversion happens.  uv0/uv1 are packed texcoord corners; a non-zero pair
    goes into the instance record beside the turn, since a texture rect is per-instance and the
    quad has no lane for one.

    `glyph_id` past GUI_GLYPH_ID_NONE asks for the GLYPH tag: the quad names a glyph-table entry
    instead of carrying an atlas rect, and names no prim record at all -- the fragment resolves
    the text atlas from the push block.  That holds while the ambient style says nothing but
    "sample the font atlas".  A glyph under OPS alone (an SDF outline, a gradient) takes the
    GLYPH_STYLED tag -- table ID plus the one prim record the run dedups onto -- and only a
    glyph under a FIELD or with instance extras falls back to the SHAPED tag with the table's
    rect baked in, exactly like a straddling glyph.
==============================================================================================*/

#define GUI_GLYPH_ID_NONE   0xFFFFFFFFu

/* Is a BAND covering worth four quads instead of one?  (gui.h, THE BAND COVERING.)

   The hole this mirrors is the vertex stage's (gui_quad.vs.hlsl, band_local) and must not be a
   second opinion about geometry: band_local clamps its own numbers and tiles its own outer rect
   exactly, so a disagreement here can only mean a shape kept one quad it could have split, never a
   gap or a double-blended seam.  What this decides is the TRADE -- four records and four
   rasterizer setups against the interior they save.

   Restricted to the rounded box under the SKIRT rule, which is the only field whose hole is a
   rectangle band_local can derive.  FRAME is excluded outright: its fill paints the interior it
   would be carving away. */

#define TESS_BAND_MIN_FRAC  0.45f    /* of the covering -- below this the middle is not the cost   */
#define TESS_BAND_MIN_AREA  4096.0f  /* px -- a 64x64 hole, under which four setups beat the fill */

static bool
tess_band_worth_it( f32 qhw, f32 qhh, u32 rule )
{
    const gui_prim_t* p   = &s_tess.cur_prim;
    u32               ops = s_tess.cur_ops;

    /* The repetition ops are excluded for a reason of kind rather than of trade: the region a
       repeated shape leaves at zero coverage is the space BETWEEN its copies, which is not the
       single rectangle band_local knows how to tile.  A band covering there would carve away real
       ink -- and under the polar fold the empty region is the hole in the middle of a ring, which
       is a shape band_local has no way to state at all.  SWELL is excluded with them: the hole is
       measured against the REST boundary, and a swelling boundary moves into it on the clock. */
    if ( rule != GUI_QUAD_RULE_SKIRT || p->field != (u32)GUI_FX_BOX
      || ( ops & ( GUI_OP_FRAME | GUI_OP_REPEAT | GUI_OP_REPEAT_POLAR | GUI_OP_SWELL ) ) )
        return false;

    /* EXACTLY one hole-cutting op.  Each of the three states where its own coverage reaches zero,
       and they measure from different boundaries -- BAND replaces the field INSET would then read,
       so a shape carrying two has a hole neither formula describes.  Rather than reason about which
       is the safe one, such a shape keeps its single quad: overstating a hole is the one failure
       that would clip real ink, and there is no shape in the library that asks for two. */
    u32 hole = ops & ( GUI_OP_CUT | GUI_OP_BAND | GUI_OP_INSET );
    if ( hole == 0u || ( hole & ( hole - 1u ) ) != 0u )
        return false;

    /* Where each op's coverage reaches zero, as a depth inward from the boundary.  CUT's is the
       caster's own outline -- depth 0, moved by the cut vector below. */
    f32 depth = ( hole == GUI_OP_CUT )  ? 0.0f
              : ( hole == GUI_OP_BAND ) ? p->border + p->feather * 0.5f
                                        : p->feather;

    f32 rmax = fmaxf( fmaxf( p->r_tl, p->r_tr ), fmaxf( p->r_br, p->r_bl ) );
    f32 pad  = p->feather * 0.5f + 1.0f;
    f32 in   = depth + 0.29289322f * rmax;

    f32 hix = fmaxf( qhw - in, 0.0f ), hiy = fmaxf( qhh - in, 0.0f );
    f32 hox = qhw + pad,               hoy = qhh + pad;

    /* The cut's offset shifts the hole; what it costs is the part that slides past the outer rect,
       which band_local clamps away.  Charging for it here keeps a shape whose hole barely fits from
       paying four quads for almost nothing. */
    if ( hole == GUI_OP_CUT )
    {
        hix = fmaxf( hix - fabsf( p->cut_dx ), 0.0f );
        hiy = fmaxf( hiy - fabsf( p->cut_dy ), 0.0f );
    }

    /* Both tests, and they are not the same question.  The FRACTION says the middle is where this
       shape's fill actually goes; the AREA says the fill saved is worth four quad records and four
       rasterizer setups instead of one.  A button's outline passes the first and fails the second
       -- its interior is a few hundred px -- and every widget outline on the screen quadrupling its
       quads for that would spend GUI_MAX_QUADS on nothing. */
    f32 hole_area = hix * hiy;
    return hole_area >= TESS_BAND_MIN_AREA
        && hole_area >= TESS_BAND_MIN_FRAC * ( hox * hoy );
}

static void
tess_quad_push( f32 qcx, f32 qcy, f32 qhw, f32 qhh, u32 rule,
                u32 uv0, u32 uv1, u32 tex_idx, u32 abgr, u32 glyph_id )
{
    bool band = tess_band_worth_it( qhw, qhh, rule );

    if ( s_tess.quad_count + ( band ? GUI_QUAD_BAND_COUNT : 1u ) > GUI_MAX_QUADS )
    {
        s_tess.overflow |= TESS_OVF_QUADS;
        return;
    }
    tess_set_tex( tex_idx );
    if ( !tess_ensure_gpu_cmd() )
        return;

    /* BBOX states a covering the vertex stage takes no pad on, so it is the one rule with no slack
       to absorb the quantization below.  A quarter pixel of margin restores it; every other rule
       either grows by the SDF pad or defines the rect it is mapped against, which must not move. */
    if ( rule == GUI_QUAD_RULE_BBOX )
    {
        qhw += 0.25f;
        qhh += 0.25f;
    }

    i16 pcx = gui_quad_pos_pack( qcx ), pcy = gui_quad_pos_pack( qcy );
    u16 phw = gui_quad_ext_pack( qhw ), phh = gui_quad_ext_pack( qhh );

    /* A glyph keeps the tag only while the style would have said nothing: no ops, no field, and an
       fx row inside the narrower field the GLYPH layout has room for.  Anything else -- an SDF
       outline, a pattern, a rotation past the twelfth bit -- takes the table's rect and rejoins
       the SHAPED path, which has a prim record to carry the difference. */
    if ( glyph_id != GUI_GLYPH_ID_NONE
      && s_tess.cur_ops == 0u && s_tess.cur_prim.field == 0u )
    {
        /* The tag carries one bit of texture -- which of the two text atlases -- and the fragment
           reads the slot itself from the push block.  That only means the right thing while a font
           samples one of those two, which is the whole of what font_slot_tex can return. */
        ORB_ASSERT( gui_tex_index( tex_idx ) == ( gui_tex_mode( tex_idx ) == GUI_TEX_SDF
                                                  ? res_sdf_idx() : res_atlas_idx() ) );

        u32 fx = tess_fx_local( 0u, 0u );
        if ( fx <= GUI_QUAD_GFX_MASK )
        {
            s_tess.quads[ s_tess.quad_count++ ] = ( gui_quad_t ){
                .cx   = pcx,
                .cy   = pcy,
                .hw   = phw,
                .hh   = phh,
                .idx  = gui_quad_idx_glyph( s_tess.cur_clip_local, glyph_id,
                                            gui_tex_mode( tex_idx ) == GUI_TEX_SDF, fx ),
                .abgr = abgr,
            };
            goto counted;
        }
        /* The fx row is past what the GLYPH layout can name -- fall through and let the SHAPED
           arm carry it, where the field is a bit wider.  The record written just above is left
           behind: the SHAPED arm asks for one carrying the table's rect as well, which is a
           different record.  One wasted entry at the far end of a slot's fx pages. */
    }
    /* STYLED GLYPH -- ops but no field: the SDF outline, a gradient or a glow on text.  The tag
       keeps everything that makes plain text cheap -- the table ID (repack-stable, no per-char
       fx record) and the push-block texture -- and adds the ONE prim record the whole run
       dedups onto.  Only while the instance extras are empty: the layout spent its fx bits on
       the style, so a rotated styled run falls through to SHAPED, which has room for both.
       The emptiness test must be the side-effect-free one -- tess_fx_local ALLOCATES a row
       when the ambient is non-empty, so probing with it here cost a wasted fx row per glyph
       on every rotated styled run (the SHAPED path below allocates the real one). */
    else if ( glyph_id != GUI_GLYPH_ID_NONE && s_tess.cur_prim.field == 0u
           && tess_fx_ambient_empty() )
    {
        ORB_ASSERT( gui_tex_index( tex_idx ) == ( gui_tex_mode( tex_idx ) == GUI_TEX_SDF
                                                  ? res_sdf_idx() : res_atlas_idx() ) );

        s_tess.cur_prim.tex = s_tess.cur_tex;
        s_tess.cur_prim.ops = s_tess.cur_ops;

        u32 style = tess_prim_local();
        if ( style <= GUI_QUAD_GPRIM_MASK )
        {
            s_tess.quads[ s_tess.quad_count++ ] = ( gui_quad_t ){
                .cx   = pcx,
                .cy   = pcy,
                .hw   = phw,
                .hh   = phh,
                .idx  = gui_quad_idx_glyph_styled( s_tess.cur_clip_local, glyph_id,
                                                   gui_tex_mode( tex_idx ) == GUI_TEX_SDF,
                                                   style ),
                .abgr = abgr,
            };
            goto counted;
        }
    }

    if ( glyph_id != GUI_GLYPH_ID_NONE )
        tess_glyph_uv( glyph_id, &uv0, &uv1 );   /* the fallback bakes what the table holds */

    /* Fold the ambient texture and ops into the style; the clip entry rides the quad below,
       never the style, so a style compares equal across scroll regions. */
    s_tess.cur_prim.tex = s_tess.cur_tex;
    s_tess.cur_prim.ops = s_tess.cur_ops;

    u32 style = tess_prim_local();
    u32 fx    = tess_fx_local( uv0, uv1 );

    /* The BAND covering emits the same quad four times over, differing in nothing but which strip
       of the frame each expands to.  One placement, one style, one clip, one fx record -- so the
       fragment resolves every one of them exactly as it resolved the single quad they replace. */
    for ( u32 b = 0; b < ( band ? GUI_QUAD_BAND_COUNT : 1u ); ++b )
        s_tess.quads[ s_tess.quad_count++ ] = ( gui_quad_t ){
            .cx    = pcx,
            .cy    = pcy,
            .hw    = phw,
            .hh    = phh,
            .abgr  = abgr,
            .idx   = band ? gui_quad_idx_band( b, s_tess.cur_clip_local, style, fx )
                          : gui_quad_idx( rule, s_tess.cur_clip_local, style, fx ),
        };

counted:

    /* elem_count counts QUADS under this backend; the flush multiplies by six at the draw. */
    s_tess.gpu_cmds[ s_tess.cmd_count - 1 ].elem_count += band ? GUI_QUAD_BAND_COUNT : 1;

    if ( s_tess.cur_is_text && !s_volatile_patching )
        s_tess.slot_text_quads++;
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
                    gui_uv_pack( u0, v0 ), gui_uv_pack( u1, v1 ), tex_idx, abgr,
                    GUI_GLYPH_ID_NONE );
}

/* Tessellate one whole glyph: same placement as tess_rect_filled, but the quad names a glyph-table
   entry instead of carrying an atlas rect.  The rect is resolved in the vertex stage, so this quad
   survives an atlas repack that would leave a baked uv sampling another tenant's pixels.
   Only for a glyph drawn ENTIRELY -- a run cut to its window narrows one glyph's uv span, which is
   per-instance and has no table entry, so that case stays on tess_rect_filled. */
static void
tess_rect_glyph( f32 x, f32 y, f32 w, f32 h, u32 glyph_id, u32 tex_idx, u32 abgr )
{
    x = tess_snap_px( x );
    y = tess_snap_px( y );
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0u, 0u, tex_idx, abgr, glyph_id );
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
    s_tess.cur_prim.grad_x  = horizontal ? 1.0f : 0.0f;
    s_tess.cur_prim.grad_y  = horizontal ? 0.0f : 1.0f;
    s_tess.cur_rot_c = 1.0f;
    tess_quad_push( x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, GUI_QUAD_RULE_EXACT,
                    0, 0, res_atlas_idx(), col_a, GUI_GLYPH_ID_NONE );
}


// clang-format on
