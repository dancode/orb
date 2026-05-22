/*==============================================================================================

    build_tool_str.h -- Minimal string view (str_t) and mutable buffer (strbuf_t).

    A self-contained clone of the engine's str.h / str_buf.h stripped to the subset
    the build tool needs. No orb.h dependency; uses standard C types throughout.

    str_t   -- non-owning, read-only view: { ptr, len }. No null-terminator guarantee.
               Substrings, search, and comparison are all O(len) or better with no copies.

    strbuf_t -- mutable, fixed-capacity buffer backed by caller-supplied memory.
                Always null-terminated so ptr is safe to pass to any C API.
                Overflow is tracked in the high bit of cap; strbuf_ok() is the check.

==============================================================================================*/
#ifndef BUILD_TOOL_STR_H
#define BUILD_TOOL_STR_H

// clang-format off

#include <stdbool.h>
#include <stdarg.h>

/*==============================================================================================
    str_t -- non-owning string view
==============================================================================================*/

typedef struct
{
    const char* ptr; /* first byte; NULL only when len == 0 */
    int         len; /* byte count, NOT including any null terminator */
} str_t;

/* Returned by search functions when no match is found. */
#define STR_NOT_FOUND ( -1 )

/*--- Construction ---*/

/* Build a str_t from a string literal. sizeof is resolved at compile time -- no strlen. */
#define STR( literal ) \
    ( str_t ){ .ptr = ( literal ), .len = ( int )( sizeof( literal ) - 1 ) }

/* Build a str_t from a pointer and an already-known length. */
#define str_from_ptr_len( p, n ) \
    ( str_t ){ .ptr = ( const char* )( p ), .len = ( int )( n ) }

/* True when len == 0, regardless of ptr. */
#define str_is_empty( s ) ( ( s ).len == 0 )

/* Build from a null-terminated C string. Calls strlen once; prefer STR() for literals. */
str_t str_from_cstr( const char* s );

/* Sub-view of bytes [start, end). No copy. Clamps to valid range. */
str_t str_sub( str_t s, int start, int end );

/* First n bytes. Clamps to s.len. */
str_t str_prefix( str_t s, int n );

/* Last n bytes. Clamps to s.len. */
str_t str_suffix( str_t s, int n );

/* Strip leading and trailing ASCII whitespace. No copy. */
str_t str_trim( str_t s );

/*--- Comparison ---*/

bool str_equal( str_t a, str_t b );
bool str_equal_nocase( str_t a, str_t b ); /* ASCII letters only */

/*--- Search ---*/

/* Byte index of first/last occurrence of c, or STR_NOT_FOUND. */
int  str_find_char( str_t s, char c );
int  str_rfind_char( str_t s, char c );

bool str_starts_with( str_t s, str_t prefix );
bool str_ends_with( str_t s, str_t suffix );
bool str_contains( str_t s, str_t needle );

/*
    str_next_token( remainder, token )
        Pull the next whitespace-delimited token from *remainder, write it to *token,
        and advance *remainder past it. Returns false when no token is left.
        Idiomatic replacement for manual while(*p) pointer walks:

            str_t s = str_from_cstr( raw );
            str_t tok;
            while ( str_next_token( &s, &tok ) )
                fwrite( tok.ptr, 1, tok.len, out );
*/
bool str_next_token( str_t* remainder, str_t* token );

/*
    str_split_once( s, sep, left, right )
        Split s on the first occurrence of sep. Writes the part before sep to *left
        and the part after to *right. Returns false if sep is not found (left = s, right empty).
        Useful for "KEY=VALUE" env-var lines from vcvars output.
*/
bool str_split_once( str_t s, char sep, str_t* left, str_t* right );

/*--- C string interop ---*/

/* Copy s into buf, null-terminate. Writes at most (cap - 1) bytes. Returns bytes written. */
int  str_to_cstr( str_t s, char* buf, int cap );

/*==============================================================================================
    strbuf_t -- mutable fixed-capacity buffer
==============================================================================================*/

/* High bit of cap is the overflow flag. Practical strings never exceed 2 GB. */
#define STRBUF_OVF_BIT ( ( unsigned int )1u << 31u )

typedef struct
{
    char* ptr; /* writable buffer; always null-terminated within cap */
    int   len; /* current byte count, NOT including the null terminator */
    int   cap; /* total bytes including the null slot; high bit = overflow flag */
} strbuf_t;

/*--- State query ---*/

#define strbuf_cap( sb )        ( ( int )( ( unsigned int )( sb ).cap & ~STRBUF_OVF_BIT ) )
#define strbuf_ok( sb )         ( ( ( unsigned int )( sb ).cap & STRBUF_OVF_BIT ) == 0u )
#define strbuf_overflowed( sb ) ( ( ( unsigned int )( sb ).cap & STRBUF_OVF_BIT ) != 0u )
#define strbuf_remaining( sb )  ( strbuf_ok( sb ) ? strbuf_cap( sb ) - ( sb ).len - 1 : 0 )

/*--- Construction ---*/

/* Wrap a pre-existing char array; sizeof derives capacity at compile time. */
#define STRBUF( arr ) \
    ( strbuf_t ){ .ptr = ( arr ), .len = 0, .cap = ( int )sizeof( arr ) }

/* Build from a pointer and a runtime capacity. */
#define strbuf_from_ptr_cap( p, c ) \
    ( strbuf_t ){ .ptr = ( char* )( p ), .len = 0, .cap = ( int )( c ) }

/*
    strbuf_decl( name, size )
        Declare a stack-backed strbuf in one line. The recommended way to create
        a working buffer for path or command assembly.

            strbuf_decl( path, 512 );
            strbuf_appendf( &path, "%s\\%s", dir, file );
            if ( !strbuf_ok( path ) ) return false;
            fopen( path.ptr, "rb" );
*/
#define strbuf_decl( name, size )              \
    char     name##_buf_[ ( size ) ] = { 0 };  \
    strbuf_t name = { .ptr = name##_buf_, .len = 0, .cap = ( int )( size ) }

/* Zero-cost cast to a str_t view of the current buffer contents. */
#define strbuf_str( sb ) \
    ( str_t ){ .ptr = ( sb ).ptr, .len = ( sb ).len }

/*
    STRBUF_FIELD( name, size )
        Declare a self-contained strbuf inside a struct. Emits both the backing storage
        and the strbuf_t as adjacent struct members. Follow with STRBUF_FIELD_INIT after
        the struct is allocated to wire the ptr to the storage.

            typedef struct {
                STRBUF_FIELD( flags,   512  );
                STRBUF_FIELD( defines, 1024 );
            } compile_cmd_t;

    STRBUF_FIELD_INIT( s, name )
        Initialize a STRBUF_FIELD member after the containing struct is live.

            compile_cmd_t cc;
            STRBUF_FIELD_INIT( cc, flags );
            STRBUF_FIELD_INIT( cc, defines );
*/
#define STRBUF_FIELD( name, size ) \
    char     name##_storage_[ ( size ) ]; \
    strbuf_t name

#define STRBUF_FIELD_INIT( s, name ) \
    ( s ).name = strbuf_from_ptr_cap( ( s ).name##_storage_, sizeof( ( s ).name##_storage_ ) )

/*--- Reset ---*/

/* Reset len to 0, null-terminate at ptr[0]. Preserves overflow flag. */
void strbuf_clear( strbuf_t* sb );

/* Zero the entire backing buffer, reset len, and clear the overflow flag. */
void strbuf_zero( strbuf_t* sb );

/*--- Write (overwrite from start) ---*/

bool strbuf_set( strbuf_t* sb, str_t src );
bool strbuf_set_cstr( strbuf_t* sb, const char* s );

/*--- Append ---*/

bool strbuf_append_char( strbuf_t* sb, char c );
bool strbuf_append( strbuf_t* sb, str_t src );
bool strbuf_append_cstr( strbuf_t* sb, const char* s );
bool strbuf_appendf( strbuf_t* sb, const char* fmt, ... );
bool strbuf_vappendf( strbuf_t* sb, const char* fmt, va_list args );

/*--- Format (overwrite from start) ---*/

/* Clear buffer (including overflow flag), then printf-format into it. */
bool strbuf_fmt( strbuf_t* sb, const char* fmt, ... );

/*--- Editing ---*/

/* Truncate to new_len. No-op if new_len >= current len. */
void strbuf_chop( strbuf_t* sb, int new_len );

/* Remove all trailing occurrences of character c. */
void strbuf_strip_trailing( strbuf_t* sb, char c );

/*--- Path ---*/

/*
    strbuf_path_append( sb, segment )
        Append a path segment, inserting a backslash separator as needed.
        If sb is non-empty and does not already end with \ or /, inserts one first.
        Safe to call on an empty sb for the first segment (no leading backslash emitted).

            strbuf_decl( path, 512 );
            strbuf_path_append( &path, str_from_cstr( g_build_dir ) );
            strbuf_path_append( &path, STR( "obj" ) );
            strbuf_path_append( &path, str_from_cstr( target->name ) );
*/
bool strbuf_path_append( strbuf_t* sb, str_t segment );

/*============================================================================================*/
#endif /* BUILD_TOOL_STR_H */
