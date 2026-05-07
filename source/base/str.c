/*==============================================================================================

    str.h -- C string operations implementation.

        No global state. No dynamic allocation.

==============================================================================================*/

#include "str.h"

/*==============================================================================================
    Length / Query
==============================================================================================*/

usize
str_len( const char* s )
{
    if ( !s )
        return 0;
    const char* p = s;
    while ( *p ) ++p;
    return ( usize )( p - s );
}

b32
str_empty( const char* s )
{
    return !s || s[ 0 ] == '\0';
}

/*==============================================================================================
    Copy / Concat
==============================================================================================*/

usize
str_copy( char* dst, usize dst_cap, const char* src )
{
    usize src_len = str_len( src );
    if ( dst_cap == 0 )
        return src_len;

    usize copy = src_len < ( dst_cap - 1 ) ? src_len : ( dst_cap - 1 );
    // Manual copy (avoids pulling in memcpy separately here;
    // in practice, base.h is included and mem_copy is available)
    for ( usize i = 0; i < copy; ++i ) dst[ i ] = src[ i ];
    dst[ copy ] = '\0';
    return src_len;
}

usize
str_append( char* dst, usize dst_cap, const char* src )
{
    usize dst_len = str_len( dst );
    usize src_len = str_len( src );
    if ( dst_cap == 0 )
        return dst_len + src_len;

    usize space = ( dst_len < dst_cap ) ? ( dst_cap - dst_len - 1 ) : 0;
    usize copy  = src_len < space ? src_len : space;
    for ( usize i = 0; i < copy; ++i ) dst[ dst_len + i ] = src[ i ];
    dst[ dst_len + copy ] = '\0';
    return dst_len + src_len;
}

/*==============================================================================================
    Comparison
==============================================================================================*/

b32
str_equal( const char* a, const char* b )
{
    if ( a == b )
        return 1;
    if ( !a || !b )
        return 0;
    while ( *a && *a == *b )
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

i32
str_cmp( const char* a, const char* b )
{
    if ( !a && !b )
        return 0;
    if ( !a )
        return -1;
    if ( !b )
        return 1;
    while ( *a && *a == *b )
    {
        ++a;
        ++b;
    }
    return ( unsigned char )*a - ( unsigned char )*b;
}

i32
str_ncmp( const char* a, const char* b, usize n )
{
    for ( usize i = 0; i < n; ++i )
    {
        unsigned char ca = ( unsigned char )a[ i ];
        unsigned char cb = ( unsigned char )b[ i ];
        if ( ca != cb )
            return ( i32 )ca - ( i32 )cb;
        if ( ca == 0 )
            return 0;    // both terminated
    }
    return 0;
}

b32
str_equal_nocase( const char* a, const char* b )
{
    if ( a == b )
        return 1;
    if ( !a || !b )
        return 0;
    // ASCII-only tolower
    while ( *a && *b )
    {
        char ca = *a | 0x20;    // works for A-Z only
        char cb = *b | 0x20;
        // Only apply lowercasing to letters; check letter range
        char la = ( *a >= 'A' && *a <= 'Z' ) ? ca : *a;
        char lb = ( *b >= 'A' && *b <= 'Z' ) ? cb : *b;
        if ( la != lb )
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

/*==============================================================================================
    Search
==============================================================================================*/

const char*
str_find_char( const char* s, char c )
{
    if ( !s )
        return 0;
    for ( ; *s; ++s )
    {
        if ( *s == c )
            return s;
    }
    return 0;
}

const char*
str_rfind_char( const char* s, char c )
{
    if ( !s )
        return 0;
    const char* last = 0;
    for ( ; *s; ++s )
    {
        if ( *s == c )
            last = s;
    }
    return last;
}

const char*
str_find_sub( const char* haystack, const char* needle )
{
    if ( !haystack || !needle )
        return 0;
    if ( !needle[ 0 ] )
        return haystack;

    for ( ; *haystack; ++haystack )
    {
        const char* h = haystack;
        const char* n = needle;
        while ( *h && *n && *h == *n )
        {
            ++h;
            ++n;
        }
        if ( !*n )
            return haystack;    // full needle matched
    }
    return 0;
}

b32
str_starts_with( const char* s, const char* prefix )
{
    if ( !s || !prefix )
        return 0;
    while ( *prefix )
    {
        if ( *s++ != *prefix++ )
            return 0;
    }
    return 1;
}

b32
str_ends_with( const char* s, const char* suffix )
{
    if ( !s || !suffix )
        return 0;
    usize s_len = str_len( s );
    usize p_len = str_len( suffix );
    if ( p_len > s_len )
        return 0;
    return str_ncmp( s + s_len - p_len, suffix, p_len ) == 0;
}

/*==============================================================================================
    Hashing
==============================================================================================*/

// FNV-1a 64-bit constants
#define FNV1A_OFFSET 14695981039346656037ULL
#define FNV1A_PRIME  1099511628211ULL

u64
str_hash( const char* s )
{
    u64 h = FNV1A_OFFSET;
    if ( !s )
        return h;
    while ( *s )
    {
        h ^= ( u64 )( unsigned char )*s++;
        h *= FNV1A_PRIME;
    }
    return h;
}

u64
str_hash_n( const char* s, usize n )
{
    u64 h = FNV1A_OFFSET;
    for ( usize i = 0; i < n; ++i )
    {
        h ^= ( u64 )( unsigned char )s[ i ];
        h *= FNV1A_PRIME;
    }
    return h;
}

/*==============================================================================================
    Parsing Helpers
==============================================================================================*/

usize
str_parse_i64( const char* s, long long* out )
{
    if ( !s || !out )
        return 0;
    const char* p    = s;
    long long   sign = 1;
    if ( *p == '-' )
    {
        sign = -1;
        ++p;
    }
    else if ( *p == '+' )
    {
        ++p;
    }

    if ( *p < '0' || *p > '9' )
        return 0;

    long long val = 0;
    while ( *p >= '0' && *p <= '9' )
    {
        val = val * 10 + ( *p - '0' );
        ++p;
    }
    *out = sign * val;
    return ( usize )( p - s );
}

usize
str_parse_u64( const char* s, unsigned long long* out )
{
    if ( !s || !out )
        return 0;
    const char* p = s;
    if ( *p < '0' || *p > '9' )
        return 0;

    unsigned long long val = 0;
    while ( *p >= '0' && *p <= '9' )
    {
        val = val * 10 + ( unsigned long long )( *p - '0' );
        ++p;
    }
    *out = val;
    return ( usize )( p - s );
}

/*============================================================================================*/