#ifndef SID_H
#define SID_H
// clang-format off
/*==============================================================================================

    str_intern.h

    Interned string entry (a compact representation for quick comparison)
    Think of it like an “opaque ID” for a string.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

#define SID_INVALID     ((sid_t){ 0u })
#define SID( str )      sid_intern( str, strlen( str ));

/*============================================================================================*/

typedef struct sid_s                    // string ID: offset into string arena
{ 
    uint32_t        off; 

} sid_t;

typedef struct intern_slot_s            // hash table entry: hash + sid (offset)
{
    uint32_t        hash;               // full 32-bit hash
    sid_t           sid;                // offset into string pool (or SID_INVALID if empty)

} intern_slot_t;

typedef struct string_arena_s
{
    char*           base;               // string storage arena
    uint32_t        used;               // bytes used
    uint32_t        capacity;           // total capacity

} string_arena_t;

typedef struct intern_state_s
{
    intern_slot_t*  table;              // hash table (dynamically sized)
    string_arena_t  arena;              // string storage (dynamically sized)
    uint32_t        table_size;         // entry table capacity (power of 2)
    uint32_t        entry_count;        // number of entires (interned strings) used.
    float           max_load;           // Load factor threshold (e.g., 0.7 for 70%)

    uint32_t        total_lookups;      // statistics: total lookups (number of sid_intern calls)
    uint32_t        total_probes;       // statistics: total probes (sum of all probe lengths)
    uint32_t        max_probe_length;   // statistics: max probe length (longest probe sequence)
    uint32_t        rehash_count;       // statistics: number of rehashes (resizes)

} intern_state_t;

/*============================================================================================*/
/* SID : HASHING */ 
/*============================================================================================*/

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
#endif // SID_H