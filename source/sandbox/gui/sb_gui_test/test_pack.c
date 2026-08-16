/*==============================================================================================

    sandbox/gui/sb_gui_test/test_pack.c -- the vertex, and the record it points at.

    The packings run on EVERY vertex the gui emits and fail silently: a wrong half-float misplaces
    an SDF edge by a fraction of a pixel, a swapped UV half samples the wrong row of the atlas.
    The record has no packing to get wrong -- what it has is a LAYOUT the shaders index by literal
    row, which is the other thing here.

    gui_f16_from_f32 carries a comment saying it was "verified against Python's binary16 packing
    across the range and every boundary" -- once, by hand, at the time it was written.  This is
    that verification made repeatable, including the round-half-UP tie behaviour that
    deliberately differs from the hardware convention.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    An INDEPENDENT binary16 decoder -- deliberately not the inverse of the implementation under
    test, so a round-trip check cannot pass by sharing a bug with it.
==============================================================================================*/

static f32
ref_f16_to_f32( u16 h )
{
    u32 sign = ( h >> 15 ) & 1u;
    i32 exp  = (i32)( ( h >> 10 ) & 0x1Fu );
    u32 man  = h & 0x3FFu;
    f32 v;

    if ( exp == 0 )
    {
        v = ( (f32)man / 1024.0f ) / 16384.0f;          /* subnormal: man/1024 * 2^-14 */
    }
    else
    {
        f32 scale = 1.0f;
        i32 e     = exp - 15;
        for ( i32 i = 0; i <  e; ++i ) scale *= 2.0f;
        for ( i32 i = 0; i < -e; ++i ) scale *= 0.5f;
        v = ( 1.0f + (f32)man / 1024.0f ) * scale;
    }

    return sign ? -v : v;
}

static f32
pk_inf( void )  { union { u32 u; f32 f; } n; n.u = 0x7F800000u; return n.f; }

static f32
pk_nan( void )  { union { u32 u; f32 f; } n; n.u = 0x7FC00000u; return n.f; }

/*==============================================================================================
    gui_f16_from_f32
==============================================================================================*/

static void
test_f16_exact( void )
{
    /* Canonical bit patterns. */
    test_equal( 0x0000u, gui_f16_from_f32(  0.0f ) );
    test_equal( 0x3C00u, gui_f16_from_f32(  1.0f ) );
    test_equal( 0xBC00u, gui_f16_from_f32( -1.0f ) );
    test_equal( 0x3800u, gui_f16_from_f32(  0.5f ) );
    test_equal( 0x4000u, gui_f16_from_f32(  2.0f ) );
    test_equal( 0xC000u, gui_f16_from_f32( -2.0f ) );
    test_equal( 0x5640u, gui_f16_from_f32( 100.0f ) );
    test_equal( 0x7BFFu, gui_f16_from_f32( 65504.0f ) );    /* largest finite half */
}

static void
test_f16_saturation( void )
{
    /* Documented: an exponent past the half range clamps to the largest finite value with the
       sign kept -- and infinities and NaN land in the same place.  A gui that returned a half
       INF here would put an inf into a vertex attribute and the whole quad would vanish. */
    test_equal( 0x7BFFu, gui_f16_from_f32(  1.0e9f ) );
    test_equal( 0xFBFFu, gui_f16_from_f32( -1.0e9f ) );
    test_equal( 0x7BFFu, gui_f16_from_f32( pk_inf() ) );
    test_equal( 0xFBFFu, gui_f16_from_f32( -pk_inf() ) );
    test_equal( 0x7BFFu, gui_f16_from_f32( pk_nan() ) );

    /* Below the subnormal range flushes to a signed zero. */
    test_equal( 0x0000u, gui_f16_from_f32(  1.0e-10f ) );
    test_equal( 0x8000u, gui_f16_from_f32( -1.0e-10f ) );
}

static void
test_f16_round_half_up( void )
{
    /* The documented deviation from hardware: round-half-UP, so an exact tie goes away from
       zero where the struct/hardware convention would go to even.  2049 -> 2050 here, 2048
       there.  Pinning it matters because the two are indistinguishable except on ties. */
    test_equal( 0x6801u, gui_f16_from_f32( 2049.0f ) );
    test_true( ref_f16_to_f32( gui_f16_from_f32( 2049.0f ) ) == 2050.0f );
}

static void
test_f16_round_trip( void )
{
    /* The live range: effect coordinates are pixel magnitudes in the hundreds.  Half has ~3
       decimal digits there, so require the round-trip within one part in 1000. */
    static const f32 vals[] = { 0.25f, 1.0f, 7.5f, 16.0f, 100.0f, 250.5f, 511.875f, 1024.0f,
                                -0.25f, -100.0f, -511.875f };

    for ( u32 i = 0; i < ARRAY_COUNT( vals ); ++i )
    {
        f32 v   = vals[ i ];
        f32 got = ref_f16_to_f32( gui_f16_from_f32( v ) );
        f32 err = ( got - v ) / v;
        if ( err < 0.0f ) err = -err;
        test_true( err < 0.001f );
    }

    /* Exactly representable values must survive EXACTLY -- powers of two and their halves are
       what a tessellator's skirt offsets actually are. */
    test_true( ref_f16_to_f32( gui_f16_from_f32( 0.5f  ) ) == 0.5f  );
    test_true( ref_f16_to_f32( gui_f16_from_f32( 16.0f ) ) == 16.0f );
    test_true( ref_f16_to_f32( gui_f16_from_f32( 256.0f ) ) == 256.0f );
}

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
    gui_fxc_pack -- the per-corner effect coordinate, two halves.
==============================================================================================*/

static void
test_fxc_pack( void )
{
    test_equal( 0x00000000u, gui_fxc_pack( 0.0f, 0.0f ) );

    /* ex in the low half, ey in the high half -- the same convention as the UV word. */
    test_equal( (u32)gui_f16_from_f32( 1.0f ), gui_fxc_pack( 1.0f, 0.0f ) );
    test_equal( (u32)gui_f16_from_f32( 1.0f ) << 16, gui_fxc_pack( 0.0f, 1.0f ) );

    u32 w = gui_fxc_pack( -8.0f, 100.0f );
    test_equal( (u32)gui_f16_from_f32( -8.0f  ), w & 0xFFFFu );
    test_equal( (u32)gui_f16_from_f32( 100.0f ), w >> 16 );

    /* Signed coordinates matter: ARC/PIE carry the RAW signed offset because the sign is what
       carries the angle.  A packer that dropped the sign would collapse a sector to a quadrant. */
    test_not_equal( gui_fxc_pack( 8.0f, 0.0f ), gui_fxc_pack( -8.0f, 0.0f ) );
}

/*==============================================================================================
    The vertex constructors -- the ONLY supported way to build a gui_draw_vert_t.
==============================================================================================*/

static void
test_vert_ctors( void )
{
    gui_draw_vert_t v = gui_vert( 10.0f, 20.0f, 0.0f, 1.0f, 0xFF204060u );

    test_true( v.x == 10.0f );
    test_true( v.y == 20.0f );
    test_equal( 0xFF204060u, v.abgr );
    test_equal( gui_uv_pack( 0.0f, 1.0f ), v.uv );

    /* The clear is the contract: a plain vertex must name record 0 and carry no effect
       coordinate.  A constructor that left ambient state in place would make every square fill
       inherit whatever shape was drawn before it. */
    test_equal( 0u, v.fxc );

    /* The fxc variant differs in exactly one field. */
    gui_draw_vert_t f = gui_vert_fxc( 10.0f, 20.0f, 0.0f, 1.0f, 0xFF204060u, -4.0f, 6.0f );

    test_true( f.x == v.x && f.y == v.y );
    test_equal( v.uv,   f.uv   );
    test_equal( v.abgr, f.abgr );
    test_equal( gui_fxc_pack( -4.0f, 6.0f ), f.fxc );

    /* The record index clears too, and for the same reason: the tessellator's commit point stamps
       it, so a constructor that left one behind would point a fresh primitive at whatever shape
       happened to be built before it. */
    test_equal( 0u, v.prim );
    test_equal( 0u, f.prim );
}

/*==============================================================================================
    The primitive record -- the layout the fragment indexes as vec4[].

    Nothing here checks a VALUE; the record has no packing to get wrong, which is the point of it.
    What can still go wrong is its SHAPE: the shader spells the row stride as a literal, so a
    field inserted or a type widened would slide every row past the first and shift a corner
    radius into a rotation with no compile error anywhere.
==============================================================================================*/

static void
test_prim_layout( void )
{
    /* Five 16-byte rows, no tail padding.  GUI_PRIM_ROWS is what the shaders multiply by. */
    test_equal( 5u,  GUI_PRIM_ROWS );
    test_equal( 80u, (u32)GUI_PRIM_BYTES );
    test_equal( 80u, (u32)sizeof( gui_prim_t ) );

    /* Row starts.  The hot word leads on purpose: a glyph or a flat fill reads row 0 and stops. */
    test_equal(  0u, (u32)offsetof( gui_prim_t, field   ) );
    test_equal( 16u, (u32)offsetof( gui_prim_t, cx      ) );
    test_equal( 32u, (u32)offsetof( gui_prim_t, r_tl    ) );
    test_equal( 48u, (u32)offsetof( gui_prim_t, feather ) );
    test_equal( 64u, (u32)offsetof( gui_prim_t, param_a ) );

    /* Within-row order, since a row is read as one vec4 and its components are positional. */
    test_equal(  4u, (u32)offsetof( gui_prim_t, ops     ) );
    test_equal(  8u, (u32)offsetof( gui_prim_t, tex     ) );
    test_equal( 12u, (u32)offsetof( gui_prim_t, clip    ) );
    test_equal( 20u, (u32)offsetof( gui_prim_t, cy      ) );
    test_equal( 24u, (u32)offsetof( gui_prim_t, hw      ) );
    test_equal( 28u, (u32)offsetof( gui_prim_t, hh      ) );
    test_equal( 36u, (u32)offsetof( gui_prim_t, r_tr    ) );
    test_equal( 40u, (u32)offsetof( gui_prim_t, r_br    ) );
    test_equal( 44u, (u32)offsetof( gui_prim_t, r_bl    ) );
    test_equal( 52u, (u32)offsetof( gui_prim_t, border  ) );
    test_equal( 56u, (u32)offsetof( gui_prim_t, rot_cos ) );
    test_equal( 60u, (u32)offsetof( gui_prim_t, rot_sin ) );
    test_equal( 68u, (u32)offsetof( gui_prim_t, param_b ) );
    test_equal( 72u, (u32)offsetof( gui_prim_t, param_c ) );
    test_equal( 76u, (u32)offsetof( gui_prim_t, col_b   ) );
}

/* The op bits are single bits and DISJOINT, which is the whole claim the op word makes: any op
   composes with any field and with any other op.  A shared bit would silently turn a neighbour on
   -- the failure the tex word's op band was carved out to avoid, restated where it now lives. */
static void
test_prim_ops( void )
{
    const u32 ops[] = { GUI_OP_BAND, GUI_OP_CUT, GUI_OP_INSET,
                        GUI_OP_PULSE, GUI_OP_STRIPES, GUI_OP_SELF };

    u32 seen = 0u;
    for ( u32 i = 0; i < ARRAY_COUNT( ops ); ++i )
    {
        test_true( ops[ i ] != 0u );
        test_equal( 0u, ops[ i ] & ( ops[ i ] - 1u ) );   /* exactly one bit set */
        test_equal( 0u, seen & ops[ i ] );                /* and not one already spent */
        seen |= ops[ i ];
    }
    test_equal( 0x3Fu, seen );
}

/*============================================================================================*/
