/*==============================================================================================

    sandbox/gui/sb_gui_test/test_edit.c -- UTF-8 caret math and word classes (the edit seams).

    The interact-side measurement helpers every text field rides: text_x_at (caret byte ->
    pixel), text_offset_at (click pixel -> caret byte), and the char_class / word_bounds pair
    behind double-click and Ctrl+arrow.  What is worth pinning after the UTF-8 campaign is the
    BOUNDARY contract: a caret position is always a sequence start, a click can never land a
    caret inside a multi-byte character, and no word-class boundary can split a sequence.
    Runs over the synthetic font from test_font.c (advance == codepoint), so every expected
    pixel names exactly which glyphs were measured.

==============================================================================================*/
// clang-format off

#include "runtime_service/gui/interact/gui_interact.h"

/*==============================================================================================
    Cases
==============================================================================================*/

static void
test_edit_caret_utf8( void )
{
    /* "a e-acute euro emoji" -- 1-, 2-, 3-, and 4-byte sequences in one run.
       Advances (== codepoint & 0xFFFF): 97, 233, 8364, 62976.
       Byte boundaries: 0, 1, 3, 6, 10.  Cumulative x: 0, 97, 330, 8694, 71670. */
    const char* s   = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    u32         len = 10;
    font_case_activate();

    /* Caret pixel-x at every sequence boundary. */
    test_true( text_x_at( s, 0 )  == 0.0f );
    test_true( text_x_at( s, 1 )  == 97.0f );
    test_true( text_x_at( s, 3 )  == 330.0f );
    test_true( text_x_at( s, 6 )  == 8694.0f );
    test_true( text_x_at( s, 10 ) == 71670.0f );

    /* A byte offset INSIDE a sequence measures through the whole glyph -- the walk cannot
       split one, so the result equals the next boundary's x. */
    test_true( text_x_at( s, 2 ) == 330.0f );
    test_true( text_x_at( s, 8 ) == 71670.0f );

    /* Click-to-caret: snaps at each glyph's midpoint and returns ONLY sequence boundaries.
       Midpoints: 'a' 48.5, e-acute 213.5, euro 4512, emoji 40182. */
    test_equal( 0u,  text_offset_at( s, len, 0.0f ) );
    test_equal( 0u,  text_offset_at( s, len, 48.0f ) );      /* left half of 'a'      */
    test_equal( 1u,  text_offset_at( s, len, 49.0f ) );      /* right half of 'a'     */
    test_equal( 1u,  text_offset_at( s, len, 213.0f ) );     /* left half of e-acute  */
    test_equal( 3u,  text_offset_at( s, len, 214.0f ) );     /* right half of e-acute */
    test_equal( 3u,  text_offset_at( s, len, 4511.0f ) );    /* left half of euro     */
    test_equal( 6u,  text_offset_at( s, len, 4512.0f ) );    /* right half of euro    */
    test_equal( 6u,  text_offset_at( s, len, 40181.0f ) );   /* left half of emoji    */
    test_equal( 10u, text_offset_at( s, len, 40182.0f ) );   /* right half of emoji   */
    test_equal( 10u, text_offset_at( s, len, 1e9f ) );       /* past the end          */
}

static void
test_edit_word_utf8( void )
{
    /* Every UTF-8 lead and continuation byte is WORD class -- the one rule that keeps a class
       boundary from ever falling inside a sequence, so the byte-walking word ops need no
       decoding at all. */
    test_true( char_class( (u8)'a' )   == 1 );
    test_true( char_class( (u8)0xC3u ) == 1 );   /* lead byte          */
    test_true( char_class( (u8)0xA9u ) == 1 );   /* continuation byte  */
    test_true( char_class( (u8)0xF0u ) == 1 );   /* 4-byte lead        */
    test_true( char_class( (u8)' ' )   == 0 );
    test_true( char_class( (u8)'.' )   == 2 );

    /* "h e-acute llo w o-umlaut rld" -- accented words select whole, across the sequences.
       Bytes: h C3 A9 l l o SP w C3 B6 r l d  (len 13; the space is byte 6). */
    const char* s   = "h\xC3\xA9llo w\xC3\xB6rld";
    u32         len = 13;
    u32         lo, hi;

    word_bounds( s, len, 0, &lo, &hi );   test_equal( 0u, lo );   test_equal( 6u,  hi );
    word_bounds( s, len, 2, &lo, &hi );   test_equal( 0u, lo );   test_equal( 6u,  hi );  /* mid-sequence off */
    word_bounds( s, len, 7, &lo, &hi );   test_equal( 7u, lo );   test_equal( 13u, hi );
    word_bounds( s, len, 6, &lo, &hi );   test_equal( 6u, lo );   test_equal( 7u,  hi );  /* the space itself */
}

// clang-format on
/*============================================================================================*/
