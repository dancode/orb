/*==============================================================================================

    char.h -- MASCII character classification and conversion.

        Branchless where possible. Locale-independent (ASCII only).

==============================================================================================*/
#ifndef CHAR_H
#define CHAR_H

/*==============================================================================================
    classification  (all return b32: 1 = true, 0 = false)
==============================================================================================*/

// Letter (a-z, A-Z)
static inline b32
char_is_alpha( char c )
{
    return ( unsigned )( ( c | 0x20 ) - 'a' ) < 26u;
}

// Decimal digit (0-9)
static inline b32
char_is_digit( char c )
{
    return ( unsigned )( c - '0' ) < 10u;
}

// Hexadecimal digit (0-9, a-f, A-F)
static inline b32
char_is_hex( char c )
{
    return char_is_digit( c ) || ( unsigned )( ( c | 0x20 ) - 'a' ) < 6u;
}

// Alphanumeric
static inline b32
char_is_alnum( char c )
{
    return char_is_alpha( c ) || char_is_digit( c );
}

// Uppercase letter
static inline b32
char_is_upper( char c )
{
    return ( unsigned )( c - 'A' ) < 26u;
}

// Lowercase letter
static inline b32
char_is_lower( char c )
{
    return ( unsigned )( c - 'a' ) < 26u;
}

// ASCII whitespace (space, tab, newline, carriage return, form feed, vertical tab)
static inline b32
char_is_space( char c )
{
    return c == ' ' || ( unsigned )( c - '\t' ) < 5u;
}

// Printable ASCII (0x20 to 0x7E)
static inline b32
char_is_print( char c )
{
    return ( unsigned )( c - 0x20 ) < 0x5Fu;
}

// ASCII control character (< 0x20 or == 0x7F)
static inline b32
char_is_ctrl( char c )
{
    return ( unsigned char )c < 0x20u || c == 0x7F;
}

/*==============================================================================================
    conversion
==============================================================================================*/

// Convert to uppercase (no-op if not lowercase letter).
static inline char
char_to_upper( char c )
{
    return char_is_lower( c ) ? ( char )( c - 0x20 ) : c;
}

// Convert to lowercase (no-op if not uppercase letter).
static inline char
char_to_lower( char c )
{
    return char_is_upper( c ) ? ( char )( c + 0x20 ) : c;
}

// Decimal digit to integer value (returns -1 if not a digit).
static inline i32
char_digit_value( char c )
{
    return char_is_digit( c ) ? ( i32 )( c - '0' ) : -1;
}

// Hex digit to integer value 0-15 (returns -1 if not a hex digit).
static inline i32
char_hex_value( char c )
{
    if ( char_is_digit( c ) )
        return ( i32 )( c - '0' );
    if ( ( unsigned )( ( c | 0x20 ) - 'a' ) < 6 )
        return ( i32 )( ( c | 0x20 ) - 'a' + 10 );
    return -1;
}

// Integer value 0-15 to hex character ('0'-'9', 'a'-'f').
static inline char
char_hex_digit( i32 v )
{
    return ( char )( v < 10 ? '0' + v : 'a' + v - 10 );
}

/*============================================================================================*/
#endif    // CHAR_H