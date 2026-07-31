/*==============================================================================================

    sandbox/gui/sb_gui_test/test_font.c -- the two-tier glyph lookup (font_slot_cp).

    The lookup rule every measure and draw resolves through: ASCII 32..126 indexes the dense
    lookup[] directly, anything above binary-searches the sorted ext[] records, and any miss --
    control byte, unmapped codepoint -- lands on '?'.  The binary search is the part worth
    pinning: an off-by-one there reads as "one Greek letter renders as '?'" in a demo, which is
    the hard form of the bug to notice.  These cases run it over a hand-built slot -- no file,
    no registry, no context.

==============================================================================================*/
// clang-format off

#include <string.h>   /* memset -- the case slot resets between builds */

#include "runtime_service/gui/font/gui_font.h"

/* A glyph record whose advance encodes its codepoint, so a lookup result identifies itself. */
static orb_font_glyph_t
font_case_rec( u32 cp )
{
    orb_font_glyph_t g = { 0 };
    g.codepoint = cp;
    g.advance   = (u16)( cp & 0xFFFFu );
    return g;
}

/* Build a slot with every ASCII record filled and the given sorted extended codepoints. */
static font_slot_t s_font_case_slot;    /* static: font_slot_t is large and cases run on one */

static font_slot_t*
font_case_slot( const u32* ext_cps, u32 ext_count, orb_font_glyph_t* ext_store )
{
    font_slot_t* slot = &s_font_case_slot;
    memset( slot, 0, sizeof( *slot ) );
    for ( u32 cp = ORB_FONT_CP_FIRST; cp <= ORB_FONT_CP_LAST; ++cp )
        slot->lookup[ cp - ORB_FONT_CP_FIRST ] = font_case_rec( cp );
    for ( u32 i = 0; i < ext_count; ++i )
        ext_store[ i ] = font_case_rec( ext_cps[ i ] );
    slot->ext       = ext_store;
    slot->ext_count = ext_count;
    return slot;
}

/*==============================================================================================
    Cases
==============================================================================================*/

static void
test_font_cp_ascii( void )
{
    /* The dense tier: every printable ASCII codepoint resolves to its own record, ext untouched. */
    font_slot_t* slot = font_case_slot( NULL, 0, NULL );
    test_true( font_slot_cp( slot, ' ' )->advance == ' ' );
    test_true( font_slot_cp( slot, 'A' )->advance == 'A' );
    test_true( font_slot_cp( slot, '~' )->advance == '~' );

    /* Both edges just OUTSIDE the span miss to '?' -- 31 must not wrap into the table, 127 must
       not read one past it. */
    test_true( font_slot_cp( slot, 31u  )->advance == '?' );
    test_true( font_slot_cp( slot, 127u )->advance == '?' );
    test_true( font_slot_cp( slot, 0u   )->advance == '?' );
}

static void
test_font_cp_ext_search( void )
{
    /* The sorted-ext tier: hits at the first, an interior, and the last record -- the three
       positions a binary-search off-by-one can each lose independently.  Gapped codepoints so
       every probe between records is a genuine miss. */
    static const u32 cps[] = { 0x00E9u, 0x0394u, 0x0416u, 0x20ACu, 0x1F600u };
    orb_font_glyph_t store[ ARRAY_COUNT( cps ) ];
    font_slot_t*     slot = font_case_slot( cps, ARRAY_COUNT( cps ), store );

    for ( u32 i = 0; i < ARRAY_COUNT( cps ); ++i )
        test_true( font_slot_cp( slot, cps[ i ] )->codepoint == cps[ i ] );

    /* Misses on every side: below the first, in each gap, above the last -> '?'. */
    test_true( font_slot_cp( slot, 0x00E8u  )->advance == '?' );
    test_true( font_slot_cp( slot, 0x0395u  )->advance == '?' );
    test_true( font_slot_cp( slot, 0x20ABu  )->advance == '?' );
    test_true( font_slot_cp( slot, 0x1F601u )->advance == '?' );

    /* An even record count exercises the other mid-rounding parity. */
    static const u32 cps_even[] = { 0x00A0u, 0x0100u, 0x0370u, 0x0400u };
    orb_font_glyph_t store_even[ ARRAY_COUNT( cps_even ) ];
    slot = font_case_slot( cps_even, ARRAY_COUNT( cps_even ), store_even );
    for ( u32 i = 0; i < ARRAY_COUNT( cps_even ); ++i )
        test_true( font_slot_cp( slot, cps_even[ i ] )->codepoint == cps_even[ i ] );
    test_true( font_slot_cp( slot, 0x0101u )->advance == '?' );
}

static void
test_font_measure_utf8( void )
{
    /* font_text_w_n decodes UTF-8 -- the one measure loop with its own hand-inlined fast path,
       so it can drift from font_slot_cp independently.  Advances encode the codepoint, so each
       expected width names exactly which glyphs were counted.  Uses a real registry slot +
       font_activate: the reader measures through the active-slot pointer, not a parameter. */
    font_slot_t* slot = font_slot_ptr( GUI_FONT_REGISTRY_MAX - 1 );
    memset( slot, 0, sizeof( *slot ) );
    for ( u32 cp = ORB_FONT_CP_FIRST; cp <= ORB_FONT_CP_LAST; ++cp )
        slot->lookup[ cp - ORB_FONT_CP_FIRST ] = font_case_rec( cp );

    static orb_font_glyph_t ext[ 2 ];   /* static, never freed: no registry reset runs here */
    ext[ 0 ]        = font_case_rec( 0x00E9u );   /* e-acute */
    ext[ 1 ]        = font_case_rec( 0x20ACu );   /* euro    */
    slot->ext       = ext;
    slot->ext_count = 2;
    slot->used      = true;
    font_activate( GUI_FONT_REGISTRY_MAX - 1 );

    /* Pure ASCII: the dense fast path, one advance per byte. */
    test_true( font_text_w_n( "abc", 3 ) == (f32)( 'a' + 'b' + 'c' ) );

    /* Mixed run: 2- and 3-byte sequences measure as ONE codepoint each, not per byte. */
    test_true( font_text_w_n( "a\xC3\xA9\xE2\x82\xAC", 6 ) == (f32)( 'a' + 0x00E9 + 0x20AC ) );
    test_true( font_text_w( "a\xC3\xA9" ) == (f32)( 'a' + 0x00E9 ) );

    /* Malformed input degrades per byte: an invalid lead and a NUL-truncated sequence each
       decode to the replacement codepoint with a 1-byte step, and measure as '?'. */
    test_true( font_text_w_n( "\xFF", 1 )     == (f32)'?' );
    test_true( font_text_w_n( "\xE2\x82", 2 ) == (f32)( '?' + '?' ) );

    /* A codepoint the font lacks (in a valid sequence) also measures as '?'. */
    test_true( font_text_w_n( "\xCE\x94", 2 ) == (f32)'?' );   /* U+0394, not in ext */
}

static void
test_font_cp_ext_empty( void )
{
    /* An ASCII-only font (ext NULL / count 0): every extended codepoint falls back to '?'
       without the search ever touching the NULL ext pointer. */
    font_slot_t* slot = font_case_slot( NULL, 0, NULL );
    test_true( font_slot_cp( slot, 0x00E9u  )->advance == '?' );
    test_true( font_slot_cp( slot, 0x20ACu  )->advance == '?' );
    test_true( font_slot_cp( slot, 0x10FFFFu )->advance == '?' );
}

// clang-format on
/*============================================================================================*/
