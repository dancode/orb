/*==============================================================================================

    build_tool_str.c -- Implementation of str_t and strbuf_t for the build tool.

    Standalone translation unit. No orb.h, no engine dependencies.
    Include this file (or add it to the unity build) before any module that uses
    build_tool_str.h.

==============================================================================================*/
// clang-format off

#include "build_tool_str.h"

#include <string.h>  /* memcpy, memcmp, strlen */
#include <stdio.h>   /* vsnprintf */
#include <ctype.h>   /* tolower, isspace */

/*==============================================================================================
    str_t
==============================================================================================*/

str_t
str_from_cstr( const char* s )
{
    if ( !s ) return ( str_t ){ 0 };
    return str_from_ptr_len( s, ( int )strlen( s ) );
}

str_t
str_sub( str_t s, int start, int end )
{
    // Clamp both bounds before computing the view.
    if ( start < 0 ) start = 0;
    if ( end > s.len ) end = s.len;
    if ( start >= end ) return ( str_t ){ 0 };
    return str_from_ptr_len( s.ptr + start, end - start );
}

str_t
str_prefix( str_t s, int n )
{
    return str_sub( s, 0, n );
}

str_t
str_suffix( str_t s, int n )
{
    return str_sub( s, s.len - n, s.len );
}

str_t
str_trim( str_t s )
{
    // Advance start past leading whitespace.
    int start = 0;
    while ( start < s.len && isspace( ( unsigned char )s.ptr[ start ] ) ) ++start;

    // Walk end back past trailing whitespace.
    int end = s.len;
    while ( end > start && isspace( ( unsigned char )s.ptr[ end - 1 ] ) ) --end;

    return str_sub( s, start, end );
}

/*--- Comparison ---*/

bool
str_equal( str_t a, str_t b )
{
    return a.len == b.len && memcmp( a.ptr, b.ptr, ( size_t )a.len ) == 0;
}

bool
str_equal_nocase( str_t a, str_t b )
{
    if ( a.len != b.len ) return false;
    for ( int i = 0; i < a.len; ++i )
        if ( tolower( ( unsigned char )a.ptr[ i ] ) != tolower( ( unsigned char )b.ptr[ i ] ) )
            return false;
    return true;
}

/*--- Search ---*/

int
str_find_char( str_t s, char c )
{
    for ( int i = 0; i < s.len; ++i )
        if ( s.ptr[ i ] == c ) return i;
    return STR_NOT_FOUND;
}

int
str_rfind_char( str_t s, char c )
{
    for ( int i = s.len - 1; i >= 0; --i )
        if ( s.ptr[ i ] == c ) return i;
    return STR_NOT_FOUND;
}

bool
str_starts_with( str_t s, str_t prefix )
{
    if ( prefix.len > s.len ) return false;
    return memcmp( s.ptr, prefix.ptr, ( size_t )prefix.len ) == 0;
}

bool
str_ends_with( str_t s, str_t suffix )
{
    if ( suffix.len > s.len ) return false;
    return memcmp( s.ptr + s.len - suffix.len, suffix.ptr, ( size_t )suffix.len ) == 0;
}

bool
str_contains( str_t s, str_t needle )
{
    if ( needle.len == 0 ) return true;
    if ( needle.len > s.len ) return false;

    int limit = s.len - needle.len;
    for ( int i = 0; i <= limit; ++i )
        if ( memcmp( s.ptr + i, needle.ptr, ( size_t )needle.len ) == 0 ) return true;
    return false;
}

bool
str_next_token( str_t* remainder, str_t* token )
{
    // Skip leading whitespace.
    int start = 0;
    while ( start < remainder->len && isspace( ( unsigned char )remainder->ptr[ start ] ) )
        ++start;

    if ( start == remainder->len )
    {
        *token     = ( str_t ){ 0 };
        *remainder = ( str_t ){ 0 };
        return false;
    }

    // Scan to end of token.
    int end = start;
    while ( end < remainder->len && !isspace( ( unsigned char )remainder->ptr[ end ] ) )
        ++end;

    *token     = str_from_ptr_len( remainder->ptr + start, end - start );
    *remainder = str_from_ptr_len( remainder->ptr + end, remainder->len - end );
    return true;
}

bool
str_split_once( str_t s, char sep, str_t* left, str_t* right )
{
    int i = str_find_char( s, sep );
    if ( i == STR_NOT_FOUND )
    {
        *left  = s;
        *right = ( str_t ){ 0 };
        return false;
    }
    *left  = str_prefix( s, i );
    *right = str_from_ptr_len( s.ptr + i + 1, s.len - i - 1 );
    return true;
}

/*--- C string interop ---*/

int
str_to_cstr( str_t s, char* buf, int cap )
{
    if ( cap <= 0 ) return 0;
    int n = s.len < cap - 1 ? s.len : cap - 1;
    if ( n > 0 ) memcpy( buf, s.ptr, ( size_t )n );
    buf[ n ] = '\0';
    return n;
}

/*==============================================================================================
    strbuf_t
==============================================================================================*/

void
strbuf_clear( strbuf_t* sb )
{
    // Preserve overflow flag so a clear does not silently hide a prior overflow.
    sb->len    = 0;
    sb->ptr[0] = '\0';
}

void
strbuf_zero( strbuf_t* sb )
{
    // Full reset including overflow flag -- use to reclaim an overflowed buffer.
    memset( sb->ptr, 0, ( size_t )strbuf_cap( *sb ) );
    sb->len = 0;
    sb->cap = strbuf_cap( *sb ); /* strips overflow bit */
}

/*--- Write ---*/

bool
strbuf_set( strbuf_t* sb, str_t src )
{
    // Clear overflow and reset before writing, so this is always a clean overwrite.
    sb->len = 0;
    sb->cap = strbuf_cap( *sb );
    sb->ptr[0] = '\0';
    return strbuf_append( sb, src );
}

bool
strbuf_set_cstr( strbuf_t* sb, const char* s )
{
    return strbuf_set( sb, str_from_cstr( s ) );
}

/*--- Append ---*/

bool
strbuf_append_char( strbuf_t* sb, char c )
{
    if ( strbuf_overflowed( *sb ) ) return false;
    int remaining = strbuf_remaining( *sb );
    if ( remaining < 1 )
    {
        sb->cap |= ( int )STRBUF_OVF_BIT;
        return false;
    }
    sb->ptr[ sb->len++ ] = c;
    sb->ptr[ sb->len   ] = '\0';
    return true;
}

bool
strbuf_append( strbuf_t* sb, str_t src )
{
    if ( strbuf_overflowed( *sb ) ) return false;
    if ( src.len == 0 ) return true;

    int remaining = strbuf_remaining( *sb );
    if ( src.len > remaining )
    {
        // Partial write up to capacity, then flag overflow.
        if ( remaining > 0 ) memcpy( sb->ptr + sb->len, src.ptr, ( size_t )remaining );
        sb->len += remaining;
        sb->ptr[ sb->len ] = '\0';
        sb->cap |= ( int )STRBUF_OVF_BIT;
        return false;
    }

    memcpy( sb->ptr + sb->len, src.ptr, ( size_t )src.len );
    sb->len += src.len;
    sb->ptr[ sb->len ] = '\0';
    return true;
}

bool
strbuf_append_cstr( strbuf_t* sb, const char* s )
{
    return strbuf_append( sb, str_from_cstr( s ) );
}

bool
strbuf_vappendf( strbuf_t* sb, const char* fmt, va_list args )
{
    if ( strbuf_overflowed( *sb ) ) return false;

    int   remaining = strbuf_remaining( *sb );
    char* dst       = sb->ptr + sb->len;

    // vsnprintf returns the length it WOULD have written, not the truncated length.
    // Use that to detect overflow without a second call.
    int written = vsnprintf( dst, ( size_t )( remaining + 1 ), fmt, args );
    if ( written < 0 )
    {
        // Encoding error -- treat as overflow so caller can detect.
        sb->cap |= ( int )STRBUF_OVF_BIT;
        return false;
    }

    if ( written > remaining )
    {
        // Did not fit. The buffer holds as many bytes as remaining; advance len to
        // reflect the partial write and flag overflow.
        sb->len += remaining;
        sb->ptr[ sb->len ] = '\0';
        sb->cap |= ( int )STRBUF_OVF_BIT;
        return false;
    }

    sb->len += written;
    return true;
}

bool
strbuf_appendf( strbuf_t* sb, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    bool ok = strbuf_vappendf( sb, fmt, args );
    va_end( args );
    return ok;
}

/*--- Format (overwrite from start) ---*/

bool
strbuf_fmt( strbuf_t* sb, const char* fmt, ... )
{
    // Full reset including overflow so strbuf_fmt is always a clean write.
    sb->len = 0;
    sb->cap = strbuf_cap( *sb );
    sb->ptr[0] = '\0';

    va_list args;
    va_start( args, fmt );
    bool ok = strbuf_vappendf( sb, fmt, args );
    va_end( args );
    return ok;
}

/*--- Editing ---*/

void
strbuf_chop( strbuf_t* sb, int new_len )
{
    if ( new_len >= sb->len ) return;
    if ( new_len < 0 ) new_len = 0;
    sb->len            = new_len;
    sb->ptr[ new_len ] = '\0';
}

void
strbuf_strip_trailing( strbuf_t* sb, char c )
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
strbuf_path_append( strbuf_t* sb, str_t segment )
{
    // Insert separator only when the buffer is non-empty and doesn't already end with one.
    if ( sb->len > 0 )
    {
        char last = sb->ptr[ sb->len - 1 ];
        if ( last != '\\' && last != '/' )
            if ( !strbuf_append_char( sb, '\\' ) ) return false;
    }
    return strbuf_append( sb, segment );
}

/*============================================================================================*/
