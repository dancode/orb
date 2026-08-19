/*==============================================================================================

    sandbox/gui/sb_gui_test/test_pack.c -- the quad record, and the style record it points at.

    The UV packing runs on every textured quad the gui emits and fails silently: a swapped half
    samples the wrong row of the atlas, which is loud, but a swap in ONE emit path is not.

    The records have no packing to get wrong -- what they have is a LAYOUT the shaders index by
    literal row, so a field inserted or a type widened would slide a corner radius into a
    rotation with no compile error anywhere.  That is the other thing here.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    gui_uv_pack -- two unorm16 over [0,1], u in the LOW half.
==============================================================================================*/

static void
test_uv_pack( void )
{
    test_equal( 0x00000000u, gui_uv_pack( 0.0f, 0.0f ) );
    test_equal( 0xFFFFFFFFu, gui_uv_pack( 1.0f, 1.0f ) );

    /* Half order: u low, v high.  A swap samples the atlas transposed -- every glyph wrong,
       which is loud, but a swap in ONE emit path is not. */
    test_equal( 0x0000FFFFu, gui_uv_pack( 1.0f, 0.0f ) );
    test_equal( 0xFFFF0000u, gui_uv_pack( 0.0f, 1.0f ) );

    test_equal( 32768u, gui_uv_pack( 0.5f, 0.0f ) & 0xFFFFu );
    test_equal( 16384u, gui_uv_pack( 0.25f, 0.0f ) & 0xFFFFu );
    test_equal( 49151u, gui_uv_pack( 0.75f, 0.0f ) & 0xFFFFu );

    /* Both halves independent. */
    u32 w = gui_uv_pack( 0.25f, 0.75f );
    test_equal( 16384u, w & 0xFFFFu );
    test_equal( 49151u, w >> 16 );
}

/*==============================================================================================
    The style record -- the layout the fragment indexes as vec4[].

    Nothing here checks a VALUE; the record has no packing to get wrong, which is the point of it.
    What can still go wrong is its SHAPE: the shader spells the row stride as a literal, so a
    field inserted or a type widened would slide every row past the first and shift a corner
    radius into a rotation with no compile error anywhere.
==============================================================================================*/

static void
test_prim_layout( void )
{
    /* Eight 16-byte rows, no tail padding.  GUI_PRIM_ROWS is what the shaders multiply by. */
    test_equal( 8u,   GUI_PRIM_ROWS );
    test_equal( 128u, (u32)GUI_PRIM_BYTES );
    test_equal( 128u, (u32)sizeof( gui_prim_t ) );

    /* Row starts.  The hot word leads on purpose: a glyph or a flat fill reads row 0 and stops.
       No placement row and no clip lane -- both ride the quad record (gui_quad_t). */
    test_equal(  0u, (u32)offsetof( gui_prim_t, field   ) );
    test_equal( 16u, (u32)offsetof( gui_prim_t, r_tl    ) );
    test_equal( 32u, (u32)offsetof( gui_prim_t, feather ) );
    test_equal( 48u, (u32)offsetof( gui_prim_t, param_a ) );
    test_equal( 64u, (u32)offsetof( gui_prim_t, grad_x  ) );
    test_equal( 80u, (u32)offsetof( gui_prim_t, anim_rate   ) );
    test_equal( 84u, (u32)offsetof( gui_prim_t, anim_curve  ) );
    test_equal( 88u, (u32)offsetof( gui_prim_t, anim_param  ) );
    test_equal( 96u, (u32)offsetof( gui_prim_t, dash_period ) );
    test_equal( 112u, (u32)offsetof( gui_prim_t, pat_cell ) );

    /* Within-row order, since a row is read as one vec4 and its components are positional. */
    test_equal(  4u, (u32)offsetof( gui_prim_t, ops     ) );
    test_equal(  8u, (u32)offsetof( gui_prim_t, tex     ) );
    test_equal( 20u, (u32)offsetof( gui_prim_t, r_tr    ) );
    test_equal( 24u, (u32)offsetof( gui_prim_t, r_br    ) );
    test_equal( 28u, (u32)offsetof( gui_prim_t, r_bl    ) );
    test_equal( 36u, (u32)offsetof( gui_prim_t, border     ) );
    test_equal( 40u, (u32)offsetof( gui_prim_t, corner_pow ) );
    test_equal( 52u, (u32)offsetof( gui_prim_t, param_b ) );
    test_equal( 56u, (u32)offsetof( gui_prim_t, param_c ) );
    test_equal( 60u, (u32)offsetof( gui_prim_t, col_b   ) );
    test_equal( 68u, (u32)offsetof( gui_prim_t, grad_y  ) );
    test_equal( 72u, (u32)offsetof( gui_prim_t, cut_dx  ) );
    test_equal( 76u, (u32)offsetof( gui_prim_t, cut_dy  ) );
    test_equal( 92u, (u32)offsetof( gui_prim_t, grad_mid    ) );
    test_equal( 100u, (u32)offsetof( gui_prim_t, dash_duty   ) );
    test_equal( 104u, (u32)offsetof( gui_prim_t, dash_scroll ) );
    test_equal( 44u,  (u32)offsetof( gui_prim_t, pat_angle ) );
    test_equal( 116u, (u32)offsetof( gui_prim_t, pat_size  ) );
    test_equal( 120u, (u32)offsetof( gui_prim_t, pat_phase ) );
    test_equal( 124u, (u32)offsetof( gui_prim_t, pat_col   ) );
}

static void
test_quad_layout( void )
{
    /* ONE 16-byte row, no tail padding.  GUI_QUAD_ROWS is what the vertex stage multiplies by
       when it pulls the record through the bindless float4 buffer. */
    test_equal( 1u,  GUI_QUAD_ROWS );
    test_equal( 16u, (u32)GUI_QUAD_BYTES );
    test_equal( 16u, (u32)sizeof( gui_quad_t ) );

    /* Within-row order, since the row is read as one vec4 and its components are positional: the
       centre pair shares lane x and the half-extent pair lane y, low half first. */
    test_equal(  0u, (u32)offsetof( gui_quad_t, cx   ) );
    test_equal(  2u, (u32)offsetof( gui_quad_t, cy   ) );
    test_equal(  4u, (u32)offsetof( gui_quad_t, hw   ) );
    test_equal(  6u, (u32)offsetof( gui_quad_t, hh   ) );
    test_equal(  8u, (u32)offsetof( gui_quad_t, abgr ) );
    test_equal( 12u, (u32)offsetof( gui_quad_t, idx  ) );

    /* Placement is quarter-pixel fixed point: a snapped fill and a whole glyph land on the grid
       exactly, and out-of-range coordinates clamp instead of wrapping back into view. */
    test_equal( (u32)(i32)   4, (u32)(i32)gui_quad_pos_pack(  1.0f  ) );
    test_equal( (u32)(i32)  -6, (u32)(i32)gui_quad_pos_pack( -1.5f  ) );
    test_equal( (u32)(i32)   1, (u32)(i32)gui_quad_pos_pack(  0.25f ) );
    test_equal( (u32)(i32)32767, (u32)(i32)gui_quad_pos_pack(  1.0e6f ) );
    test_equal( (u32)(i32)-32768, (u32)(i32)gui_quad_pos_pack( -1.0e6f ) );
    test_equal(  10u, gui_quad_ext_pack( 2.5f  ) );
    test_equal(   0u, gui_quad_ext_pack( -3.0f ) );
    test_equal( 65535u, gui_quad_ext_pack( 1.0e6f ) );

    /* The instance-extras record is exactly two rows: the quad names it by ROW index, so any
       other size would make that index mean nothing, and four of them tile one style record. */
    test_equal(  2u, GUI_FX_ROWS );
    test_equal( 32u, (u32)sizeof( gui_fx_t ) );
    test_equal(  0u, (u32)offsetof( gui_fx_t, xform      ) );
    test_equal(  4u, (u32)offsetof( gui_fx_t, phase      ) );
    test_equal(  8u, (u32)offsetof( gui_fx_t, col_border ) );
    test_equal( 16u, (u32)offsetof( gui_fx_t, uv0        ) );
    test_equal( 20u, (u32)offsetof( gui_fx_t, uv1        ) );
    test_equal(  0u, GUI_PRIM_ROWS % GUI_FX_ROWS );

    test_equal( 0u, GUI_QUAD_RULE_EXACT );
    test_equal( 1u, GUI_QUAD_RULE_SKIRT );
    test_equal( 2u, GUI_QUAD_RULE_CAPSULE );
    test_equal( 3u, GUI_QUAD_RULE_BBOX );

    /* EVERY arm of the tagged union is exactly full: its fields tile all 32 bits with no gap
       and no overlap, so widening any one of them is a re-plan of the union rather than an edit.

       A field's WIDTH and the pool it names are two different numbers.  Clip and the glyph table
       sit AT their ceilings (16 clips per window slab, 8192 glyph-table entries) and are checked
       for equality below; the style and fx fields carry slack over the caps currently shipping, so
       those are checked as the bound they are.  gui.h states the same pair of invariants as static
       asserts beside GUI_MAX_PRIMS. */
    test_equal( 0xFFFFFFFFu, ( GUI_QUAD_CLIP_MASK  << GUI_QUAD_CLIP_SHIFT  )
                           | ( 3u                  << GUI_QUAD_RULE_SHIFT  )
                           | ( GUI_QUAD_STYLE_MASK << GUI_QUAD_STYLE_SHIFT )
                           | ( GUI_QUAD_FX_MASK    << GUI_QUAD_FX_SHIFT    )
                           | ( GUI_QUAD_TAG_MASK   << GUI_QUAD_TAG_SHIFT   ) );
    test_equal( 0xFFFFFFFFu, ( GUI_QUAD_CLIP_MASK  << GUI_QUAD_CLIP_SHIFT  )
                           | GUI_QUAD_SDF_BIT
                           | ( GUI_QUAD_GLYPH_MASK << GUI_QUAD_GLYPH_SHIFT )
                           | ( GUI_QUAD_GFX_MASK   << GUI_QUAD_GFX_SHIFT   )
                           | ( GUI_QUAD_TAG_MASK   << GUI_QUAD_TAG_SHIFT   ) );
    /* BAND is SHAPED's layout with the rule field re-read, so it tiles by the same arithmetic and
       the band index must fit exactly the two bits the rule vacated. */
    test_equal( 0xFFFFFFFFu, ( GUI_QUAD_CLIP_MASK  << GUI_QUAD_CLIP_SHIFT  )
                           | ( 3u                  << GUI_QUAD_BAND_SHIFT  )
                           | ( GUI_QUAD_STYLE_MASK << GUI_QUAD_STYLE_SHIFT )
                           | ( GUI_QUAD_FX_MASK    << GUI_QUAD_FX_SHIFT    )
                           | ( GUI_QUAD_TAG_MASK   << GUI_QUAD_TAG_SHIFT   ) );
    test_equal( GUI_QUAD_RULE_SHIFT, GUI_QUAD_BAND_SHIFT );
    test_equal( 4u, GUI_QUAD_BAND_COUNT );

    test_equal( 15u, GUI_QUAD_CLIP_MASK );   /* GUI_WIN_CLIP_MAX - 1, a backend-private cap */
    test_equal( GUI_GLYPH_TABLE_MAX - 1u, GUI_QUAD_GLYPH_MASK );
    test_true ( (u32)GUI_MAX_PRIMS - 1u <= GUI_QUAD_STYLE_MASK );

    /* The style field's two halves must not meet: below GUI_PAL_FIRST an index is slot-local
       against the arena, at or above it a shared palette entry, and the shader picks the base off
       the index alone (gui_common.hlsli, style_row).  An overlap would silently resolve a window's
       record against the palette block. */
    test_true ( (u32)GUI_MAX_PRIMS <= GUI_PAL_FIRST );
    test_true ( GUI_PAL_FIRST + GUI_PAL_MAX - 1u <= GUI_QUAD_STYLE_MASK );
    test_true ( gui_style_is_pal( gui_style_pal( 0u ) ) );
    test_true ( gui_style_is_pal( gui_style_pal( GUI_PAL_MAX - 1u ) ) );
    test_true ( !gui_style_is_pal( (u32)GUI_MAX_PRIMS - 1u ) );

    /* The fx field names a row in the style arena, and the tag took the two bits that would have
       let it reach every one: 8191 rows is 1024 fx pages per window slot, past which the
       tessellator flags an overflow rather than wrapping onto another slot's records. */
    test_equal( 8191u, GUI_QUAD_FX_MASK );
    test_equal( 4095u, GUI_QUAD_GFX_MASK );

    /* Clip decodes the same under both tags -- the one field the shader reads without first
       asking which arm it is looking at. */
    test_equal( 0u, GUI_QUAD_CLIP_SHIFT );

    /* Every field round-trips through its packer, and none reaches into another. */
    u32 idx = gui_quad_idx( GUI_QUAD_RULE_CAPSULE, 9u, 1337u, 4321u );
    test_equal( GUI_QUAD_TAG_SHAPED,   gui_quad_tag  ( idx ) );
    test_equal( GUI_QUAD_RULE_CAPSULE, gui_quad_rule ( idx ) );
    test_equal(  9u,    gui_quad_clip ( idx ) );
    test_equal( 1337u,  gui_quad_style( idx ) );
    test_equal( 4321u,  gui_quad_fx   ( idx ) );

    /* The four band quads of one shape differ in the band field and in NOTHING else -- that is what
       lets them resolve the same style, clip and fx record, and so the same field the single quad
       they replace resolved. */
    for ( u32 b = 0; b < GUI_QUAD_BAND_COUNT; ++b )
    {
        u32 bidx = gui_quad_idx_band( b, 9u, 1337u, 4321u );
        test_equal( GUI_QUAD_TAG_BAND, gui_quad_tag  ( bidx ) );
        test_equal( b,                 gui_quad_band ( bidx ) );
        test_equal(  9u,               gui_quad_clip ( bidx ) );
        test_equal( 1337u,             gui_quad_style( bidx ) );
        test_equal( 4321u,             gui_quad_fx   ( bidx ) );
        test_equal( idx & ~( 3u << GUI_QUAD_BAND_SHIFT | GUI_QUAD_TAG_MASK << GUI_QUAD_TAG_SHIFT ),
                    bidx & ~( 3u << GUI_QUAD_BAND_SHIFT | GUI_QUAD_TAG_MASK << GUI_QUAD_TAG_SHIFT ) );
    }

    u32 gidx = gui_quad_idx_glyph( 11u, 7000u, true, 3000u );
    test_equal( GUI_QUAD_TAG_GLYPH, gui_quad_tag  ( gidx ) );
    test_equal( 11u,                gui_quad_clip ( gidx ) );
    test_equal( 7000u,              gui_quad_glyph( gidx ) );
    test_equal( 3000u, ( gidx >> GUI_QUAD_GFX_SHIFT ) & GUI_QUAD_GFX_MASK );
    test_equal( GUI_QUAD_SDF_BIT, gidx & GUI_QUAD_SDF_BIT );
    test_equal( 0u, gui_quad_idx_glyph( 11u, 7000u, false, 3000u ) & GUI_QUAD_SDF_BIT );

    /* Two glyph table entries per float4 row is what the vertex stage's ID -> row split assumes,
       and the region must hold a whole number of rows. */
    test_equal(  8u, (u32)sizeof( gui_glyph_uv_t ) );
    test_equal(  0u, (u32)offsetof( gui_glyph_uv_t, uv0 ) );
    test_equal(  4u, (u32)offsetof( gui_glyph_uv_t, uv1 ) );
    test_equal(  0u, GUI_GLYPH_TABLE_MAX & 1u );
    test_equal( 8192u, GUI_GLYPH_TABLE_MAX );
}

/* The two per-instance packings the fx record carries, and the one property each rests on: identity
   states itself as ZERO (so an unrotated shape leaves the lane alone, like every other record
   lane here), and a phase wraps rather than clamping. */
static void
test_quad_instance_pack( void )
{
    /* Identity is the reserved zero, and nothing else packs to it. */
    test_equal( 0u, gui_xform_pack( 1.0f, 0.0f ) );
    test_true( gui_xform_pack( -1.0f, 0.0f ) != 0u );
    test_true( gui_xform_pack(  0.0f, 1.0f ) != 0u );

    /* A quarter turn round-trips through the unorm16 pair: (0, 1) sits at (half, full). */
    u32 q = gui_xform_pack( 0.0f, 1.0f );
    test_equal( 32768u, q & 0xFFFFu );
    test_equal( 65535u, q >> 16 );

    /* Phase is a bare unorm16 over one cycle -- it owns a whole lane, so nothing is shifted. */
    test_equal( 0u, gui_phase_pack( 0.0f ) );
    test_equal( 32768u, gui_phase_pack( 0.5f ) );

    /* Whole cycles wrap away -- 1.25 and 0.25 are the same point in the wave, and a NEGATIVE
       phase is the same point measured backwards rather than a clamp to zero. */
    test_equal( gui_phase_pack( 0.25f ), gui_phase_pack(  1.25f ) );
    test_equal( gui_phase_pack( 0.75f ), gui_phase_pack( -0.25f ) );
}

/* The one-shot rests entirely on this: choosing the phase as -t0/duration puts a cycle boundary
   exactly on t0, so the periodic clock the fragment already runs BECOMES the transition's
   progress.  The property is checked the way the fragment computes it -- through the quantized
   unorm16 the quad actually carries, not the exact float -- since that quantization is the only
   thing that could make the anchor land off the event. */
static void
test_phase_anchor( void )
{
    const f32 t0  = 12.5f;
    const f32 dur = 0.4f;

    /* The phase as the fragment receives it: packed to unorm16, read back over 65535. */
    f32 phase = (f32)gui_phase_pack( gui_phase_anchor( t0, dur ) ) / 65535.0f;

    /* frac( time/dur + phase ) is the progress, at the start, through, and just short of the end. */
    const f32 at[] = { 0.0f, 0.25f, 0.5f, 0.75f, 0.999f };
    for ( u32 i = 0; i < ARRAY_COUNT( at ); ++i )
    {
        f32 x   = ( t0 + at[ i ] * dur ) / dur + phase;
        f32 phi = x - (f32)(i32)x;
        if ( phi < 0.0f ) phi += 1.0f;
        f32 err = phi - at[ i ];
        test_true( ( err < 0.0f ? -err : err ) < 0.001f );
    }

    /* A zero or negative duration is the "no animation" request, and states itself as no offset
       rather than as a division. */
    test_true( gui_phase_anchor( 12.5f,  0.0f ) == 0.0f );
    test_true( gui_phase_anchor( 12.5f, -1.0f ) == 0.0f );
}

/* The op bits are single bits and DISJOINT, which is the whole claim the op word makes: any op
   composes with any field and with any other op.  A shared bit would silently turn a neighbour on
   -- the failure the tex word's op band was carved out to avoid, restated where it now lives. */
static void
test_prim_ops( void )
{
    const u32 ops[] = { GUI_OP_BAND, GUI_OP_CUT, GUI_OP_INSET,
                        GUI_OP_PULSE, GUI_OP_STRIPES, GUI_OP_SELF,
                        GUI_OP_GRAD, GUI_OP_GRAD_RADIAL, GUI_OP_GRAD_CONIC,
                        GUI_OP_SPIN, GUI_OP_DASH, GUI_OP_DITHER, GUI_OP_FRAME,
                        GUI_OP_TILE_U, GUI_OP_TEXT_EDGE, GUI_OP_CHECKER, GUI_OP_GRID };

    u32 seen = 0u;
    for ( u32 i = 0; i < ARRAY_COUNT( ops ); ++i )
    {
        test_true( ops[ i ] != 0u );
        test_equal( 0u, ops[ i ] & ( ops[ i ] - 1u ) );   /* exactly one bit set */
        test_equal( 0u, seen & ops[ i ] );                /* and not one already spent */
        seen |= ops[ i ];
    }
    test_equal( 0x1FFFFu, seen );
}

/*============================================================================================*/
