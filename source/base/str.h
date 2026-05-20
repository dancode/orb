/*==============================================================================================

    str_new.h -- Non-owning string view type (str_t).

    WHY str_t BEATS char*

    C strings (char*) have three fundamental problems:
      1. Unknown length  -- every receiver must call strlen(), paying O(n) repeatedly.
      2. No sub-string   -- producing a sub-string requires a copy or mutating the buffer.
      3. Implicit length -- no bound information means overruns are structurally possible.

    str_t solves all three by carrying {ptr, len} together:
      - O(1) length at every call site; strlen() is called at most once, at construction.
      - Substrings are free: str_sub() adjusts ptr and len with no copy or allocation.
      - All read operations use len as the bound -- overruns are structurally impossible.
      - STR("literal") evaluates sizeof at compile time: zero runtime cost.

    str_t does NOT guarantee null termination and does NOT own its memory.
    It is a read-only window into bytes that someone else owns. For a writable,
    always-null-terminated buffer see str_buf_new.h (strbuf_t).

==============================================================================================*/
#ifndef STR_NEW_H
#define STR_NEW_H

#include "orb.h"

/*==============================================================================================
    Type
==============================================================================================*/

typedef struct
{
    const char* ptr;  /* first byte; may be NULL only when len == 0 */
    i32         len;  /* byte count, NOT including any null terminator */
    u32         _pad; /* padding for future use, zeroed by construction */
} str_t;

/* A valid empty string that can be used as a default or sentinel return value. */
extern const str_t STR_EMPTY;

/*==============================================================================================
    Construction Macros
==============================================================================================*/

/*
    STR( "literal" )
        Build a str_t from a string literal. sizeof is evaluated at compile time by the
        compiler, so this expands to a struct literal with no runtime work at all -- no
        function call, no strlen, no loop.

        Example:
            str_t shader = STR( "shaders/basic.hlsl" );
            // shader.ptr == "shaders/basic.hlsl"
            // shader.len == 18 (computed at compile time via sizeof)

        IMPORTANT: the argument must be a string literal ("..."), not a char* variable.
        Use str_from_cstr() for variables.
*/
#define STR( literal ) \
    ( str_t ) { .ptr = ( literal ), .len = ( i32 )( sizeof( literal ) - 1 ) }

/*
    str_from_ptr_len( ptr, len )
        Build a str_t from any char pointer and a length you already have.
        Use when parsing, reading from buffers, or when len came from another call.

        Example:
            const char* p = some_parser( cursor, &n );
            str_t token = str_from_ptr_len( p, n );
*/
#define str_from_ptr_len( p, n ) \
    ( str_t ) { .ptr = ( const char* )( p ), .len = ( i32 )( n ) }

/*
    str_is_empty( s ) / str_is_valid( s )
        Inline predicates that expand to a single comparison -- no function call overhead.
        str_is_valid allows NULL ptr as long as len is 0 (the STR_EMPTY sentinel).
*/
#define str_is_empty( s ) ( ( s ).len == 0 )
#define str_is_valid( s ) ( ( s ).ptr != NULL || ( s ).len == 0 )

/* Sentinel returned by search functions when no match is found.
   -1 is chosen because valid indices are always >= 0. */
#define STR_NOT_FOUND ( -1 )

/*==============================================================================================
    Construction Functions
==============================================================================================*/

/* Build from a null-terminated C string. Walks the string once to measure length (O(n)).
   Returns STR_EMPTY if s is NULL. Use STR() instead whenever s is a string literal. */
str_t str_from_cstr( const char* s );

/* Return a sub-view of s covering bytes [start, end) -- no copy, no allocation.
   Clamps start and end to valid range. Returns STR_EMPTY if start >= end. */
str_t str_sub( str_t s, i32 start, i32 end );

/* First n bytes of s. Clamps to s.len. */
str_t str_prefix( str_t s, i32 n );

/* Last n bytes of s. Clamps to s.len. */
str_t str_suffix( str_t s, i32 n );

/* Advance ptr past any leading ASCII whitespace, return a sub-view. No allocation. */
str_t str_trim_left( str_t s );

/* Retreat past trailing ASCII whitespace by reducing len. Return a sub-view. No allocation. */
str_t str_trim_right( str_t s );

/* Trim both edges. */
str_t str_trim( str_t s );

/*==============================================================================================
    Comparison
==============================================================================================*/

/* True if a and b contain the same bytes in the same order. O(n). */
b32 str_equal( str_t a, str_t b );

/* Case-insensitive equality for ASCII letters. O(n). */
b32 str_equal_nocase( str_t a, str_t b );

/* Lexicographic compare. Returns < 0, 0, or > 0.
   Shorter string is less-than when it is a prefix of the longer one. */
i32 str_cmp( str_t a, str_t b );

/*==============================================================================================
    Search
==============================================================================================*/

/* Index of the first byte equal to c, or STR_NOT_FOUND. */
i32 str_find_char( str_t s, char c );

/* Index of the last byte equal to c, or STR_NOT_FOUND. */
i32 str_rfind_char( str_t s, char c );

/* Byte offset of the first occurrence of needle in haystack, or STR_NOT_FOUND.
   Uses memcmp at each candidate position -- efficient for short needles. */
i32 str_find( str_t haystack, str_t needle );

/* True if s begins with prefix. */
b32 str_starts_with( str_t s, str_t prefix );

/* True if s ends with suffix. */
b32 str_ends_with( str_t s, str_t suffix );

/* True if needle appears anywhere in s. */
b32 str_contains( str_t s, str_t needle );

/*==============================================================================================
    Hashing
==============================================================================================*/

/* FNV-1a 32-bit hash over the full byte sequence. Case-sensitive.
   Use for small hash tables or when a 32-bit key is required (e.g. SID). */
u32 str_hash32( str_t s );

/* FNV-1a 64-bit hash. Better avalanche; use for large tables or many keys. */
u64 str_hash64( str_t s );

/*==============================================================================================
    Parsing (str_t -> number)
==============================================================================================*/

/* Parse a signed decimal integer. Stores result in *out. Returns 1 on success, 0 on failure.
   Accepts optional leading '+' or '-'. Any non-digit character after optional sign fails. */
b32 str_to_i32( str_t s, i32* out );
b32 str_to_i64( str_t s, i64* out );

/* Parse a decimal float. Accepts optional sign, integer digits, optional '.' and fraction. */
b32 str_to_f64( str_t s, f64* out );
b32 str_to_f32( str_t s, f32* out );

/* Scan a signed integer prefix from s. Unlike str_to_i64, does NOT require s to be entirely
   numeric: parsing stops at the first non-digit byte. Returns bytes consumed, 0 on failure.
   Useful for tokenizers where a number token precedes other characters. */
i32 str_scan_i64( str_t s, i64* out );

/* Scan an unsigned integer prefix from s. Returns bytes consumed, 0 on failure. */
i32 str_scan_u64( str_t s, u64* out );

/*==============================================================================================
    C String Interop
==============================================================================================*/

/* Copy s into buf and null-terminate. Writes at most (cap - 1) bytes.
   Returns the number of bytes written (less than s.len if buf was too small).
   Always null-terminates provided cap >= 1.
   This is the correct way to bridge a str_t into a C API that requires char*. */
i32 str_to_cstr( str_t s, char* buf, i32 cap );

/*============================================================================================*/
#endif /* STR_NEW_H */
