#ifndef STR_INTERN_H
#define STR_INTERN_H
// clang-format off
/*==============================================================================================

    str_intern.h

    Interned string entry (a compact representation for quick comparison)
    Think of it like an “opaque ID” for a string.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

typedef uint32_t sid_t;
#define SID_INVALID 0u

/*============================================================================================*/
/* HASHING */ 
/*============================================================================================*/

// TODO: make internal static functions?

                                // Case-insensitive FNV-1a hash
uint32_t    sid_hash            ( const char* s );

                                // Case-insensitive FNV-1a hash with length
uint32_t    sid_hash_len        ( const char* str, size_t len );

/*============================================================================================*/
/* INITIALIZATION */
/*============================================================================================*/
                    
                                // Initialize string interning system
void        sid_init            ( void );

                                // Shutdown string interning system
void        sid_shutdown        ( void );

/*============================================================================================*/
/* INTERNING */
/*============================================================================================*/

                                // Intern string with given length
sid_t       sid_intern          ( const char* str, int32_t len );

                                // Intern C string (null-terminated)
sid_t       sid_intern_cstr     ( const char* str );

/*============================================================================================*/
/* ACCESSORS + UTILITIES */
/*============================================================================================*/

                                // Get C string from sid
const char* sid_cstr            ( sid_t sid );

                                // Get length of interned string
uint8_t     sid_length          ( sid_t sid );

                                // Compare two sids for equality
bool        sid_equals          ( sid_t a, sid_t b );

                                // Check if provided string matches canonical case for given sid
bool        sid_is_canonical    ( sid_t sid, const char* str, size_t len );

/*============================================================================================*/
/* DEBUG UTILITY */
/*============================================================================================*/

                                // Print statistics to given file (e.g., stdout)
void        sid_print_stats     ( void* fp );

                                // Reset period statistics counters (usually never).
void        sid_reset_stats     ( void );

/*============================================================================================*/
#endif