/*==============================================================================================

    sandbox/gui/sb_gui_test/test_pack.c -- the vertex, and the record it points at.

    The UV packing runs on every textured vertex the gui emits and fails silently: a swapped half
    samples the wrong row of the atlas, which is loud, but a swap in ONE emit path is not.

    The record has no packing to get wrong -- what it has is a LAYOUT the shaders index by literal
    row, so a field inserted or a type widened would slide a corner radius into a rotation with no
    compile error anywhere.  That is the other thing here.

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

    /* The clear is the contract: a plain vertex must name record 0.  The tessellator's commit
       point stamps the real index, so a constructor that left ambient state in place would point a
       fresh primitive at whatever shape happened to be built before it. */
    test_equal( 0u, v.prim );
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
    /* Eight 16-byte rows, no tail padding.  GUI_PRIM_ROWS is what the shaders multiply by. */
    test_equal( 8u,   GUI_PRIM_ROWS );
    test_equal( 128u, (u32)GUI_PRIM_BYTES );
    test_equal( 128u, (u32)sizeof( gui_prim_t ) );

    /* Row starts.  The hot word leads on purpose: a glyph or a flat fill reads row 0 and stops. */
    test_equal(  0u, (u32)offsetof( gui_prim_t, field   ) );
    test_equal( 16u, (u32)offsetof( gui_prim_t, cx      ) );
    test_equal( 32u, (u32)offsetof( gui_prim_t, r_tl    ) );
    test_equal( 48u, (u32)offsetof( gui_prim_t, feather ) );
    test_equal( 64u, (u32)offsetof( gui_prim_t, param_a ) );
    test_equal( 80u, (u32)offsetof( gui_prim_t, grad_x  ) );
    test_equal( 96u, (u32)offsetof( gui_prim_t, anim_rate   ) );
    test_equal( 112u, (u32)offsetof( gui_prim_t, dash_period ) );

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
    test_equal( 84u, (u32)offsetof( gui_prim_t, grad_y  ) );
    test_equal( 88u, (u32)offsetof( gui_prim_t, cut_dx  ) );
    test_equal( 92u, (u32)offsetof( gui_prim_t, cut_dy  ) );
    test_equal( 100u, (u32)offsetof( gui_prim_t, anim_phase ) );
    test_equal( 104u, (u32)offsetof( gui_prim_t, grad_mid   ) );
    test_equal( 116u, (u32)offsetof( gui_prim_t, dash_duty  ) );
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
                        GUI_OP_SPIN, GUI_OP_DASH, GUI_OP_DITHER };

    u32 seen = 0u;
    for ( u32 i = 0; i < ARRAY_COUNT( ops ); ++i )
    {
        test_true( ops[ i ] != 0u );
        test_equal( 0u, ops[ i ] & ( ops[ i ] - 1u ) );   /* exactly one bit set */
        test_equal( 0u, seen & ops[ i ] );                /* and not one already spent */
        seen |= ops[ i ];
    }
    test_equal( 0xFFFu, seen );
}

/*============================================================================================*/
