#ifndef SID_H
#define SID_H
// clang-format off
/*==============================================================================================

    str_intern.h

    Interned string entry (a compact representation for quick comparison)
    Think of it like an �opaque ID� for a string.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

#define SID_INVALID     ((sid_t){ 0u })

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

/*==============================================================================================
    SID : INLINE HELPERS (stateless - callable from any TU, no link dependency)                
==============================================================================================*/

// case-insensitive FNV-1a: DLLs and codegen call this directly
static inline uint32_t
sid_hash( const char* s )
{
    uint32_t h = 2166136261u;
    while ( *s )
    {
        unsigned char c = ( unsigned char )*s++;
        if ( c >= 'A' && c <= 'Z' ) c = ( unsigned char )( c + 32 );
        h = ( h ^ c ) * 16777619u;
    }
    return h;
}

static inline uint32_t
sid_hash_len( const char* str, size_t len )
{
    uint32_t h = 2166136261u;
    for ( size_t i = 0; i < len; i++ )
    {
        unsigned char c = ( unsigned char )str[ i ];
        if ( c >= 'A' && c <= 'Z' ) c = ( unsigned char )( c + 32 );
        h = ( h ^ c ) * 16777619u;
    }
    return h;
}

static inline bool
sid_equals( sid_t a, sid_t b )
{
    return a.off == b.off;
}

/*============================================================================================*/
#endif // SID_H