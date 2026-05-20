/*==============================================================================================

    str_arena.c -- Linear string arena implementation.

==============================================================================================*/
#include "orb.h"
#include "str_arena.h"
#include <string.h>
#include <stdio.h>

/*==============================================================================================
    Marks
==============================================================================================*/

i32
str_arena_mark( const str_arena_t* a )
{
    return a->pos;
}

void
str_arena_pop( str_arena_t* a, i32 mark )
{
    if ( mark < 0 )
        mark = 0;
    if ( mark > a->pos )
        return;  /* advancing forward would corrupt; only retreat is valid */
    a->pos = mark;
}

void
str_arena_clear( str_arena_t* a )
{
    a->pos = 0;
}

/*==============================================================================================
    Push
==============================================================================================*/

char*
str_arena_push_raw( str_arena_t* a, i32 n )
{
    if ( n <= 0 || a->pos + n > a->cap )
        return NULL;
    char* p = a->buf + a->pos;
    a->pos += n;
    return p;
}

str_t
str_arena_push_str( str_arena_t* a, str_t src )
{
    if ( src.len == 0 )
        return STR_EMPTY;

    /* +1 for null terminator so the result is directly usable as a C string. */
    char* p = str_arena_push_raw( a, src.len + 1 );
    if ( !p )
        return STR_EMPTY;

    memcpy( p, src.ptr, ( usize )src.len );
    p[ src.len ] = '\0';
    return str_from_ptr_len( p, src.len );
}

str_t
str_arena_push_cstr( str_arena_t* a, const char* s )
{
    return str_arena_push_str( a, str_from_cstr( s ) );
}

str_t
str_arena_vpush_fmt( str_arena_t* a, const char* fmt, va_list args )
{
    if ( !fmt )
        return STR_EMPTY;

    i32 avail = a->cap - a->pos;
    if ( avail <= 1 )
        return STR_EMPTY;  /* need at least one byte of content + null */

    /* Write directly into the arena at the current cursor position.
       vsnprintf null-terminates within avail bytes; we check if it fit. */
    char* dest = a->buf + a->pos;
    int   n    = vsnprintf( dest, ( usize )avail, fmt, args );

    if ( n <= 0 || n >= avail )
        return STR_EMPTY;  /* error or truncation; cursor is not advanced */

    /* Advance past the content and the null terminator so the byte is owned. */
    a->pos += n + 1;
    return str_from_ptr_len( dest, n );
}

str_t
str_arena_push_fmt( str_arena_t* a, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    str_t result = str_arena_vpush_fmt( a, fmt, args );
    va_end( args );
    return result;
}

/*==============================================================================================
    strbuf_t Integration
==============================================================================================*/

strbuf_t
str_arena_strbuf( str_arena_t* a, i32 cap )
{
    if ( cap <= 0 )
        return ( strbuf_t ){ 0 };

    char* p = str_arena_push_raw( a, cap );
    if ( !p )
        return ( strbuf_t ){ 0 };  /* caller checks strbuf_ok to detect this */

    /* Ensure the buffer starts as a valid empty strbuf. */
    p[ 0 ] = '\0';
    return strbuf_from_ptr_cap( p, cap );
}

void
str_arena_trim_strbuf( str_arena_t* a, const strbuf_t* buf )
{
    /* Compute where this strbuf's content ends (+1 for null terminator). */
    i32 strbuf_start = ( i32 )( buf->ptr - a->buf );
    i32 new_pos      = strbuf_start + buf->len + 1;

    /* Only retreat; never advance the cursor past its current position. */
    if ( new_pos > 0 && new_pos < a->pos )
        a->pos = new_pos;
}

/*============================================================================================*/
