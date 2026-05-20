/*==============================================================================================

    str_buf_new.h -- Mutable, fixed-capacity string buffer (strbuf_t).

    strbuf_t is the writable companion to str_t. It pairs a writable char* with a length
    and a capacity. The buffer is ALWAYS null-terminated, so ptr is always safe to pass
    directly to C APIs that expect char*. No heap allocation is required -- back the buffer
    with a stack array, a struct field, or (optionally) a heap allocation.

    WHY strbuf_t BEATS char[] + manual tracking:

      With raw char[]:
          char path[512];
          int len = 0;
          len += sprintf(path + len, "%s/", base);      // not safe: no overflow check
          len += sprintf(path + len, "%s", file);       // still not safe

      With strbuf_t:
          strbuf_decl( path, 512 );
          strbuf_appendf( &path, "%s/", base );
          strbuf_appendf( &path, "%s", file );
          if ( !strbuf_ok( path ) ) handle_overflow();
          fopen( path.ptr, "rb" );                      // ptr always null-terminated

      Benefits:
        - Overflow is tracked and checked at any point with strbuf_ok(), not silently ignored.
        - ptr is ALWAYS null-terminated -- safe to hand to any C API at any time.
        - strbuf_str() is a zero-cost cast to str_t for read-only operations.
        - strbuf_decl() declares and initializes a stack buffer in a single line.
        - All write functions return b32 so errors can be chained.

    OVERFLOW TRACKING:
        The high bit of cap (bit 31) is the overflow flag. Practical strings never exceed
        2 GB, so this bit is always free. Use strbuf_ok() and strbuf_overflowed() to read it.
        Once set, all append/write functions become no-ops until the buffer is reset.

==============================================================================================*/
#ifndef STR_BUF_NEW_H
#define STR_BUF_NEW_H

#include "orb.h"
#include "str.h"
#include <stdarg.h>

/*==============================================================================================
    Type
==============================================================================================*/

/* Overflow flag is packed into the high bit of cap. */
#define STRBUF_OVF_BIT ( ( u32 )1u << 31u )

typedef struct
{
    char* ptr; /* writable buffer; always null-terminated within cap */
    i32   len; /* current byte count, NOT including the null terminator */
    i32   cap; /* total buffer bytes including the null slot; high bit = overflow flag */

} strbuf_t;

/*==============================================================================================
    State Query Macros

    strbuf_cap( sb )
        Real capacity in bytes (null terminator slot included), overflow bit masked off.
        Use this instead of .cap directly whenever you need the numeric capacity.

    strbuf_ok( sb )
        True if the buffer has NOT overflowed. The normal success check after a series
        of appends:

            strbuf_decl( msg, 64 );
            strbuf_appendf( &msg, "error %d in %s", code, name );
            if ( strbuf_ok( msg ) ) log( msg.ptr );

    strbuf_overflowed( sb )
        True if any previous write did not fit. The buffer still contains a valid, partial,
        null-terminated string up to the available space.

    strbuf_remaining( sb )
        Writable bytes left before the buffer is full (0 if overflowed).
        Accounts for the null terminator slot.
==============================================================================================*/

#define strbuf_cap( sb )        ( ( i32 )( ( u32 )( sb ).cap & ~STRBUF_OVF_BIT ) )
#define strbuf_ok( sb )         ( ( ( u32 )( sb ).cap & STRBUF_OVF_BIT ) == 0u )
#define strbuf_overflowed( sb ) ( ( ( u32 )( sb ).cap & STRBUF_OVF_BIT ) != 0u )
#define strbuf_remaining( sb )  ( strbuf_ok( sb ) ? strbuf_cap( sb ) - ( sb ).len - 1 : 0 )

/*==============================================================================================
    Construction Macros

    STRBUF( arr )
        Wrap a pre-existing char array. sizeof derives the capacity at compile time.

        Example:
            char storage[ 256 ];
            strbuf_t buf = STRBUF( storage );

    strbuf_from_ptr_cap( ptr, cap )
        Build a strbuf_t from a pointer and an explicit capacity integer.
        Use when wrapping a buffer whose size is only known at runtime.

        Example:
            strbuf_t sub = strbuf_from_ptr_cap( arena_ptr, arena_remaining );

    strbuf_decl( name, size )
        Declare a self-contained strbuf in one line. Creates a backing array 'name_buf_'
        on the stack and initialises 'name' as a strbuf_t over it.
        This is the recommended way to create a stack-backed working buffer.

        Example:
            strbuf_decl( path, 512 );
            strbuf_appendf( &path, "%s/%s", dir, file );
            fopen( path.ptr, "rb" );   // ptr is always safe to pass here

    strbuf_str( sb )
        Produce a zero-cost str_t view of the current strbuf contents.
        The view shares the same pointer and length -- no copy.
        Valid only while the strbuf is alive and unmodified.

        Example:
            strbuf_decl( label, 64 );
            strbuf_fmt( &label, "Entity_%04d", id );
            log_info( strbuf_str( label ) );   // str_t, zero cost
==============================================================================================*/

#define STRBUF( arr ) \
    ( strbuf_t ) { .ptr = ( arr ), .len = 0, .cap = ( i32 )sizeof( arr ) }

#define strbuf_from_ptr_cap( p, c ) \
    ( strbuf_t ) { .ptr = ( char* )( p ), .len = 0, .cap = ( i32 )( c ) }

#define strbuf_decl( name, size )             \
    char     name##_buf_[ ( size ) ] = { 0 }; \
    strbuf_t name                    = { .ptr = name##_buf_, .len = 0, .cap = ( i32 )( size ) }

#define strbuf_str( sb ) \
    ( str_t ) { .ptr = ( sb ).ptr, .len = ( sb ).len }

/*==============================================================================================
    Heap Allocation (Optional)

    All other strbuf functions work identically on stack and heap buffers.
    Only call strbuf_free on a buffer returned by strbuf_alloc.
==============================================================================================*/

/* Allocate a heap-backed strbuf with the given capacity.
   Returns a zeroed strbuf_t on allocation failure (strbuf_ok returns 0). */
strbuf_t strbuf_alloc( i32 capacity );

/* Free a heap-backed strbuf. Zeros out the struct so dangling use is obvious.
   Must only be called on buffers produced by strbuf_alloc. */
void strbuf_free( strbuf_t* sb );

/*==============================================================================================
    Reset
==============================================================================================*/

/* Reset length to 0 and null-terminate at ptr[0]. Preserves the overflow flag so
   overflow is not silently ignored after a cheap clear. */
void strbuf_clear( strbuf_t* sb );

/* Zero the entire backing buffer and reset len to 0. Also clears the overflow flag.
   Use to fully reclaim a buffer after overflow. */
void strbuf_zero( strbuf_t* sb );

/*==============================================================================================
    Write (overwrite from start)
==============================================================================================*/

/* Overwrite buffer contents with src. Clears previous content.
   Sets overflow and returns 0 if src.len does not fit. */
b32 strbuf_set( strbuf_t* sb, str_t src );

/* Overwrite from a null-terminated C string. */
b32 strbuf_set_cstr( strbuf_t* sb, const char* s );

/*==============================================================================================
    Append
==============================================================================================*/

/* Append a single character. */
b32 strbuf_append_char( strbuf_t* sb, char c );

/* Append a str_t. */
b32 strbuf_append( strbuf_t* sb, str_t src );

/* Append a null-terminated C string. */
b32 strbuf_append_cstr( strbuf_t* sb, const char* s );

/* Printf-style append into remaining space. If the formatted string does not fit,
   writes as many bytes as possible, null-terminates, sets overflow, returns 0. */
b32 strbuf_appendf( strbuf_t* sb, const char* fmt, ... );

/* va_list variant; useful when forwarding from another variadic function. */
b32 strbuf_vappendf( strbuf_t* sb, const char* fmt, va_list args );

/*==============================================================================================
    Format (overwrite from start)
==============================================================================================*/

/* Printf-style format. Clears the buffer first (including overflow), then formats.
   This is the safe way to re-use an overflowed buffer: strbuf_fmt resets state.
   Returns 0 if the formatted string did not fit. */
b32 strbuf_fmt( strbuf_t* sb, const char* fmt, ... );

/*==============================================================================================
    Editing
==============================================================================================*/

/* Truncate to new_len, writing a null terminator at ptr[new_len].
   No-op if new_len >= current len. */
void strbuf_chop( strbuf_t* sb, i32 new_len );

/* Remove the last n bytes. Clamps so len never goes below 0. */
void strbuf_trim( strbuf_t* sb, i32 n );

/* Remove all trailing occurrences of character c. */
void strbuf_strip_trailing( strbuf_t* sb, char c );

/* Insert src at byte position pos. Shifts existing content right.
   Sets overflow and returns 0 if there is not enough space. */
b32 strbuf_insert( strbuf_t* sb, i32 pos, str_t src );

/* Remove count bytes starting at pos. Shifts remaining content left.
   Returns 0 if pos or count are out of range. */
b32 strbuf_remove( strbuf_t* sb, i32 pos, i32 count );

/*==============================================================================================
    Number Formatting

    Format a primitive value into a fresh strbuf backed by the provided buffer.
    These do NOT heap-allocate. buf and cap come from the caller (typically a stack array).
    Check strbuf_ok() on the result if you care about truncation.

    Example:
        char tmp[ 32 ];
        strbuf_t s = strbuf_from_i64( entity_id, tmp, sizeof( tmp ) );
        log_info( strbuf_str( s ) );
==============================================================================================*/

strbuf_t strbuf_from_i64( i64 value, char* buf, i32 cap );
strbuf_t strbuf_from_u64( u64 value, char* buf, i32 cap );
strbuf_t strbuf_from_f64( f64 value, i32 precision, char* buf, i32 cap );

/* Lowercase hex, no "0x" prefix (e.g. 255 -> "ff"). */
strbuf_t strbuf_from_hex64( u64 value, char* buf, i32 cap );

/*============================================================================================*/
#endif /* STR_BUF_NEW_H */
