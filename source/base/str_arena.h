/*==============================================================================================

    str_arena.h -- Linear string arena for scope-lifetime string building.

    A str_arena_t is a flat byte buffer with a position cursor. Allocations advance the
    cursor; saving and restoring marks provides stack-like scope management with no heap
    involvement. The user owns the backing memory -- stack array, struct field, or global.

    WHY USE str_arena OVER strbuf_t:

      strbuf_t is the right tool for building a SINGLE string incrementally (a path, a
      log line). str_arena_t is the right tool when you need MANY temporary strings in
      one scope, all freed together at scope exit:

          i32 mark = str_arena_mark( &scratch );
          str_t a = str_arena_push_fmt( &scratch, "Player_%d", id );
          str_t b = str_arena_push_fmt( &scratch, "%s/%s", dir, file );
          str_t c = str_arena_push_str( &scratch, prefix );
          send_message( a, b, c );
          str_arena_pop( &scratch, mark );    // a, b, c all gone at once

      Equivalent strbuf_t usage would require three separate fixed-size buffers and
      three independent overflow checks.

    GLOBAL BACKING BUFFERS:

      The base library has no internal state. Modules or executables that need a
      persistent scratch arena may declare a global buffer and a global str_arena_t
      wrapping it -- the arena struct itself holds no heap pointers:

          static char        g_scratch_buf[ 8192 ];
          static str_arena_t g_scratch = STR_ARENA_INIT( g_scratch_buf );

      str_arena_clear( &g_scratch ) resets the cursor at the start of any operation
      that needs a clean scratch space.

    STRBUF INTEGRATION:

      str_arena_strbuf allocates a writable strbuf_t from the arena, useful when you
      need incremental append operations and want the result to live in the arena:

          strbuf_t b = str_arena_strbuf( &scratch, 256 );
          strbuf_appendf( &b, "cmd: %s --n %d", cmd, count );
          str_t result = strbuf_str( b );     // valid until pop

==============================================================================================*/
#ifndef STR_ARENA_H
#define STR_ARENA_H

#include "orb.h"
#include "str.h"
#include "str_buf.h"
#include <stdarg.h>

/*==============================================================================================
    Type
==============================================================================================*/

typedef struct
{
    char* buf;  /* backing buffer; user-owned, never freed or reallocated by the arena */
    i32   cap;  /* total byte capacity of buf */
    i32   pos;  /* allocation cursor; next byte will be written here */
} str_arena_t;

/*==============================================================================================
    Construction Macros

    STR_ARENA_INIT( arr )
        Initialize a str_arena_t from a fixed char array. sizeof derives the capacity
        at compile time. Use for static/global initialization where a compound literal
        is not valid.

        Example:
            static char        g_buf[ 4096 ];
            static str_arena_t g_scratch = STR_ARENA_INIT( g_buf );

    STR_ARENA( arr )
        Compound-literal form. Use inside functions for local variables.

        Example:
            char storage[ 4096 ];
            str_arena_t arena = STR_ARENA( storage );

    str_arena_from_ptr_cap( ptr, cap )
        Build from a pointer and an explicit runtime capacity.

        Example:
            str_arena_t sub = str_arena_from_ptr_cap( region_ptr, region_bytes );

    str_arena_decl( name, size )
        Declare a self-contained stack-backed arena in one line. Creates a backing
        array 'name_buf_' and initializes 'name' as a str_arena_t over it.

        Example:
            str_arena_decl( scratch, 2048 );
            str_t path = str_arena_push_fmt( &scratch, "%s/%s", dir, file );
==============================================================================================*/

#define STR_ARENA_INIT( arr ) \
    { ( arr ), ( i32 )sizeof( arr ), 0 }

#define STR_ARENA( arr ) \
    ( str_arena_t ){ .buf = ( arr ), .cap = ( i32 )sizeof( arr ), .pos = 0 }

#define str_arena_from_ptr_cap( p, c ) \
    ( str_arena_t ){ .buf = ( char* )( p ), .cap = ( i32 )( c ), .pos = 0 }

#define str_arena_decl( name, size )                                              \
    char        name##_buf_[ ( size ) ] = { 0 };                                  \
    str_arena_t name = { .buf = name##_buf_, .cap = ( i32 )( size ), .pos = 0 }

/*==============================================================================================
    State Query
==============================================================================================*/

/* Bytes remaining before the arena is full. */
#define str_arena_remaining( a ) ( ( a ).cap - ( a ).pos )

/*==============================================================================================
    Marks -- scope-level allocate / free

    Save the cursor position before a group of allocations, then restore it to release
    all of them at once. str_t views returned between mark and pop become invalid after
    the pop -- do not hold onto them past the scope.

    Example:
        i32 mark = str_arena_mark( &scratch );
        str_t s  = str_arena_push_fmt( &scratch, "tmp_%d", n );
        use( s );
        str_arena_pop( &scratch, mark );    // s is now invalid
==============================================================================================*/

/* Returns the current cursor as a save point (an opaque integer offset). */
i32  str_arena_mark( const str_arena_t* a );

/* Restore the cursor to a saved mark, releasing all allocations made after it. */
void str_arena_pop( str_arena_t* a, i32 mark );

/* Reset the entire arena: cursor goes to 0. All returned str_t views become invalid. */
void str_arena_clear( str_arena_t* a );

/*==============================================================================================
    Push -- allocate strings into the arena
==============================================================================================*/

/* Allocate n raw bytes and return a pointer to the start. Returns NULL if out of space.
   The caller is responsible for initializing the memory. */
char* str_arena_push_raw( str_arena_t* a, i32 n );

/* Copy src into the arena and null-terminate it. Returns a str_t view of the copy,
   valid until the next pop or clear. Returns STR_EMPTY if src is empty or no space. */
str_t str_arena_push_str( str_arena_t* a, str_t src );

/* Copy a null-terminated C string into the arena. Returns STR_EMPTY on failure. */
str_t str_arena_push_cstr( str_arena_t* a, const char* s );

/* Format a string into the arena via printf-style arguments. Returns the resulting
   str_t (null-terminated, safe to pass as char* directly), or STR_EMPTY if the
   formatted result does not fit. On failure the arena cursor is not advanced. */
str_t str_arena_push_fmt( str_arena_t* a, const char* fmt, ... );

/* va_list variant; useful when forwarding from another variadic function. */
str_t str_arena_vpush_fmt( str_arena_t* a, const char* fmt, va_list args );

/*==============================================================================================
    strbuf_t Integration

    Allocate a writable strbuf_t whose backing bytes come from the arena. Use when you
    need incremental building with overflow detection and want the result to live in the
    arena. The arena advances by exactly cap bytes on success.

    After building, call str_arena_trim_strbuf to release the unused tail of the
    reservation so the arena cursor sits just after the written content.

    Important: str_arena_trim_strbuf is only safe when the strbuf is the most recent
    allocation in the arena. Calling it after other allocations will corrupt the arena.

    Example:
        i32      mark = str_arena_mark( &scratch );
        strbuf_t buf  = str_arena_strbuf( &scratch, 256 );
        strbuf_appendf( &buf, "Entity_%04d at (%d,%d)", id, x, y );
        str_arena_trim_strbuf( &scratch, &buf );   // release unused tail
        str_t label = strbuf_str( buf );           // valid str_t until pop
        str_arena_pop( &scratch, mark );
==============================================================================================*/

/* Allocate a strbuf_t of cap bytes from the arena. Returns a zeroed strbuf_t
   (strbuf_ok == 0) if cap <= 0 or the arena has insufficient space. */
strbuf_t str_arena_strbuf( str_arena_t* a, i32 cap );

/* Trim the arena cursor to sit immediately after buf's current content (len + 1).
   Only call this when buf is the most recent allocation made from this arena. */
void str_arena_trim_strbuf( str_arena_t* a, const strbuf_t* buf );

/*============================================================================================*/
#endif /* STR_ARENA_H */
