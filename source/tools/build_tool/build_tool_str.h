/*==============================================================================================

    build_tool_str.h -- Minimal string view (s_t) and mutable buffer (sbuf_t).

    Self-contained; no orb.h or engine dependency. Uses standard C types throughout.

    s_t    -- non-owning read-only view: { ptr, len }. No null-terminator guarantee.
    sbuf_t -- mutable fixed-capacity buffer backed by caller-supplied memory.
              Always null-terminated. Overflow tracked in the high bit of cap; sbuf_ok() checks it.

==============================================================================================*/
#ifndef S_H
#define S_H

// clang-format off

#include <stdbool.h>
#include <stdarg.h>

/*==============================================================================================
    s_t -- non-owning string view
==============================================================================================*/

typedef struct
{
    const char* ptr; /* first byte; NULL only when len == 0 */
    int         len; /* byte count, NOT including any null terminator */
} s_t;

#define S_NOT_FOUND ( -1 )

/*--- Construction ---*/

/* Build from a string literal; sizeof resolved at compile time, no strlen. */
#define S( literal ) \
    ( s_t ){ .ptr = ( literal ), .len = ( int )( sizeof( literal ) - 1 ) }

#define s_from_ptr_len( p, n ) \
    ( s_t ){ .ptr = ( const char* )( p ), .len = ( int )( n ) }

#define s_is_empty( s ) ( ( s ).len == 0 )

/* Build from a null-terminated C string. Calls strlen once; prefer S() for literals. */
s_t  s_from_cstr( const char* s );

/* Sub-view of bytes [start, end). No copy. Clamps to valid range. */
s_t  s_sub( s_t s, int start, int end );

/* First/last n bytes. Clamp to s.len. */
s_t  s_prefix( s_t s, int n );
s_t  s_suffix( s_t s, int n );

/* Strip leading and trailing ASCII whitespace. No copy. */
s_t  s_trim( s_t s );

/*--- Comparison ---*/

bool s_equal( s_t a, s_t b );
bool s_equal_nocase( s_t a, s_t b ); /* ASCII letters only */

/*--- Search ---*/

int  s_find_char( s_t s, char c );   /* first occurrence, or S_NOT_FOUND */
int  s_rfind_char( s_t s, char c );  /* last occurrence, or S_NOT_FOUND */

bool s_starts_with( s_t s, s_t prefix );
bool s_ends_with( s_t s, s_t suffix );
bool s_contains( s_t s, s_t needle );

/*
    s_next_token( remainder, token )
        Pull the next whitespace-delimited token from *remainder, write it to *token,
        and advance *remainder past it. Returns false when no token is left.

            s_t s = s_from_cstr( raw );
            s_t tok;
            while ( s_next_token( &s, &tok ) )
                fwrite( tok.ptr, 1, tok.len, out );
*/
bool s_next_token( s_t* remainder, s_t* token );

/*
    s_split_once( s, sep, left, right )
        Split on the first occurrence of sep. Returns false if sep not found.
        Useful for "KEY=VALUE" env-var lines.
*/
bool s_split_once( s_t s, char sep, s_t* left, s_t* right );

/*--- C string interop ---*/

/* Copy into buf, null-terminate. Writes at most (cap - 1) bytes. Returns bytes written. */
int  s_to_cstr( s_t s, char* buf, int cap );

/*==============================================================================================
    sbuf_t -- mutable fixed-capacity buffer
==============================================================================================*/

#define SBUF_OVF_BIT ( ( unsigned int )1u << 31u )

typedef struct
{
    char* ptr; /* writable buffer; always null-terminated within cap */
    int   len; /* current byte count, NOT including the null terminator */
    int   cap; /* total bytes including the null slot; high bit = overflow flag */
} sbuf_t;

/*--- State query ---*/

#define sbuf_cap( sb )        ( ( int )( ( unsigned int )( sb ).cap & ~SBUF_OVF_BIT ) )
#define sbuf_ok( sb )         ( ( ( unsigned int )( sb ).cap & SBUF_OVF_BIT ) == 0u )
#define sbuf_overflowed( sb ) ( ( ( unsigned int )( sb ).cap & SBUF_OVF_BIT ) != 0u )
#define sbuf_remaining( sb )  ( sbuf_ok( sb ) ? sbuf_cap( sb ) - ( sb ).len - 1 : 0 )

/*--- Construction ---*/

/* Wrap a pre-existing char array; sizeof derives capacity at compile time. */
#define SBUF( arr ) \
    ( sbuf_t ){ .ptr = ( arr ), .len = 0, .cap = ( int )sizeof( arr ) }

#define sbuf_from_ptr_cap( p, c ) \
    ( sbuf_t ){ .ptr = ( char* )( p ), .len = 0, .cap = ( int )( c ) }

/*
    sbuf_decl( name, size )
        Declare a stack-backed sbuf_t in one line.

            sbuf_decl( path, 512 );
            sbuf_appendf( &path, "%s\\%s", dir, file );
            if ( !sbuf_ok( path ) ) return false;
            fopen( path.ptr, "rb" );
*/
#define sbuf_decl( name, size )                \
    char   name##_buf_[ ( size ) ] = { 0 };    \
    sbuf_t name = { .ptr = name##_buf_, .len = 0, .cap = ( int )( size ) }

/* Zero-cost cast to an s_t view of the current buffer contents. */
#define sbuf_str( sb ) \
    ( s_t ){ .ptr = ( sb ).ptr, .len = ( sb ).len }

/*
    SBUF_FIELD( name, size ) / SBUF_FIELD_INIT( s, name )
        Declare a self-contained sbuf_t inside a struct, then wire it after the struct is live.

            typedef struct {
                SBUF_FIELD( flags,   512  );
                SBUF_FIELD( defines, 1024 );
            } compile_cmd_t;

            compile_cmd_t cc;
            SBUF_FIELD_INIT( cc, flags );
            SBUF_FIELD_INIT( cc, defines );
*/
#define SBUF_FIELD( name, size ) \
    char   name##_storage_[ ( size ) ]; \
    sbuf_t name

#define SBUF_FIELD_INIT( s, name ) \
    ( s ).name = sbuf_from_ptr_cap( ( s ).name##_storage_, sizeof( ( s ).name##_storage_ ) )

/*--- Reset ---*/

void sbuf_clear( sbuf_t* sb );  /* reset len; preserve overflow flag */
void sbuf_zero( sbuf_t* sb );   /* full reset including overflow flag */

/*--- Write (overwrite from start) ---*/

bool sbuf_set( sbuf_t* sb, s_t src );
bool sbuf_set_cstr( sbuf_t* sb, const char* s );

/*--- Append ---*/

bool sbuf_append_char( sbuf_t* sb, char c );
bool sbuf_append( sbuf_t* sb, s_t src );
bool sbuf_append_cstr( sbuf_t* sb, const char* s );
bool sbuf_appendf( sbuf_t* sb, const char* fmt, ... );
bool sbuf_vappendf( sbuf_t* sb, const char* fmt, va_list args );

/*--- Format (overwrite from start) ---*/

/* Clear buffer (including overflow flag), then printf-format into it. */
bool sbuf_fmt( sbuf_t* sb, const char* fmt, ... );

/*--- Editing ---*/

void sbuf_chop( sbuf_t* sb, int new_len );         /* truncate to new_len */
void sbuf_strip_trailing( sbuf_t* sb, char c );    /* remove trailing occurrences of c */

/*--- Path ---*/

/*
    sbuf_path_append( sb, segment )
        Append a path segment with a backslash separator.
        No leading backslash when sb is empty; no double separator if sb already ends with one.

            sbuf_decl( path, 512 );
            sbuf_path_append( &path, s_from_cstr( g_build_dir ) );
            sbuf_path_append( &path, S( "obj" ) );
            sbuf_path_append( &path, s_from_cstr( target->name ) );
*/
bool sbuf_path_append( sbuf_t* sb, s_t segment );

/*============================================================================================*/
#endif /* S_H */
