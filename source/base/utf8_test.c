/*==============================================================================================

    utf8_test.c -- Tests for base/utf8.h.

    Included by base_test.c (unity). Non-ASCII test data is expressed as \x byte escapes
    so the source file itself stays 7-bit ASCII per project rules.

    Reference sequences:
        U+00E9  e-acute            C3 A9          (2 bytes)
        U+20AC  euro sign          E2 82 AC       (3 bytes)
        U+1F600 emoji face         F0 9F 98 80    (4 bytes, supplementary plane)

==============================================================================================*/

/*==============================================================================================
    decode: valid sequences
==============================================================================================*/

static void
test_utf8_decode_valid( void )
{
    u32 adv = 0;

    /* ASCII fast path */
    test_equal( 0x41u, utf8_decode( "A", &adv ) );
    test_equal( 1, adv );

    /* 2-byte: U+00E9 */
    test_equal( 0xE9u, utf8_decode( "\xC3\xA9", &adv ) );
    test_equal( 2, adv );

    /* 3-byte: U+20AC */
    test_equal( 0x20ACu, utf8_decode( "\xE2\x82\xAC", &adv ) );
    test_equal( 3, adv );

    /* 4-byte: U+1F600 */
    test_equal( 0x1F600u, utf8_decode( "\xF0\x9F\x98\x80", &adv ) );
    test_equal( 4, adv );

    /* boundary codepoints of each length class */
    test_equal( 0x7Fu, utf8_decode( "\x7F", &adv ) );          /* last 1-byte */
    test_equal( 0x80u, utf8_decode( "\xC2\x80", &adv ) );      /* first 2-byte */
    test_equal( 0x7FFu, utf8_decode( "\xDF\xBF", &adv ) );     /* last 2-byte */
    test_equal( 0x800u, utf8_decode( "\xE0\xA0\x80", &adv ) ); /* first 3-byte */
    test_equal( 0xFFFDu, utf8_decode( "\xEF\xBF\xBD", &adv ) );/* U+FFFD itself, valid */
    test_equal( 3, adv );
    test_equal( 0xFFFFu, utf8_decode( "\xEF\xBF\xBF", &adv ) );        /* last 3-byte */
    test_equal( 0x10000u, utf8_decode( "\xF0\x90\x80\x80", &adv ) );   /* first 4-byte */
    test_equal( 0x10FFFFu, utf8_decode( "\xF4\x8F\xBF\xBF", &adv ) );  /* last valid cp */
    test_equal( 4, adv );
}

/*==============================================================================================
    decode: malformed input -> U+FFFD, advance 1, never past NUL
==============================================================================================*/

static void
test_utf8_decode_invalid( void )
{
    u32 adv = 0;

    /* lone continuation byte */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\x80", &adv ) );
    test_equal( 1, adv );

    /* truncated 2-byte lead (NUL follows -- must not read past it) */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xC3", &adv ) );
    test_equal( 1, adv );

    /* truncated 3-byte in the middle */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xE2\x82", &adv ) );
    test_equal( 1, adv );

    /* lead followed by a plain ASCII byte instead of a continuation */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xC3(", &adv ) );
    test_equal( 1, adv );

    /* overlong encodings: 2-byte NUL, 3-byte slash */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xC0\x80", &adv ) );
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xE0\x80\xAF", &adv ) );

    /* encoded surrogate half U+D800 */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xED\xA0\x80", &adv ) );

    /* past U+10FFFF */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xF4\x90\x80\x80", &adv ) );

    /* invalid lead bytes */
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xFE", &adv ) );
    test_equal( UTF8_REPLACEMENT, utf8_decode( "\xFF", &adv ) );
}

/*==============================================================================================
    encode + round-trip
==============================================================================================*/

static void
test_utf8_encode( void )
{
    char out[ UTF8_MAX_BYTES ];

    test_equal( 1, utf8_encode( 0x41u, out ) );
    test_true( out[ 0 ] == 'A' );

    test_equal( 2, utf8_encode( 0xE9u, out ) );
    test_true( out[ 0 ] == '\xC3' && out[ 1 ] == '\xA9' );

    test_equal( 3, utf8_encode( 0x20ACu, out ) );
    test_true( out[ 0 ] == '\xE2' && out[ 1 ] == '\x82' && out[ 2 ] == '\xAC' );

    test_equal( 4, utf8_encode( 0x1F600u, out ) );
    test_true( out[ 0 ] == '\xF0' && out[ 1 ] == '\x9F' && out[ 2 ] == '\x98' &&
               out[ 3 ] == '\x80' );

    /* rejected values */
    test_equal( 0, utf8_encode( 0xD800u, out ) );      /* surrogate lo bound */
    test_equal( 0, utf8_encode( 0xDFFFu, out ) );      /* surrogate hi bound */
    test_equal( 0, utf8_encode( 0x110000u, out ) );    /* past max */

    /* round-trip every length-class boundary */
    static const u32 cps[] = { 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xFFFFu, 0x10000u, 0x10FFFFu };
    for ( u32 i = 0; i < sizeof( cps ) / sizeof( cps[ 0 ] ); ++i )
    {
        char buf[ UTF8_MAX_BYTES + 1 ] = { 0 };
        u32  n                         = utf8_encode( cps[ i ], buf );
        u32  adv                       = 0;
        test_true( n >= 1 && n <= 4 );
        test_equal( cps[ i ], utf8_decode( buf, &adv ) );
        test_equal( n, adv );
    }
}

/*==============================================================================================
    stepping + counting  ("a" U+00E9 "b" U+20AC U+1F600 -- starts at 0,1,3,4,7, len 11)
==============================================================================================*/

static void
test_utf8_step( void )
{
    const char* s   = "a\xC3\xA9"
                      "b\xE2\x82\xAC\xF0\x9F\x98\x80";
    i32         len = 11;

    /* forward walk hits each codepoint start */
    test_equal( 1, utf8_next( s, len, 0 ) );
    test_equal( 3, utf8_next( s, len, 1 ) );
    test_equal( 4, utf8_next( s, len, 3 ) );
    test_equal( 7, utf8_next( s, len, 4 ) );
    test_equal( 11, utf8_next( s, len, 7 ) );
    test_equal( 11, utf8_next( s, len, 11 ) );    /* clamped at end */

    /* backward walk mirrors it */
    test_equal( 7, utf8_prev( s, 11 ) );
    test_equal( 4, utf8_prev( s, 7 ) );
    test_equal( 3, utf8_prev( s, 4 ) );
    test_equal( 1, utf8_prev( s, 3 ) );
    test_equal( 0, utf8_prev( s, 1 ) );
    test_equal( 0, utf8_prev( s, 0 ) );    /* clamped at start */

    /* prev from the middle of a sequence snaps to its start */
    test_equal( 7, utf8_prev( s, 9 ) );

    test_equal( 5, utf8_count( s, len ) );
    test_equal( 0, utf8_count( s, 0 ) );
    test_equal( 2, utf8_count( s, 3 ) );    /* "a" + e-acute */
}

/*==============================================================================================
    UTF-16 surrogate seam
==============================================================================================*/

static void
test_utf16_surrogates( void )
{
    /* U+1F600 encodes as the pair D83D DE00 */
    test_true( utf16_is_high( 0xD83Du ) );
    test_true( utf16_is_low( 0xDE00u ) );
    test_false( utf16_is_high( 0xDE00u ) );
    test_false( utf16_is_low( 0xD83Du ) );
    test_false( utf16_is_high( 0x0041u ) );
    test_false( utf16_is_low( 0xE000u ) );    /* first cp past the surrogate block */

    test_equal( 0x1F600u, utf16_pair_to_cp( 0xD83Du, 0xDE00u ) );
    test_equal( 0x10000u, utf16_pair_to_cp( 0xD800u, 0xDC00u ) );    /* first pair */
    test_equal( 0x10FFFFu, utf16_pair_to_cp( 0xDBFFu, 0xDFFFu ) );   /* last pair */
}

/*============================================================================================*/
