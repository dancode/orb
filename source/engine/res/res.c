/*==============================================================================================

    engine/res/res.c - Unity entry point for the resource catalogue.

    Layout:
        res.c           storage (this file)
        res_registry.c  name pool, hash table, registration and lookup
        res_api.c       res_api_t wiring and the module descriptor (must be last)

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/mod/mod_export.h"
#include "engine/res/res_host.h"

// clang-format off
/*==============================================================================================
    Storage

    Three heap blocks that double as they fill, on the sid model (core/sid/sid.c): an open
    addressing hash table of slot indices (+1, so 0 means empty), a slot array in registration
    order, and a bump-allocated pool of canonical name text.

    All three are NULL at program start and that IS the valid empty catalogue -- the first
    registration allocates.  They are never released short of res_exit: the catalogue is
    cumulative, res is statically linked into the host and never hot-reloads, so its storage
    is good for the lifetime of the program.

    Slots hold pool OFFSETS rather than pointers, so growing the pool needs no fixup pass.
    Byte 0 of the pool is a reserved NUL: offset 0 is the empty string, which is how a slot
    with no cooked path reads back as "" without spending a byte per entry.  Names always
    land above it.
    The hash table is rebuilt on growth, and always holds twice slot_cap so the load factor
    stays at or under 50% and a linear probe always terminates on an empty bucket.

    Bucket indices are u16, which is what caps the catalogue at RES_MAX_ENTRIES.  Every
    bucket pays for that width whether or not it is occupied, and 64K names is far past
    anything a game here will catalogue.
==============================================================================================*/

typedef struct res_slot_s
{
    rid_t   id;          // hash of the canonical name
    u32     name_off;    // byte offset of the NUL-terminated canonical name in pool
    u32     path_off;    // byte offset of the cooked relative path in pool; 0 = none

} res_slot_t;

typedef struct res_registry_s
{
    u16*        hash;         // hash_size buckets: slot index + 1; 0 = empty
    res_slot_t* slots;        // slot_cap entries, registration order
    char*       pool;         // pool_cap bytes of name text

    u32         hash_size;    // power of two, always 2 * slot_cap
    u32         slot_cap;     // slots allocated
    u32         slot_count;   // slots in use
    u32         pool_cap;     // pool bytes allocated
    u32         pool_top;     // pool bytes in use

} res_registry_t;

static res_registry_t   g_res;
static char             g_res_error[ 640 ];     // fits a collision report: two RES_NAME_MAX names

ORB_STATIC_ASSERT(( RES_INIT_ENTRIES & ( RES_INIT_ENTRIES - 1 )) == 0, "RES_INIT_ENTRIES must be a power of two" );
ORB_STATIC_ASSERT(( RES_MAX_ENTRIES  & ( RES_MAX_ENTRIES  - 1 )) == 0, "RES_MAX_ENTRIES must be a power of two" );
ORB_STATIC_ASSERT( RES_MAX_ENTRIES <= 65535, "slot index + 1 must fit a u16 bucket" );

/*==============================================================================================
    Implementation Includes (Unity Build)
==============================================================================================*/

#include "engine/res/res_registry.c"

#ifndef RES_API_C_PRELUDE
#include "engine/res/res_api.c"
#endif

/*============================================================================================*/
// clang-format on
