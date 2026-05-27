/*==============================================================================================

    str_buf_new.c -- Mutable string buffer implementation.

==============================================================================================*/
#include "orb.h"
#include "base/str_buf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*==============================================================================================
    Internal Helpers
==============================================================================================*/

/* Set the overflow flag. Called whenever a write cannot fit. The buffer remains in a
   valid, null-terminated, partially-written state. */
static void
_strbuf_set_overflow( strbuf_t* sb )
{
    sb->cap = ( i32 )( ( u32 )sb->cap | STRBUF_OVF_BIT );
}

/* Clear the overflow flag without touching content (used by strbuf_fmt and strbuf_zero). */
static void
_strbuf_clear_overflow( strbuf_t* sb )
{
    sb->cap = ( i32 )( ( u32 )sb->cap & ~STRBUF_OVF_BIT );
}

/*==============================================================================================
    Heap Allocation
==============================================================================================*/

strbuf_t
strbuf_alloc( i32 capacity )
{
    if ( capacity <= 0 )
        return ( strbuf_t ){ 0 };

    char* mem = ( char* )malloc( ( usize )capacity );
    if ( !mem )
        return ( strbuf_t ){ 0 };  /* caller checks strbuf_ok to detect failure */

    mem[ 0 ] = '\0';
    return ( strbuf_t ){ .ptr = mem, .len = 0, .cap = capacity };
}

void
strbuf_free( strbuf_t* sb )
{
    if ( sb && sb->ptr )
    {
        free( sb->ptr );
        *sb = ( strbuf_t ){ 0 };
    }
}

/*==============================================================================================
    Reset
==============================================================================================*/

void
strbuf_clear( strbuf_t* sb )
{
    /* Lightweight reset: only update len and null-terminate.
       Overflow flag is intentionally preserved -- caller must strbuf_zero or strbuf_fmt
       to explicitly acknowledge and clear the error. */
    sb->len = 0;
    if ( sb->ptr && strbuf_cap( *sb ) > 0 )
        sb->ptr[ 0 ] = '\0';
}

void
strbuf_zero( strbuf_t* sb )
{
    /* Full reset: zero every byte and clear the overflow flag.
       Use after overflow when you want to re-use the buffer with a clean slate. */
    _strbuf_clear_overflow( sb );
    sb->len = 0;
    if ( sb->ptr && strbuf_cap( *sb ) > 0 )
        memset( sb->ptr, 0, ( usize )strbuf_cap( *sb ) );
}

/*==============================================================================================
    Write (overwrite from start)
==============================================================================================*/

b32
strbuf_set( strbuf_t* sb, str_t src )
{
    /* Overwrite from the beginning; act as if the buffer were freshly empty. */
    sb->len = 0;
    if ( sb->ptr && strbuf_cap( *sb ) > 0 )
        sb->ptr[ 0 ] = '\0';

    return strbuf_append( sb, src );
}

b32
strbuf_set_cstr( strbuf_t* sb, const char* s )
{
    return strbuf_set( sb, str_from_cstr( s ) );
}

/*==============================================================================================
    Append
==============================================================================================*/

b32
strbuf_append_char( strbuf_t* sb, char c )
{
    /* Reject writes into an already-overflowed buffer so errors accumulate visibly. */
    if ( !strbuf_ok( *sb ) )
        return 0;

    if ( sb->len + 1 >= strbuf_cap( *sb ) )
    {
        _strbuf_set_overflow( sb );
        return 0;
    }

    sb->ptr[ sb->len ]     = c;
    sb->ptr[ sb->len + 1 ] = '\0';
    sb->len++;
    return 1;
}

b32
strbuf_append( strbuf_t* sb, str_t src )
{
    if ( !strbuf_ok( *sb ) )
        return 0;

    /* +1 for null terminator. */
    if ( sb->len + src.len + 1 > strbuf_cap( *sb ) )
    {
        _strbuf_set_overflow( sb );
        return 0;
    }

    memcpy( sb->ptr + sb->len, src.ptr, ( usize )src.len );
    sb->len += src.len;
    sb->ptr[ sb->len ] = '\0';
    return 1;
}

b32
strbuf_append_cstr( strbuf_t* sb, const char* s )
{
    return strbuf_append( sb, str_from_cstr( s ) );
}

b32
strbuf_vappendf( strbuf_t* sb, const char* fmt, va_list args )
{
    if ( !strbuf_ok( *sb ) )
        return 0;

    /* avail includes the null terminator slot; vsnprintf writes at most avail bytes. */
    i32 avail = strbuf_cap( *sb ) - sb->len;
    if ( avail <= 0 )
    {
        _strbuf_set_overflow( sb );
        return 0;
    }

    int n = vsnprintf( sb->ptr + sb->len, ( usize )avail, fmt, args );

    /* vsnprintf returns the number of chars that WOULD be written, not counting '\0'.
       If n >= avail the output was truncated (though vsnprintf still null-terminates). */
    if ( n < 0 || n >= avail )
    {
        /* Clamp len to max valid -- the partial write is still null-terminated by vsnprintf. */
        sb->len = strbuf_cap( *sb ) - 1;
        sb->ptr[ sb->len ] = '\0';
        _strbuf_set_overflow( sb );
        return 0;
    }

    sb->len += n;
    return 1;
}

b32
strbuf_appendf( strbuf_t* sb, const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    b32 ok = strbuf_vappendf( sb, fmt, args );
    va_end( args );
    return ok;
}

/*==============================================================================================
    Format (overwrite from start)
==============================================================================================*/

b32
strbuf_fmt( strbuf_t* sb, const char* fmt, ... )
{
    /* Clear content and overflow so this is always a fresh start regardless of prior state.
       This is the key difference from strbuf_appendf: strbuf_fmt resets the buffer. */
    _strbuf_clear_overflow( sb );
    sb->len = 0;
    if ( sb->ptr && strbuf_cap( *sb ) > 0 )
        sb->ptr[ 0 ] = '\0';

    va_list args;
    va_start( args, fmt );
    b32 ok = strbuf_vappendf( sb, fmt, args );
    va_end( args );
    return ok;
}

/*==============================================================================================
    Editing
==============================================================================================*/

void
strbuf_chop( strbuf_t* sb, i32 new_len )
{
    if ( new_len < 0 )
        new_len = 0;
    if ( new_len >= sb->len )
        return;

    sb->len             = new_len;
    sb->ptr[ sb->len ]  = '\0';
}

void
strbuf_trim( strbuf_t* sb, i32 n )
{
    if ( n <= 0 )
        return;

    sb->len -= n;
    if ( sb->len < 0 )
        sb->len = 0;
    sb->ptr[ sb->len ] = '\0';
}

void
strbuf_strip_trailing( strbuf_t* sb, char c )
{
    while ( sb->len > 0 && sb->ptr[ sb->len - 1 ] == c )
    {
        sb->len--;
        sb->ptr[ sb->len ] = '\0';
    }
}

b32
strbuf_insert( strbuf_t* sb, i32 pos, str_t src )
{
    if ( !strbuf_ok( *sb ) )
        return 0;

    /* Validate insertion point and check capacity. */
    if ( pos < 0 || pos > sb->len || sb->len + src.len + 1 > strbuf_cap( *sb ) )
    {
        _strbuf_set_overflow( sb );
        return 0;
    }

    /* Shift existing content right to make room, including the null terminator. */
    memmove( sb->ptr + pos + src.len, sb->ptr + pos, ( usize )( sb->len - pos + 1 ) );

    /* Fill the gap with src. */
    memcpy( sb->ptr + pos, src.ptr, ( usize )src.len );
    sb->len += src.len;
    return 1;
}

b32
strbuf_remove( strbuf_t* sb, i32 pos, i32 count )
{
    if ( !strbuf_ok( *sb ) )
        return 0;

    if ( pos < 0 || count < 0 || pos + count > sb->len )
        return 0;

    /* Shift content left, including the null terminator (+1). */
    memmove( sb->ptr + pos, sb->ptr + pos + count, ( usize )( sb->len - pos - count + 1 ) );
    sb->len -= count;
    return 1;
}

/*==============================================================================================
    Number Formatting
==============================================================================================*/

strbuf_t
strbuf_from_i64( i64 value, char* buf, i32 cap )
{
    strbuf_t sb = strbuf_from_ptr_cap( buf, cap );
    strbuf_appendf( &sb, "%lld", ( long long )value );
    return sb;
}

strbuf_t
strbuf_from_u64( u64 value, char* buf, i32 cap )
{
    strbuf_t sb = strbuf_from_ptr_cap( buf, cap );
    strbuf_appendf( &sb, "%llu", ( unsigned long long )value );
    return sb;
}

strbuf_t
strbuf_from_f64( f64 value, i32 precision, char* buf, i32 cap )
{
    strbuf_t sb = strbuf_from_ptr_cap( buf, cap );
    strbuf_appendf( &sb, "%.*f", precision, value );
    return sb;
}

strbuf_t
strbuf_from_hex64( u64 value, char* buf, i32 cap )
{
    /* Lowercase hex digits, no "0x" prefix -- matches fmt_hex64 output semantics. */
    strbuf_t sb = strbuf_from_ptr_cap( buf, cap );
    strbuf_appendf( &sb, "%llx", ( unsigned long long )value );
    return sb;
}

/*============================================================================================*/
