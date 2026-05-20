/*==============================================================================================

    char.h -- MASCII character classification and conversion.

        Branchless where possible. Locale-independent (ASCII only).

==============================================================================================*/
#ifndef CHAR_H
#define CHAR_H

// clang-format off
/*==============================================================================================
    classification  (all return b32: 1 = true, 0 = false)
==============================================================================================*/

// Letter (a-z, A-Z)
ORB_INLINE b32
char_is_alpha( char c )
{
    return ( u32 )( ( c | 0x20 ) - 'a' ) < 26u;
}

// Decimal digit (0-9)
ORB_INLINE b32
char_is_digit( char c )
{
    return ( u32 )( c - '0' ) < 10u;
}

// Hexadecimal digit (0-9, a-f, A-F)
ORB_INLINE b32
char_is_hex( char c )
{
    return char_is_digit( c ) || ( u32 )( ( c | 0x20 ) - 'a' ) < 6u;
}

// Alphanumeric
ORB_INLINE b32
char_is_alnum( char c )
{
    return char_is_alpha( c ) || char_is_digit( c );
}

// Uppercase letter
ORB_INLINE b32
char_is_upper( char c )
{
    return ( u32 )( c - 'A' ) < 26u;
}

// Lowercase letter
ORB_INLINE b32
char_is_lower( char c )
{
    return ( u32 )( c - 'a' ) < 26u;
}

// ASCII whitespace (space, tab, newline, carriage return, form feed, vertical tab)
ORB_INLINE b32
char_is_space( char c )
{
    return c == ' ' || ( u32 )( c - '\t' ) < 5u;
}

// Printable ASCII (0x20 to 0x7E)
ORB_INLINE b32
char_is_print( char c )
{
    return ( u32 )( c - 0x20 ) < 0x5Fu;
}

// ASCII control character (< 0x20 or == 0x7F)
ORB_INLINE b32
char_is_ctrl( char c )
{
    return ( u8 )c < 0x20u || c == 0x7F;
}

/*==============================================================================================
    conversion
==============================================================================================*/

// Convert to uppercase (no-op if not lowercase letter).
ORB_INLINE char
char_to_upper( char c )
{
    return ( char )( c & ~( char_is_lower( c ) << 5 ) );
}

// Convert to lowercase (no-op if not uppercase letter).
ORB_INLINE char
char_to_lower( char c )
{
    return ( char )( c | ( char_is_upper( c ) << 5 ) );
}

// Decimal digit to integer value (returns -1 if not a digit).
ORB_INLINE i32
char_digit_value( char c )
{
    return char_is_digit( c ) ? ( i32 )( c - '0' ) : -1;
}

// Hex digit to integer value 0-15 (returns -1 if not a hex digit).
ORB_INLINE i32
char_hex_value( char c )
{
    if ( char_is_digit( c ) )
        return ( i32 )( c - '0' );
    if ( ( u32 )( ( c | 0x20 ) - 'a' ) < 6 )
        return ( i32 )( ( c | 0x20 ) - 'a' + 10 );
    return -1;
}

// Integer value 0-15 to hex character ('0'-'9', 'a'-'f').
ORB_INLINE char
char_hex_digit( i32 v )
{
    return ( char )( v < 10 ? '0' + v : 'a' + v - 10 );
}

// clang-format on
/*============================================================================================*/
#endif    // CHAR_H