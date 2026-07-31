/*==============================================================================================

    utf8.h -- UTF-8 encode/decode and codepoint stepping.

        The engine's strings stay char* / byte-indexed; this unit is the one place that
        knows the encoding. Strict decoder: overlong forms, surrogate codepoints and
        values past U+10FFFF all yield UTF8_REPLACEMENT with a 1-byte advance, so a
        damaged stream resynchronizes at the next byte and never loops or reads past a
        terminating NUL. Includes the UTF-16 surrogate helpers for the Win32 WM_CHAR seam.

==============================================================================================*/
#ifndef UTF8_H
#define UTF8_H

#define UTF8_REPLACEMENT 0xFFFDu      // U+FFFD, returned for every malformed sequence
#define UTF8_MAX_CP      0x10FFFFu    // highest valid Unicode codepoint
#define UTF8_MAX_BYTES   4            // longest encoded sequence

// clang-format off
/*==============================================================================================
    classification
==============================================================================================*/

// Continuation byte (10xxxxxx) -- any byte that is not the start of a codepoint.
ORB_INLINE b32
utf8_is_cont( u8 b )
{
    return ( b & 0xC0u ) == 0x80u;
}

/*==============================================================================================
    decode / encode
==============================================================================================*/

// Decode one codepoint at s. Writes the byte count consumed to *adv (always >= 1).
// Malformed input returns UTF8_REPLACEMENT with *adv = 1. Continuation bytes are checked
// before they are consumed, so decoding never reads past a NUL terminator.
ORB_INLINE u32
utf8_decode( const char* s, u32* adv )
{
    u8 b0 = ( u8 )s[ 0 ];
    if ( b0 < 0x80u )
    {
        *adv = 1;
        return b0;
    }

    u32 n, cp, min;
    if ( ( b0 & 0xE0u ) == 0xC0u )      { n = 1; cp = b0 & 0x1Fu; min = 0x80u; }
    else if ( ( b0 & 0xF0u ) == 0xE0u ) { n = 2; cp = b0 & 0x0Fu; min = 0x800u; }
    else if ( ( b0 & 0xF8u ) == 0xF0u ) { n = 3; cp = b0 & 0x07u; min = 0x10000u; }
    else
    {
        *adv = 1;
        return UTF8_REPLACEMENT;
    }

    for ( u32 i = 1; i <= n; ++i )
    {
        u8 b = ( u8 )s[ i ];
        if ( ( b & 0xC0u ) != 0x80u )
        {
            *adv = 1;
            return UTF8_REPLACEMENT;
        }
        cp = ( cp << 6 ) | ( b & 0x3Fu );
    }

    if ( cp < min || cp > UTF8_MAX_CP || ( cp - 0xD800u ) < 0x800u )
    {
        *adv = 1;
        return UTF8_REPLACEMENT;
    }

    *adv = n + 1u;
    return cp;
}

// Encode one codepoint into out (not null-terminated). Returns bytes written, or 0 if cp
// is a surrogate or past U+10FFFF -- callers treat 0 as "drop the character".
ORB_INLINE u32
utf8_encode( u32 cp, char out[ UTF8_MAX_BYTES ] )
{
    if ( cp < 0x80u )
    {
        out[ 0 ] = ( char )cp;
        return 1;
    }
    if ( cp < 0x800u )
    {
        out[ 0 ] = ( char )( 0xC0u | ( cp >> 6 ) );
        out[ 1 ] = ( char )( 0x80u | ( cp & 0x3Fu ) );
        return 2;
    }
    if ( ( cp - 0xD800u ) < 0x800u )
        return 0;
    if ( cp < 0x10000u )
    {
        out[ 0 ] = ( char )( 0xE0u | ( cp >> 12 ) );
        out[ 1 ] = ( char )( 0x80u | ( ( cp >> 6 ) & 0x3Fu ) );
        out[ 2 ] = ( char )( 0x80u | ( cp & 0x3Fu ) );
        return 3;
    }
    if ( cp <= UTF8_MAX_CP )
    {
        out[ 0 ] = ( char )( 0xF0u | ( cp >> 18 ) );
        out[ 1 ] = ( char )( 0x80u | ( ( cp >> 12 ) & 0x3Fu ) );
        out[ 2 ] = ( char )( 0x80u | ( ( cp >> 6 ) & 0x3Fu ) );
        out[ 3 ] = ( char )( 0x80u | ( cp & 0x3Fu ) );
        return 4;
    }
    return 0;
}

/*==============================================================================================
    stepping  (byte indices in, byte indices out -- caret math stays in byte space)
==============================================================================================*/

// Byte index of the next codepoint start after i (clamped to len).
ORB_INLINE i32
utf8_next( const char* s, i32 len, i32 i )
{
    if ( i >= len )
        return len;
    ++i;
    while ( i < len && utf8_is_cont( ( u8 )s[ i ] ) )
        ++i;
    return i;
}

// Byte index of the codepoint start before i (clamped to 0).
ORB_INLINE i32
utf8_prev( const char* s, i32 i )
{
    if ( i <= 0 )
        return 0;
    --i;
    while ( i > 0 && utf8_is_cont( ( u8 )s[ i ] ) )
        --i;
    return i;
}

// Codepoints in the byte range [0, len) -- counts sequence starts, so malformed bytes
// each count as one (matching what utf8_decode would yield).
ORB_INLINE i32
utf8_count( const char* s, i32 len )
{
    i32 n = 0;
    for ( i32 i = 0; i < len; ++i )
        n += !utf8_is_cont( ( u8 )s[ i ] );
    return n;
}

/*==============================================================================================
    UTF-16 surrogate helpers  (Win32 WM_CHAR delivers UTF-16 code units)
==============================================================================================*/

// High (leading) surrogate, 0xD800..0xDBFF.
ORB_INLINE b32
utf16_is_high( u32 unit )
{
    return ( unit & 0xFC00u ) == 0xD800u;
}

// Low (trailing) surrogate, 0xDC00..0xDFFF.
ORB_INLINE b32
utf16_is_low( u32 unit )
{
    return ( unit & 0xFC00u ) == 0xDC00u;
}

// Combine a surrogate pair into a supplementary-plane codepoint. Inputs must satisfy
// utf16_is_high / utf16_is_low.
ORB_INLINE u32
utf16_pair_to_cp( u32 hi, u32 lo )
{
    return 0x10000u + ( ( hi - 0xD800u ) << 10 ) + ( lo - 0xDC00u );
}

// clang-format on
/*============================================================================================*/
#endif    // UTF8_H
