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

#define SID( str )  sid_intern( str, strlen( str ));

/*============================================================================================*/
/* SID : HASHING */ 
/*============================================================================================*/

// TODO: make internal static functions?

                                // case-insensitive FNV-1a hash
uint32_t    sid_hash            ( const char* s );

                                // case-insensitive FNV-1a hash with length
uint32_t    sid_hash_len        ( const char* str, size_t len );

/*============================================================================================*/
/* SID : USAGE */
/*============================================================================================*/

                                // get C string from sid
const char* sid_cstr            ( sid_t sid );

                                // get length of interned string
uint8_t     sid_length          ( sid_t sid );

                                // compare two sids for equality
bool        sid_equals          ( sid_t a, sid_t b );

                                // check if string matches canonical case for given sid
bool        sid_is_canonical    ( sid_t sid, const char* str, size_t len );

                                // get hash of string entry
uint32_t    sid_get_hash        ( sid_t sid );

/*============================================================================================*/
/* SID : INITIALIZATION */
/*============================================================================================*/
                    
                                // initialize string interning system
void        sid_init            ( void );

                                // shutdown string interning system
void        sid_shutdown        ( void );

/*============================================================================================*/
/* INTERNING */
/*============================================================================================*/

                                // intern string with given length
sid_t       sid_intern          ( const char* str, int32_t len );

                                // intern C string (null-terminated)
sid_t       sid_intern_cstr     ( const char* str );

/*============================================================================================*/
/* DEBUG UTILITY */
/*============================================================================================*/

                                // print statistics to given file (e.g., stdout)
void        sid_print_stats     ( void* fp );

                                // reset period statistics counters (usually never).
void        sid_reset_stats     ( void );

/*============================================================================================*/
#endif