/*==============================================================================================

    sandbox/gui/sb_gui_test/test_font.c -- the two-tier glyph lookup (font_slot_cp).

    The lookup rule every measure and draw resolves through: ASCII 32..126 indexes the dense
    lookup[] directly, anything above binary-searches the sorted ext[] records, and any miss --
    control byte, unmapped codepoint -- lands on '?'.  The binary search is the part worth
    pinning: an off-by-one there reads as "one Greek letter renders as '?'" in a demo, which is
    the hard form of the bug to notice.  These cases run it over a hand-built slot -- no file,
    no registry, no context.

    The last section is the .orb_font byte contract as font_load_mem enforces it, over a
    minimal font assembled in memory and bent one field at a time.

==============================================================================================*/
// clang-format off

#include <string.h>   /* memset / memcpy -- the case slot resets between builds; the byte cases */

#include "runtime_service/gui/font/gui_font.h"

/* A glyph record whose advance encodes the codepoint's low byte, so a lookup result identifies
   itself.  The actual match (font_slot_cp's binary search over ext[].codepoint) is unaffected --
   codepoint stays a full u32 -- this only feeds the width-sum checks in test_font_measure_utf8,
   which mask the expected codepoint the same way. */
static orb_font_glyph_t
font_case_rec( u32 cp )
{
    orb_font_glyph_t g = { 0 };
    g.codepoint = cp;
    g.advance   = (u8)( cp & 0xFFu );
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
    slot->ext_count = (u16)ext_count;
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

/* Fill the LAST registry slot with the synthetic font (advance == codepoint & 0xFF, ext
   carries e-acute / euro / a slightly-smiling emoji) and activate it.  Shared by the measure
   case below and the caret cases in test_edit.c -- the readers under test all measure through
   the active-slot pointer, not a parameter.  Idempotent; the ext store is static and never
   freed (no registry reset runs in this bed).

   The emoji is U+1F642, not the more obvious U+1F600: 0x1F600 & 0xFF is 0, which would give that
   glyph zero width and make test_edit.c's midpoint-click case for it degenerate. */
static void
font_case_activate( void )
{
    font_slot_t* slot = font_slot_ptr( GUI_FONT_REGISTRY_MAX - 1 );
    memset( slot, 0, sizeof( *slot ) );
    for ( u32 cp = ORB_FONT_CP_FIRST; cp <= ORB_FONT_CP_LAST; ++cp )
        slot->lookup[ cp - ORB_FONT_CP_FIRST ] = font_case_rec( cp );

    static orb_font_glyph_t ext[ 3 ];
    ext[ 0 ]        = font_case_rec( 0x00E9u );    /* e-acute, 2 bytes            */
    ext[ 1 ]        = font_case_rec( 0x20ACu );    /* euro, 3 bytes               */
    ext[ 2 ]        = font_case_rec( 0x1F642u );   /* slightly-smiling, 4 bytes   */
    slot->ext       = ext;
    slot->ext_count = 3;
    slot->used      = true;
    font_activate( GUI_FONT_REGISTRY_MAX - 1 );
}

static void
test_font_measure_utf8( void )
{
    /* font_text_w_n decodes UTF-8 -- the one measure loop with its own hand-inlined fast path,
       so it can drift from font_slot_cp independently.  Advances encode the codepoint, so each
       expected width names exactly which glyphs were counted. */
    font_case_activate();

    /* Pure ASCII: the dense fast path, one advance per byte. */
    test_true( font_text_w_n( "abc", 3 ) == (f32)( 'a' + 'b' + 'c' ) );

    /* Mixed run: 2- and 3-byte sequences measure as ONE codepoint each, not per byte.  Expected
       widths mask each codepoint to a byte, matching advance's u8 encoding (font_case_rec). */
    test_true( font_text_w_n( "a\xC3\xA9\xE2\x82\xAC", 6 )
               == (f32)( 'a' + ( 0x00E9 & 0xFF ) + ( 0x20AC & 0xFF ) ) );
    test_true( font_text_w( "a\xC3\xA9" ) == (f32)( 'a' + ( 0x00E9 & 0xFF ) ) );

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

/*==============================================================================================
    The resolver's name utilities -- how a request matches files and memo keys.
==============================================================================================*/

static void
test_font_name_normalize( void )
{
    char a[ 96 ], b[ 96 ];

    /* Spaces, case, and punctuation all vanish: a friendly name, a filename stem, and a
       cache-mangled stem of the same face compare equal. */
    font_name_normalize( "Cascadia Mono", a, sizeof( a ) );
    font_name_normalize( "CascadiaMono",  b, sizeof( b ) );
    test_true( strcmp( a, b ) == 0 );
    font_name_normalize( "cascadia_mono", b, sizeof( b ) );
    test_true( strcmp( a, b ) == 0 );

    font_name_normalize( "Roboto-Regular", a, sizeof( a ) );
    font_name_normalize( "Roboto_Regular", b, sizeof( b ) );   /* dev_font cache mangling */
    test_true( strcmp( a, b ) == 0 );

    font_name_normalize( "JetBrains Mono NL", a, sizeof( a ) );
    test_true( strcmp( a, "jetbrainsmononl" ) == 0 );

    /* Distinct faces stay distinct. */
    font_name_normalize( "JetBrainsMonoNL-Regular", b, sizeof( b ) );
    test_true( strcmp( a, b ) != 0 );
}

/*==============================================================================================
    The .orb_font byte contract -- what font_load_mem accepts.

    A minimal font: header, reference section, one glyph, a 1x1 atlas (orb_font.h).  Each case
    assembles the pieces into a buffer -- appended one by one, never as a struct, so no padding
    lands between them.  The reference section is the field under test: it sits between the
    header and the glyph records, so a count the bytes do not back (or bytes no count admits to)
    shifts every record after it.  The loader must refuse both at parse, and refuse a count past
    RES_REF_MAX before it sizes anything.
==============================================================================================*/

/* Assemble a font whose header claims `ref_count` references while `refs_written` ids actually
   follow it (equal for a well-formed file).  Returns the byte count written into `out`. */
static u32
font_case_build( u8* out, u32 version, u32 ref_count, u32 refs_written )
{
    orb_font_header_t hdr = { 0 };
    hdr.magic       = ORB_FONT_MAGIC;
    hdr.version     = version;
    hdr.atlas_w     = 1;
    hdr.atlas_h     = 1;
    hdr.font_size   = 8;
    hdr.ascent      = 6;
    hdr.descent     = -2;
    hdr.line_gap    = 0;
    hdr.glyph_count = 1;
    hdr.sdf_range   = 0;
    hdr.ref_count   = ref_count;

    orb_font_glyph_t g = { 0 };
    g.codepoint = 'A';
    g.w         = 1;
    g.h         = 1;
    g.advance   = 1;

    u32 refs[ 4 ] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
    u8  pixel     = 255;

    u32 n = 0;
    memcpy( out + n, &hdr, sizeof( hdr ) );              n += (u32)sizeof( hdr );
    memcpy( out + n, refs, sizeof( u32 ) * refs_written ); n += (u32)( sizeof( u32 ) * refs_written );
    memcpy( out + n, &g, sizeof( g ) );                  n += (u32)sizeof( g );
    out[ n++ ] = pixel;
    return n;
}

static void
test_font_file_contract( void )
{
    u8  buf[ 256 ];
    u32 n;

    /* Empty reference section: the shape every bake writes today. */
    n = font_case_build( buf, ORB_FONT_VERSION, 0, 0 );
    test_true( font_load_mem( buf, n, "case/plain" ) != 0 );

    /* Populated section: two ids the loader steps over to reach the glyph record. */
    n = font_case_build( buf, ORB_FONT_VERSION, 2, 2 );
    test_true( font_load_mem( buf, n, "case/refs" ) != 0 );

    /* Bad section length, short: a count with no bytes behind it.  The buffer is 4 bytes shorter
       than the header claims and the glyph record would be read 4 bytes late. */
    n = font_case_build( buf, ORB_FONT_VERSION, 1, 0 );
    test_true( font_load_mem( buf, n, "case/short" ) == 0 );

    /* Bad section length, long: bytes no count admits to. */
    n = font_case_build( buf, ORB_FONT_VERSION, 0, 1 );
    test_true( font_load_mem( buf, n, "case/long" ) == 0 );

    /* A count past the format cap is refused on the count alone. */
    n = font_case_build( buf, ORB_FONT_VERSION, RES_REF_MAX + 1, 0 );
    test_true( font_load_mem( buf, n, "case/cap" ) == 0 );

    /* The previous version, whose header had no ref_count: refused outright. */
    n = font_case_build( buf, ORB_FONT_VERSION - 1, 0, 0 );
    test_true( font_load_mem( buf, n, "case/old" ) == 0 );
}

// clang-format on
/*============================================================================================*/
