/*==============================================================================================

    sandbox/gui/sb_gui_test/test_fx.c -- the packed effect word.

    THE case for this file: eleven fx modes re-partition ONE 32-bit word, at 1/8 px, 1/4 px,
    1/16 and 1/4 Hz, each field saturating at its own maximum.  Every failure mode here is
    silent -- a shifted field overlaps its neighbour and a shape draws subtly wrong, or an
    out-of-range value wraps and a "fully round" pill comes back as an 88 px radius.  No visual
    demo can show you a wrong bit; only an equality can.

    The strategy is field ISOLATION: drive one field to its maximum with every other field at
    zero and assert the whole word, so a collision between two shifts cannot hide behind a
    field that happens to be zero.  Then drive every field at once and assert the word is
    saturated: that proves all 28 parameter bits are reachable AND that no two fields overlap,
    since overlapping fields could not sum to a full word.

==============================================================================================*/
// clang-format off

/* A NaN built from bits -- 0.0f/0.0f invites the compiler to fold it and warn. */
static f32
fx_nan( void )
{
    union { u32 u; f32 f; } n;
    n.u = 0x7FC00000u;
    return n.f;
}

/*==============================================================================================
    gui_fx_fixed -- the saturating quantizer every packer goes through.
==============================================================================================*/

static void
test_fx_fixed( void )
{
    /* Quantization: value * scale, rounded half-up. */
    test_equal( 0,    gui_fx_fixed( 0.0f,   8.0f, 0xFFFu ) );
    test_equal( 1,    gui_fx_fixed( 0.125f, 8.0f, 0xFFFu ) );   /* one 1/8-px quantum */
    test_equal( 8,    gui_fx_fixed( 1.0f,   8.0f, 0xFFFu ) );
    test_equal( 80,   gui_fx_fixed( 10.0f,  8.0f, 0xFFFu ) );
    test_equal( 1,    gui_fx_fixed( 0.25f,  4.0f, 0x1FFu ) );   /* one 1/4-px quantum */
    test_equal( 8,    gui_fx_fixed( 2.0f,   4.0f, 0x1FFu ) );

    /* Saturation, not wrapping.  This is the whole reason the function exists. */
    test_equal( 0xFFF, gui_fx_fixed( 511.875f, 8.0f, 0xFFFu ) );   /* exactly the max      */
    test_equal( 0xFFF, gui_fx_fixed( 600.0f,   8.0f, 0xFFFu ) );   /* past it -> clamped   */
    test_equal( 0xFFF, gui_fx_fixed( 1.0e9f,   8.0f, 0xFFFu ) );
    test_equal( 0x1FF, gui_fx_fixed( 127.75f,  4.0f, 0x1FFu ) );
    test_equal( 0x7F,  gui_fx_fixed( 15.875f,  8.0f, 0x7Fu  ) );

    /* Negatives and NaN clamp to 0 -- converting a negative float to unsigned is UB. */
    test_equal( 0, gui_fx_fixed( -0.001f, 8.0f, 0xFFFu ) );
    test_equal( 0, gui_fx_fixed( -600.0f, 8.0f, 0xFFFu ) );
    test_equal( 0, gui_fx_fixed( fx_nan(), 8.0f, 0xFFFu ) );
}

/*==============================================================================================
    gui_fx_pack -- mode | radius (4..15) | feather (16..24) | border (25..31)
==============================================================================================*/

static void
test_fx_pack_box( void )
{
    /* Mode occupies the low nibble alone, and survives every other field being zero. */
    test_equal( GUI_FX_NONE, gui_fx_pack( GUI_FX_NONE, 0.0f, 0.0f, 0.0f ) & 0xFu );
    test_equal( GUI_FX_BOX,  gui_fx_pack( GUI_FX_BOX,  0.0f, 0.0f, 0.0f ) & 0xFu );
    test_equal( GUI_FX_RING, gui_fx_pack( GUI_FX_RING, 0.0f, 0.0f, 0.0f ) & 0xFu );

    /* Field isolation: one field at max, the rest zero, assert the ENTIRE word. */
    test_equal( 0x0000FFF1u, gui_fx_pack( GUI_FX_BOX, GUI_FX_RADIUS_MAX,  0.0f, 0.0f ) );
    test_equal( 0x01FF0001u, gui_fx_pack( GUI_FX_BOX, 0.0f, GUI_FX_FEATHER_MAX, 0.0f ) );
    test_equal( 0xFE000001u, gui_fx_pack( GUI_FX_BOX, 0.0f, 0.0f, GUI_FX_BORDER_MAX  ) );

    /* Every field at once saturates the word: all 28 parameter bits reachable, none shared. */
    test_equal( 0xFFFFFFF1u, gui_fx_pack( GUI_FX_BOX, 9999.0f, 9999.0f, 9999.0f ) );

    /* Ordinary values land in the right field and read back exactly. */
    u32 w = gui_fx_pack( GUI_FX_RING, 10.0f, 2.0f, 1.0f );
    test_equal( GUI_FX_RING, w & 0xFu );
    test_equal( 80, ( w >>  4 ) & 0xFFFu );    /* 10.0 px at 1/8 */
    test_equal( 8,  ( w >> 16 ) & 0x1FFu );    /*  2.0 px at 1/4 */
    test_equal( 8,  ( w >> 25 ) & 0x7Fu  );    /*  1.0 px at 1/8 */
}

/*==============================================================================================
    The 600 px regression.

    Documented at gui_fx_fixed: masking AFTER the shift turned a 600 px radius into 88 px --
    the `draw_set_rounding( 9999 )` pill idiom silently drawing the wrong shape with nothing to
    point at.  600 * 8 = 4800, and 4800 & 0xFFF = 704, which is exactly the 88 px the comment
    records.  This asserts the saturated answer AND names the wrong one, so a regression reads
    as itself rather than as an anonymous mismatch.
==============================================================================================*/

static void
test_fx_no_wrap_regression( void )
{
    u32 w = gui_fx_pack( GUI_FX_BOX, 600.0f, 0.0f, 0.0f );
    u32 r = ( w >> 4 ) & 0xFFFu;

    test_equal( 0xFFF, r );          /* saturated to 511.875 px */
    test_not_equal( 704u, r );       /* the wrapped value the old masking produced (88.0 px) */

    /* An over-range radius must not bleed into the neighbouring fields either. */
    test_equal( 0, ( w >> 16 ) & 0x1FFu );
    test_equal( 0, ( w >> 25 ) & 0x7Fu  );
}

/*==============================================================================================
    gui_fx_pack_pulse -- radius / feather keep their positions; RING's border bits become
    rate (25..28) + depth (29..31).
==============================================================================================*/

static void
test_fx_pack_pulse( void )
{
    test_equal( GUI_FX_PULSE, gui_fx_pack_pulse( 0.0f, 0.0f, 0.0f, 0.0f ) & 0xFu );

    test_equal( 0x0000FFF3u, gui_fx_pack_pulse( GUI_FX_RADIUS_MAX, 0.0f, 0.0f, 0.0f ) );
    test_equal( 0x01FF0003u, gui_fx_pack_pulse( 0.0f, GUI_FX_FEATHER_MAX, 0.0f, 0.0f ) );
    test_equal( 0x1E000003u, gui_fx_pack_pulse( 0.0f, 0.0f, GUI_FX_RATE_MAX, 0.0f ) );
    test_equal( 0xE0000003u, gui_fx_pack_pulse( 0.0f, 0.0f, 0.0f, 1.0f ) );

    test_equal( 0xFFFFFFF3u, gui_fx_pack_pulse( 9999.0f, 9999.0f, 9999.0f, 9999.0f ) );

    /* Radius and feather sit where the BOX modes put them -- the fragment decodes them with the
       same two shifts, so a PULSE that moved them would decode as a differently-shaped box. */
    u32 pulse = gui_fx_pack_pulse( 10.0f, 2.0f, 0.0f, 0.0f );
    u32 box   = gui_fx_pack( GUI_FX_BOX, 10.0f, 2.0f, 0.0f );
    test_equal( ( box >> 4 ) & 0xFFFu, ( pulse >> 4 ) & 0xFFFu );
    test_equal( ( box >> 16 ) & 0x1FFu, ( pulse >> 16 ) & 0x1FFu );

    /* Rate quantizes to quarter-Hz so rate * GUI_FX_TIME_WRAP is a whole cycle count. */
    test_equal( 4, ( gui_fx_pack_pulse( 0.0f, 0.0f, 1.0f, 0.0f ) >> 25 ) & 0xFu );
    test_equal( 1, ( gui_fx_pack_pulse( 0.0f, 0.0f, 0.25f, 0.0f ) >> 25 ) & 0xFu );

    /* Depth is 3 bits over 0..1. */
    test_equal( 7, ( gui_fx_pack_pulse( 0.0f, 0.0f, 0.0f, 1.0f ) >> 29 ) & 0x7u );
    test_equal( 0, ( gui_fx_pack_pulse( 0.0f, 0.0f, 0.0f, 0.0f ) >> 29 ) & 0x7u );
}

/*==============================================================================================
    gui_fx_pack_tile_u -- the whole 24-bit parameter field is one repeat count (4..27).
    Bits 28..31 are the one region of a packed word that must stay CLEAR.
==============================================================================================*/

static void
test_fx_pack_tile_u( void )
{
    test_equal( GUI_FX_TILE_U, gui_fx_pack_tile_u( 0.0f ) & 0xFu );

    test_equal( 16,       ( gui_fx_pack_tile_u( 1.0f ) >> 4 ) & 0xFFFFFFu );   /* 1/16 quantum */
    test_equal( 8,        ( gui_fx_pack_tile_u( 0.5f ) >> 4 ) & 0xFFFFFFu );
    test_equal( 0xFFFFFF, ( gui_fx_pack_tile_u( GUI_FX_TILE_MAX ) >> 4 ) & 0xFFFFFFu );

    test_equal( 0x0FFFFFF4u, gui_fx_pack_tile_u( GUI_FX_TILE_MAX ) );

    /* The top nibble is unused by TILE_U and must never be set -- a future mode may claim it. */
    test_equal( 0, gui_fx_pack_tile_u( GUI_FX_TILE_MAX ) >> 28 );
    test_equal( 0, gui_fx_pack_tile_u( 1.0e12f ) >> 28 );
}

/*==============================================================================================
    gui_fx_pack_arc -- ra (4..15) | tube (16..22) | aperture (23..31).  No feather field.
==============================================================================================*/

static void
test_fx_pack_arc( void )
{
    test_equal( GUI_FX_ARC, gui_fx_pack_arc( GUI_FX_ARC, 0.0f, 0.0f, 0.0f ) & 0xFu );
    test_equal( GUI_FX_PIE, gui_fx_pack_arc( GUI_FX_PIE, 0.0f, 0.0f, 0.0f ) & 0xFu );

    /* The two self-sampled variants share ARC's partition exactly -- same fields, same shifts,
       so only the mode nibble may differ.  A drift here would decode a dashed ring as a solid
       one at the wrong radius. */
    u32 arc  = gui_fx_pack_arc( GUI_FX_ARC,      40.0f, 2.0f, 1.0f );
    u32 dash = gui_fx_pack_arc( GUI_FX_ARC_DASH, 40.0f, 2.0f, 1.0f );
    u32 grad = gui_fx_pack_arc( GUI_FX_ARC_GRAD, 40.0f, 2.0f, 1.0f );
    test_equal( arc & ~0xFu, dash & ~0xFu );
    test_equal( arc & ~0xFu, grad & ~0xFu );
    test_equal( GUI_FX_ARC_DASH, dash & 0xFu );
    test_equal( GUI_FX_ARC_GRAD, grad & 0xFu );

    /* Field isolation. */
    test_equal( 0x0000FFF7u, gui_fx_pack_arc( GUI_FX_ARC, GUI_FX_RADIUS_MAX, 0.0f, 0.0f ) );
    test_equal( 0x007F0007u, gui_fx_pack_arc( GUI_FX_ARC, 0.0f, GUI_FX_ARC_TUBE_MAX, 0.0f ) );
    test_equal( 0xFF800007u, gui_fx_pack_arc( GUI_FX_ARC, 0.0f, 0.0f, GUI_FX_PI ) );

    test_equal( 0xFFFFFFF7u, gui_fx_pack_arc( GUI_FX_ARC, 9999.0f, 9999.0f, 9999.0f ) );

    /* ra shares the box modes' radius quantization (1/8 px) -- the doc says so explicitly. */
    test_equal( 320, ( gui_fx_pack_arc( GUI_FX_ARC, 40.0f, 0.0f, 0.0f ) >> 4 ) & 0xFFFu );

    /* tube is HALF the stroke thickness, at 1/8 px. */
    test_equal( 8, ( gui_fx_pack_arc( GUI_FX_ARC, 0.0f, 1.0f, 0.0f ) >> 16 ) & 0x7Fu );

    /* aperture is 9 bits over 0..pi -- HALF the swept angle.  A half-circle sweep (aperture
       pi/2) must land at half scale; allow one step for f32 rounding of 511/pi. */
    u32 half = ( gui_fx_pack_arc( GUI_FX_ARC, 0.0f, 0.0f, GUI_FX_PI * 0.5f ) >> 23 ) & 0x1FFu;
    test_true( half >= 255u && half <= 257u );

    /* PIE packs tube 0 by convention -- callers pass it, so assert the field is honoured. */
    test_equal( 0, ( gui_fx_pack_arc( GUI_FX_PIE, 40.0f, 0.0f, 1.0f ) >> 16 ) & 0x7Fu );
}

/*==============================================================================================
    gui_fx_pack_text_edge -- width (4..11) | r (12..16) | g (17..21) | b (22..26) | a (27..31).
    The only packer whose shape comes from the texture, so it spends all 28 bits.
==============================================================================================*/

static void
test_fx_pack_text_edge( void )
{
    test_equal( GUI_FX_TEXT_EDGE, gui_fx_pack_text_edge( 0.0f, 0u ) & 0xFu );

    /* Field isolation -- width alone, then each colour channel alone. */
    test_equal( 0x00000FF5u, gui_fx_pack_text_edge( GUI_FX_EDGE_MAX, 0x00000000u ) );
    test_equal( 0x0001F005u, gui_fx_pack_text_edge( 0.0f, 0x000000FFu ) );   /* R */
    test_equal( 0x003E0005u, gui_fx_pack_text_edge( 0.0f, 0x0000FF00u ) );   /* G */
    test_equal( 0x07C00005u, gui_fx_pack_text_edge( 0.0f, 0x00FF0000u ) );   /* B */
    test_equal( 0xF8000005u, gui_fx_pack_text_edge( 0.0f, 0xFF000000u ) );   /* A */

    test_equal( 0xFFFFFFF5u, gui_fx_pack_text_edge( 9999.0f, 0xFFFFFFFFu ) );

    /* Colour is 5 bits per channel: the doc promises black and white are EXACT. */
    test_equal( 31, ( gui_fx_pack_text_edge( 0.0f, 0xFFFFFFFFu ) >> 12 ) & 0x1Fu );
    test_equal( 0,  ( gui_fx_pack_text_edge( 0.0f, 0xFF000000u ) >> 12 ) & 0x1Fu );

    /* Channel order is R-in-the-low-byte (R8G8B8A8_UNORM), matching every other colour word
       here.  A swapped pair would outline text in the wrong hue and nothing would crash. */
    u32 w = gui_fx_pack_text_edge( 0.0f, 0xFF204060u );   /* a=FF b=20 g=40 r=60 */
    test_equal( 0x60u >> 3, ( w >> 12 ) & 0x1Fu );
    test_equal( 0x40u >> 3, ( w >> 17 ) & 0x1Fu );
    test_equal( 0x20u >> 3, ( w >> 22 ) & 0x1Fu );
    test_equal( 0xFFu >> 3, ( w >> 27 ) & 0x1Fu );

    /* width is 8 bits at 1/8 px. */
    test_equal( 8,    ( gui_fx_pack_text_edge( 1.0f, 0u ) >> 4 ) & 0xFFu );
    test_equal( 0xFF, ( gui_fx_pack_text_edge( 99.0f, 0u ) >> 4 ) & 0xFFu );
}

/*==============================================================================================
    Cross-mode invariants -- properties that must hold for EVERY packer at once.
==============================================================================================*/

static void
test_fx_mode_nibble( void )
{
    /* Whatever a packer does with its 28 parameter bits, the fragment reads the mode from the
       low nibble first.  A packer whose parameters reached bit 0..3 would decode as a
       different SHAPE -- the one failure here that changes everything downstream. */
    test_equal( GUI_FX_BOX,       gui_fx_pack( GUI_FX_BOX, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );
    test_equal( GUI_FX_RING,      gui_fx_pack( GUI_FX_RING, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );
    test_equal( GUI_FX_PULSE,     gui_fx_pack_pulse( 9999.0f, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );
    test_equal( GUI_FX_TILE_U,    gui_fx_pack_tile_u( 1.0e12f ) & 0xFu );
    test_equal( GUI_FX_TEXT_EDGE, gui_fx_pack_text_edge( 9999.0f, 0xFFFFFFFFu ) & 0xFu );
    test_equal( GUI_FX_ARC,       gui_fx_pack_arc( GUI_FX_ARC, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );
    test_equal( GUI_FX_PIE,       gui_fx_pack_arc( GUI_FX_PIE, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );
    test_equal( GUI_FX_ARC_DASH,  gui_fx_pack_arc( GUI_FX_ARC_DASH, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );
    test_equal( GUI_FX_ARC_GRAD,  gui_fx_pack_arc( GUI_FX_ARC_GRAD, 9999.0f, 9999.0f, 9999.0f ) & 0xFu );

    /* Every live mode fits the nibble the shader masks with (GUI_FX_MODE_BITS == 4). */
    test_equal( 4, GUI_FX_MODE_BITS );
    test_true( (u32)GUI_FX_ARC_GRAD < ( 1u << GUI_FX_MODE_BITS ) );

    /* Mode 0 is "no effect", and a zero word must mean exactly that: the tessellator memsets
       the band and every non-SDF primitive relies on the result decoding as NONE. */
    test_equal( GUI_FX_NONE, 0u & 0xFu );
    test_equal( 0u, gui_fx_pack( GUI_FX_NONE, 0.0f, 0.0f, 0.0f ) );
}

/*============================================================================================*/
