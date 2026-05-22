/*==============================================================================================

    build_tool_str.c -- Implementation of s_t and sbuf_t for the build tool.

    Standalone translation unit. No orb.h, no engine dependencies.

==============================================================================================*/
// clang-format off

#include "build_str.h"

#include <string.h>  /* memcpy, memcmp, strlen */
#include <stdio.h>   /* vsnprintf */
#include <ctype.h>   /* tolower, isspace */

/*==============================================================================================
    s_t
==============================================================================================*/

s_t
s_from_cstr( const char* s )
{
    if ( !s ) return ( s_t ){ 0 };
    return s_from_ptr_len( s, ( int )strlen( s ) );
}

s_t
s_sub( s_t s, int start, int end )
{
    if ( start < 0 ) start = 0;
    if ( end > s.len ) end = s.len;
    if ( start >= end ) return ( s_t ){ 0 };
    return s_from_ptr_len( s.ptr + start, end - start );
}

s_t
s_prefix( s_t s, int n )
{
    return s_sub( s, 0, n );
}

s_t
s_suffix( s_t s, int n )
{
    return s_sub( s, s.len - n, s.len );
}

s_t
s_trim( s_t s )
{
    int start = 0;
    while ( start < s.len && isspace( ( unsigned char )s.ptr[ start ] ) ) ++start;
    int end = s.len;
    while ( end > start && isspace( ( unsigned char )s.ptr[ end - 1 ] ) ) --end;
    return s_sub( s, start, end );
}

/*--- Comparison ---*/

bool
s_equal( s_t a, s_t b )
{
    return a.len == b.len && memcmp( a.ptr, b.ptr, ( size_t )a.len ) == 0;
}

bool
s_equal_nocase( s_t a, s_t b )
{
    if ( a.len != b.len ) return false;
    for ( int i = 0; i < a.len; ++i )
        if ( tolower( ( unsigned char )a.ptr[ i ] ) != tolower( ( unsigned char )b.ptr[ i ] ) )
            return false;
    return true;
}

/*--- Search ---*/

int
s_find_char( s_t s, char c )
{
    for ( int i = 0; i < s.len; ++i )
        if ( s.ptr[ i ] == c ) return i;
    return S_NOT_FOUND;
}

int
s_rfind_char( s_t s, char c )
{
    for ( int i = s.len - 1; i >= 0; --i )
        if ( s.ptr[ i ] == c ) return i;
    return S_NOT_FOUND;
}

bool
s_starts_with( s_t s, s_t prefix )
{
    if ( prefix.len > s.len ) return false;
    return memcmp( s.ptr, prefix.ptr, ( size_t )prefix.len ) == 0;
}

bool
s_ends_with( s_t s, s_t suffix )
{
    if ( suffix.len > s.len ) return false;
    return memcmp( s.ptr + s.len - suffix.len, suffix.ptr, ( size_t )suffix.len ) == 0;
}

bool
s_contains( s_t s, s_t needle )
{
    if ( needle.len == 0 ) return true;
    if ( needle.len > s.len ) return false;
    int limit = s.len - needle.len;
    for ( int i = 0; i <= limit; ++i )
        if ( memcmp( s.ptr + i, needle.ptr, ( size_t )needle.len ) == 0 ) return true;
    return false;
}

bool
s_next_token( s_t* remainder, s_t* token )
{
    // Skip leading whitespace.
    int start = 0;
    while ( start < remainder->len && isspace( ( unsigned char )remainder->ptr[ start ] ) )
        ++start;

    if ( start == remainder->len )
    {
        *token     = ( s_t ){ 0 };
        *remainder = ( s_t ){ 0 };
        return false;
    }

    // Scan to end of token.
    int end = start;
    while ( end < remainder->len && !isspace( ( unsigned char )remainder->ptr[ end ] ) )
        ++end;

    *token     = s_from_ptr_len( remainder->ptr + start, end - start );
    *remainder = s_from_ptr_len( remainder->ptr + end, remainder->len - end );
    return true;
}

bool
s_split_once( s_t s, char sep, s_t* left, s_t* right )
{
    int i = s_find_char( s, sep );
    if ( i == S_NOT_FOUND )
    {
        *left  = s;
        *right = ( s_t ){ 0 };
        return false;
    }
    *left  = s_prefix( s, i );
    *right = s_from_ptr_len( s.ptr + i + 1, s.len - i - 1 );
    return true;
}

/*--- C string interop ---*/

int
s_to_cstr( s_t s, char* buf, int cap )
{
    if ( cap <= 0 ) return 0;
    int n = s.len < cap - 1 ? s.len : cap - 1;
    if ( n > 0 ) memcpy( buf, s.ptr, ( size_t )n );
    buf[ n ] = '\0';
    return n;
}

/*==============================================================================================
    sbuf_t
==============================================================================================*/

void
sbuf_clear( sbuf_t* sb )
{
    // Preserve overflow flag so a clear does not silently hide a prior overflow.
    sb->len    = 0;
    sb->ptr[0] = '\0';
}

void
sbuf_zero( sbuf_t* sb )
{
    // Full reset including overflow flag -- use to reclaim an overflowed buffer.
    memset( sb->ptr, 0, ( size_t )sbuf_cap( *sb ) );
    sb->len = 0;
    sb->cap = sbuf_cap( *sb );
}

/*--- Write ---*/

bool
sbuf_set( sbuf_t* sb, s_t src )
{
    sb->len    = 0;
    sb->cap    = sbuf_cap( *sb );
    sb->ptr[0] = '\0';
    return sbuf_append( sb, src );
}

bool
sbuf_set_cstr( sbuf_t* sb, const char* s )
{
    return sbuf_set( sb, s_from_cstr( s ) );
}

/*--- Append ---*/

bool
sbuf_append_char( sbuf_t* sb, char c )
{
    if ( sbuf_overflowed( *sb ) ) return false;
    if ( sbuf_remaining( *sb ) < 1 )
    {
        sb->cap |= ( int )SBUF_OVF_BIT;
        return false;
    }
    sb->ptr[ sb->len++ ] = c;
    sb->ptr[ sb->len   ] = '\0';
    return true;
}

bool
sbuf_append( sbuf_t* sb, s_t src )
{
    if ( sbuf_overflowed( *sb ) ) return false;
    if ( src.len == 0 ) return true;

    int remaining = sbuf_remaining( *sb );
    if ( src.len > remaining )
    {
        // Partial write up to capacity, then flag overflow.
        if ( remaining > 0 ) memcpy( sb->ptr + sb->len, src.ptr, ( size_t )remaining );
        sb->len += remaining;
        sb->ptr[ sb->len ] = '\0';
        sb->cap |= ( int )SBUF_OVF_BIT;
        return false;
    }

    memcpy( sb->ptr + sb->len, src.ptr, ( size_t )src.len );
    sb->len += src.len;
    sb->ptr[ sb->len ] = '\0';
    return true;
}

bool
sbuf_append_cstr( sbuf_t* sb, const char* s )
{
    return sbuf_append( sb, s_from_cstr( s ) );
}

bool
sbuf_vappendf( sbuf_t* sb, const char* fmt, va_list args )
{
    if ( sbuf_overflowed( *sb ) ) return false;

    int   remaining = sbuf_remaining( *sb );
    char* dst       = sb->ptr + sb->len;

    // vsnprintf returns the length it WOULD have written, not the truncated length.
    int written = vsnprintf( dst, ( size_t )( remaining + 1 ), fmt, args );
    if ( written < 0 )
    {
        sb->cap |= ( int )SBUF_OVF_BIT;
        return false;
    }

    if ( written > remaining )
    {
        sb->len += remaining;
        sb->ptr[ sb->len ] = '\0';
        sb->cap |= ( int )SBUF_OVF_BIT;
        return false;
    }

    sb->len += written;
    return true;
}

bool
sbuf_appendf( sbuf_t* sb, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    bool ok = sbuf_vappendf( sb, fmt, args );
    va_end( args );
    return ok;
}

/*--- Format (overwrite from start) ---*/

bool
sbuf_fmt( sbuf_t* sb, const char* fmt, ... )
{
    sb->len    = 0;
    sb->cap    = sbuf_cap( *sb );
    sb->ptr[0] = '\0';

    va_list args;
    va_start( args, fmt );
    bool ok = sbuf_vappendf( sb, fmt, args );
    va_end( args );
    return ok;
}

/*--- Editing ---*/

void
sbuf_chop( sbuf_t* sb, int new_len )
{
    if ( new_len >= sb->len ) return;
    if ( new_len < 0 ) new_len = 0;
    sb->len            = new_len;
    sb->ptr[ new_len ] = '\0';
}

void
sbuf_strip_trailing( sbuf_t* sb, char c )
{
    while ( sb->len > 0 && sb->ptr[ sb->len - 1 ] == c )
    {
        --sb->len;
        sb->ptr[ sb->len ] = '\0';
    }
}

/*==============================================================================================
    Path helpers
==============================================================================================*/

bool
sbuf_path_append( sbuf_t* sb, s_t segment )
{
    // Insert separator only when non-empty and not already ending with one.
    if ( sb->len > 0 )
    {
        char last = sb->ptr[ sb->len - 1 ];
        if ( last != '\\' && last != '/' )
            if ( !sbuf_append_char( sb, '\\' ) ) return false;
    }
    return sbuf_append( sb, segment );
}

/*============================================================================================*/
