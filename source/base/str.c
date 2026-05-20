// clang-format off
/*==============================================================================================

    str_new.c -- Non-owning string view implementation.

==============================================================================================*/
#include "orb.h"
#include "str.h"
#include <string.h>

/* Global empty string sentinel. ptr points to a real empty string so callers can safely
   call str_to_cstr on it without a NULL check at every site. */
const str_t STR_EMPTY = { .ptr = "", .len = 0 };

/*==============================================================================================
    Internal Helpers
==============================================================================================*/

/* Inline ASCII lowercase -- avoids local dependencies and is branch-predictable. */
static ORB_INLINE char
_char_lower( char c )
{
    return ( c >= 'A' && c <= 'Z' ) ? ( char )( c + 32 ) : c;
}

/* ASCII whitespace: space, tab, newline, carriage-return, vertical-tab, form-feed. */
static ORB_INLINE b32
_char_is_space( char c )
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/*==============================================================================================
    Construction
==============================================================================================*/

str_t
str_from_cstr( const char* s )
{
    if ( !s )
        return STR_EMPTY;

    /* Walk once to measure length; this is the only strlen call in the str_t lifecycle. */
    i32 n = 0; while ( s[ n ] ) n++;

    return str_from_ptr_len( s, n );
}

str_t
str_sub( str_t s, i32 start, i32 end )
{
    /* Clamp to valid range so callers never need to guard against edge cases. */
    if ( start < 0 ) start = 0;
    if ( end > s.len ) end = s.len;
    if ( start >= end ) return STR_EMPTY;

    return str_from_ptr_len( s.ptr + start, end - start );
}

str_t
str_prefix( str_t s, i32 n )
{
    return str_sub( s, 0, n );
}

str_t
str_suffix( str_t s, i32 n )
{
    return str_sub( s, s.len - n, s.len );
}

str_t
str_trim_left( str_t s )
{
    /* Advance ptr and shrink len symmetrically -- result is still a valid view. */
    while ( s.len > 0 && _char_is_space( s.ptr[ 0 ] ) )
    {
        s.ptr++;
        s.len--;
    }
    return s;
}

str_t
str_trim_right( str_t s )
{
    /* Reduce len only -- ptr is unchanged, backing memory is untouched. */
    while ( s.len > 0 && _char_is_space( s.ptr[ s.len - 1 ] ) )
        s.len--;
    return s;
}

str_t
str_trim( str_t s )
{
    return str_trim_right( str_trim_left( s ) );
}

/*==============================================================================================
    Comparison
==============================================================================================*/

b32
str_equal( str_t a, str_t b )
{
    if ( a.len != b.len )
        return 0;

    /* Pointer equality fast path avoids memcmp when comparing a string to itself. */
    if ( a.ptr == b.ptr )
        return 1;

    return memcmp( a.ptr, b.ptr, ( usize )a.len ) == 0;
}

b32
str_equal_nocase( str_t a, str_t b )
{
    if ( a.len != b.len )
        return 0;

    for ( i32 i = 0; i < a.len; i++ )
    {
        if ( _char_lower( a.ptr[ i ] ) != _char_lower( b.ptr[ i ] ) )
            return 0;
    }
    return 1;
}

i32
str_cmp( str_t a, str_t b )
{
    i32 min_len = a.len < b.len ? a.len : b.len;
    i32 r       = memcmp( a.ptr, b.ptr, ( usize )min_len );
    if ( r != 0 )
        return r;

    /* Equal prefix: shorter string sorts first. */
    return a.len - b.len;
}

/*==============================================================================================
    Search
==============================================================================================*/

i32
str_find_char( str_t s, char c )
{
    for ( i32 i = 0; i < s.len; i++ )
    {
        if ( s.ptr[ i ] == c )
            return i;
    }
    return STR_NOT_FOUND;
}

i32
str_rfind_char( str_t s, char c )
{
    for ( i32 i = s.len - 1; i >= 0; i-- )
    {
        if ( s.ptr[ i ] == c )
            return i;
    }
    return STR_NOT_FOUND;
}

i32
str_find( str_t haystack, str_t needle )
{
    if ( needle.len == 0 )
        return 0;  /* empty needle matches at offset 0 (same as strstr convention) */

    if ( needle.len > haystack.len )
        return STR_NOT_FOUND;

    /* memcmp at each candidate -- no extra state, works well for short needles. */
    i32 limit = haystack.len - needle.len;
    for ( i32 i = 0; i <= limit; i++ )
    {
        if ( memcmp( haystack.ptr + i, needle.ptr, ( usize )needle.len ) == 0 )
            return i;
    }
    return STR_NOT_FOUND;
}

b32
str_starts_with( str_t s, str_t prefix )
{
    if ( prefix.len > s.len )
        return 0;
    return memcmp( s.ptr, prefix.ptr, ( usize )prefix.len ) == 0;
}

b32
str_ends_with( str_t s, str_t suffix )
{
    if ( suffix.len > s.len )
        return 0;
    return memcmp( s.ptr + s.len - suffix.len, suffix.ptr, ( usize )suffix.len ) == 0;
}

b32
str_contains( str_t s, str_t needle )
{
    return str_find( s, needle ) != STR_NOT_FOUND;
}

/*==============================================================================================
    Hashing -- FNV-1a
==============================================================================================*/

/* Widely used non-cryptographic hash with excellent distribution for short strings.
   Case-sensitive. Changing any byte changes the hash -- strong avalanche. */

#define FNV32_BASIS 2166136261U
#define FNV32_PRIME 16777619U
#define FNV64_BASIS 14695981039346656037ULL
#define FNV64_PRIME 1099511628211ULL

u32
str_hash32( str_t s )
{
    u32 h = FNV32_BASIS;
    for ( i32 i = 0; i < s.len; i++ )
    {
        h ^= ( u8 )s.ptr[ i ];
        h *= FNV32_PRIME;
    }
    return h;
}

u64
str_hash64( str_t s )
{
    u64 h = FNV64_BASIS;
    for ( i32 i = 0; i < s.len; i++ )
    {
        h ^= ( u64 )( u8 )s.ptr[ i ];
        h *= FNV64_PRIME;
    }
    return h;
}

/*==============================================================================================
    Parsing
==============================================================================================*/

b32
str_to_i32( str_t s, i32* out )
{
    if ( s.len == 0 || !out )
        return 0;

    i32 i = 0, sign = 1, val = 0;

    if ( s.ptr[ i ] == '-' )
    {
        sign = -1;
        i++;
    }
    else if ( s.ptr[ i ] == '+' )
    {
        i++;
    }

    if ( i == s.len )
        return 0;  /* sign only, no digits */

    for ( ; i < s.len; i++ )
    {
        char c = s.ptr[ i ];
        if ( c < '0' || c > '9' )
            return 0;
        val = val * 10 + ( c - '0' );
    }

    *out = val * sign;
    return 1;
}

b32
str_to_i64( str_t s, i64* out )
{
    if ( s.len == 0 || !out )
        return 0;

    i32 i    = 0;
    i64 sign = 1, val = 0;

    if ( s.ptr[ i ] == '-' )
    {
        sign = -1;
        i++;
    }
    else if ( s.ptr[ i ] == '+' )
    {
        i++;
    }

    if ( i == s.len )
        return 0;

    for ( ; i < s.len; i++ )
    {
        char c = s.ptr[ i ];
        if ( c < '0' || c > '9' )
            return 0;
        val = val * 10 + ( i64 )( c - '0' );
    }

    *out = val * sign;
    return 1;
}

b32
str_to_f64( str_t s, f64* out )
{
    if ( s.len == 0 || !out )
        return 0;

    i32 i    = 0;
    f64 sign = 1.0;

    if ( s.ptr[ i ] == '-' )
    {
        sign = -1.0;
        i++;
    }
    else if ( s.ptr[ i ] == '+' )
    {
        i++;
    }

    /* Integer part. */
    f64 val       = 0.0;
    b32 has_digit = 0;
    while ( i < s.len && s.ptr[ i ] >= '0' && s.ptr[ i ] <= '9' )
    {
        val       = val * 10.0 + ( f64 )( s.ptr[ i ] - '0' );
        has_digit = 1;
        i++;
    }

    /* Optional fractional part. */
    if ( i < s.len && s.ptr[ i ] == '.' )
    {
        i++;
        f64 frac = 0.1;
        while ( i < s.len && s.ptr[ i ] >= '0' && s.ptr[ i ] <= '9' )
        {
            val += ( f64 )( s.ptr[ i ] - '0' ) * frac;
            frac *= 0.1;
            has_digit = 1;
            i++;
        }
    }

    if ( !has_digit )
        return 0;

    *out = val * sign;
    return 1;
}

b32
str_to_f32( str_t s, f32* out )
{
    if ( !out )
        return 0;
    f64 v;
    if ( !str_to_f64( s, &v ) )
        return 0;
    *out = ( f32 )v;
    return 1;
}

i32
str_scan_i64( str_t s, i64* out )
{
    /* Prefix parse: consume sign + digit run, stop at first non-digit. */
    if ( s.len == 0 || !out )
        return 0;

    i32 i    = 0;
    i64 sign = 1;

    if ( s.ptr[ i ] == '-' )
    {
        sign = -1;
        i++;
    }
    else if ( s.ptr[ i ] == '+' )
    {
        i++;
    }

    if ( i == s.len || s.ptr[ i ] < '0' || s.ptr[ i ] > '9' )
        return 0;  /* sign only, or non-digit start */

    i64 val = 0;
    while ( i < s.len && s.ptr[ i ] >= '0' && s.ptr[ i ] <= '9' )
    {
        val = val * 10 + ( i64 )( s.ptr[ i ] - '0' );
        i++;
    }

    *out = val * sign;
    return i;
}

i32
str_scan_u64( str_t s, u64* out )
{
    /* Prefix parse: consume digit run, stop at first non-digit. */
    if ( s.len == 0 || !out || s.ptr[ 0 ] < '0' || s.ptr[ 0 ] > '9' )
        return 0;

    i32 i   = 0;
    u64 val = 0;
    while ( i < s.len && s.ptr[ i ] >= '0' && s.ptr[ i ] <= '9' )
    {
        val = val * 10 + ( u64 )( s.ptr[ i ] - '0' );
        i++;
    }

    *out = val;
    return i;
}

/*==============================================================================================
    C String Interop
==============================================================================================*/

i32
str_to_cstr( str_t s, char* buf, i32 cap )
{
    if ( !buf || cap <= 0 )
        return 0;

    /* Copy at most cap-1 bytes so there is always room for the null terminator. */
    i32 copy = s.len < ( cap - 1 ) ? s.len : ( cap - 1 );
    if ( copy > 0 )
        memcpy( buf, s.ptr, ( usize )copy );
    buf[ copy ] = '\0';
    return copy;
}

/*============================================================================================*/
// clang-format on