/*==============================================================================================

    str.h -- null-terminated C string operations.

        All functions operate on const char * / char * only.
        No dynamic allocation. Length-limited variants are preferred over unbounded ones.

==============================================================================================*/
#ifndef STR_H
#define STR_H
/*==============================================================================================
    Length / Query
==============================================================================================*/

// Returns byte-length of s, excluding null terminator.
usize str_len( const char* s );

// Returns 1 if s is NULL or its first byte is '\0'.
b32 str_empty( const char* s );

/*==============================================================================================
    Copy / Concat
==============================================================================================*/

// Copy at most (dst_cap - 1) bytes from src into dst, always null-terminates dst.
// Returns the length of src (not the amount written), like strlcpy.
usize str_copy( char* dst, usize dst_cap, const char* src );

// Append at most (dst_cap - strlen(dst) - 1) bytes of src. Always null-terminates.
// Returns total length that would have been produced (like strlcat).
usize str_append( char* dst, usize dst_cap, const char* src );

/*==============================================================================================
    Comparison
==============================================================================================*/

// Returns 1 if a and b are equal, 0 otherwise. NULL-safe (two NULLs are equal).
b32 str_equal( const char* a, const char* b );

// Lexicographic compare; returns <0, 0, or >0.
i32 str_cmp( const char* a, const char* b );

// Compare first n bytes only.
i32 str_ncmp( const char* a, const char* b, usize n );

// Case-insensitive equality (ASCII only).
b32 str_equal_nocase( const char* a, const char* b );

/*==============================================================================================
    Search
==============================================================================================*/

// Returns pointer to first occurrence of c in s, NULL if not found.
const char* str_find_char( const char* s, char c );

// Returns pointer to last occurrence of c in s, NULL if not found.
const char* str_rfind_char( const char* s, char c );

// Returns pointer to first occurrence of needle in haystack, NULL if not found.
const char* str_find_sub( const char* haystack, const char* needle );

// Returns 1 if s starts with prefix.
b32 str_starts_with( const char* s, const char* prefix );

// Returns 1 if s ends with suffix.
b32 str_ends_with( const char* s, const char* suffix );

/*==============================================================================================
    Hashing
==============================================================================================*/

// FNV-1a 64-bit hash. Deterministic; not cryptographic.
u64 str_hash( const char* s );

// FNV-1a over exactly n bytes (does not require null terminator).
u64 str_hash_n( const char* s, usize n );

/*==============================================================================================
    Parsing Helpers
==============================================================================================*/

// Parse a signed decimal integer from s. Stops at first non-digit.
// Stores result in *out. Returns number of characters consumed (0 on failure).
usize str_parse_i64( const char* s, long long* out );

// Parse an unsigned decimal integer from s.
usize str_parse_u64( const char* s, unsigned long long* out );

/*============================================================================================*/
#endif    // STR_H
