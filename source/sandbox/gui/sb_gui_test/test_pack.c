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
    /* Three 16-byte rows, no tail padding.  GUI_QUAD_ROWS is what the vertex stage multiplies
       by when it pulls the record through the bindless float4 buffer. */
    test_equal( 3u,  GUI_QUAD_ROWS );
    test_equal( 48u, (u32)GUI_QUAD_BYTES );
    test_equal( 48u, (u32)sizeof( gui_quad_t ) );

    /* Row starts. */
    test_equal(  0u, (u32)offsetof( gui_quad_t, cx   ) );
    test_equal( 16u, (u32)offsetof( gui_quad_t, uv0  ) );
    test_equal( 32u, (u32)offsetof( gui_quad_t, clip ) );

    /* Within-row order, since a row is read as one vec4 and its components are positional. */
    test_equal(  4u, (u32)offsetof( gui_quad_t, cy    ) );
    test_equal(  8u, (u32)offsetof( gui_quad_t, hw    ) );
    test_equal( 12u, (u32)offsetof( gui_quad_t, hh    ) );
    test_equal( 20u, (u32)offsetof( gui_quad_t, uv1   ) );
    test_equal( 24u, (u32)offsetof( gui_quad_t, abgr  ) );
    test_equal( 28u, (u32)offsetof( gui_quad_t, style ) );
    test_equal( 36u, (u32)offsetof( gui_quad_t, flags ) );
    test_equal( 40u, (u32)offsetof( gui_quad_t, xform ) );
    test_equal( 44u, (u32)offsetof( gui_quad_t, col_border ) );

    /* The expansion rules share the low 2 bits of `flags` and the animation phase takes the high
       half -- the layout the quad vertex stage decodes with literal masks. */
    test_equal( 0u, GUI_QUAD_RULE_EXACT );
    test_equal( 1u, GUI_QUAD_RULE_SKIRT );
    test_equal( 2u, GUI_QUAD_RULE_CAPSULE );
    test_equal( 3u, GUI_QUAD_RULE_BBOX );
    test_equal( 16u,         GUI_QUAD_PHASE_SHIFT );
    test_equal( 0xFFFF0000u, GUI_QUAD_PHASE_MASK );

    /* The glyph-ID flag sits above the rule lane and below the phase, so a glyph quad's rule and
       phase decode exactly as any other quad's. */
    test_equal( 4u, GUI_QUAD_F_GLYPH );
    test_equal( 0u, GUI_QUAD_F_GLYPH & 3u );
    test_equal( 0u, GUI_QUAD_F_GLYPH & GUI_QUAD_PHASE_MASK );

    /* Two glyph table entries per float4 row is what the vertex stage's ID -> row split assumes,
       and the region must hold a whole number of rows. */
    test_equal(  8u, (u32)sizeof( gui_glyph_uv_t ) );
    test_equal(  0u, (u32)offsetof( gui_glyph_uv_t, uv0 ) );
    test_equal(  4u, (u32)offsetof( gui_glyph_uv_t, uv1 ) );
    test_equal(  0u, GUI_GLYPH_TABLE_MAX & 1u );
    test_equal( 8192u, GUI_GLYPH_TABLE_MAX );

    /* The rule lane and the phase lane cannot reach each other. */
    test_equal( 0u, GUI_QUAD_RULE_BBOX & GUI_QUAD_PHASE_MASK );
}

/* The two per-instance packings the quad carries, and the one property each rests on: identity
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

    /* Phase lands in the high half and nowhere else. */
    test_equal( 0u, gui_phase_pack( 0.0f ) );
    test_equal( 0u, gui_phase_pack( 0.5f ) & 0xFFFFu );
    test_equal( 32768u, gui_phase_pack( 0.5f ) >> GUI_QUAD_PHASE_SHIFT );

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
    f32 phase = (f32)( gui_phase_pack( gui_phase_anchor( t0, dur ) ) >> GUI_QUAD_PHASE_SHIFT )
              / 65535.0f;

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
