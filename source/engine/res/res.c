/*==============================================================================================

    engine/res/res.c - Unity entry point for the resource catalogue.

    Layout:
        res.c           storage (this file)
        res_registry.c  name pool, hash table, registration and lookup
        res_api.c       res_api_t wiring and the module descriptor (must be last)

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>

#include "engine/mod/mod_export.h"
#include "engine/res/res_host.h"

// clang-format off
/*==============================================================================================
    Storage

    One static block; zero-initialised is the valid empty catalogue.  The hash table is
    open addressing with linear probing over slot indices (+1, so 0 means empty).  Names are
    bump-allocated into the pool and never freed -- the catalogue is cumulative.
==============================================================================================*/

typedef struct res_slot_s
{
    rid_t   id;          // hash of the canonical name
    u32     name_off;    // byte offset of the NUL-terminated canonical name in pool[]
    u32     name_len;    // length excluding the NUL

} res_slot_t;

typedef struct res_registry_s
{
    u32         count;                       // slots in use
    u32         pool_top;                    // bytes of pool[] in use

    u32         hash [ RES_HASH_SIZE ];      // slot index + 1; 0 = empty
    res_slot_t  slots[ RES_MAX_ENTRIES ];    // registration order
    char        pool [ RES_NAME_POOL_SIZE ];

} res_registry_t;

static res_registry_t g_res;
static char           g_res_error[ 640 ];

ORB_STATIC_ASSERT( ( RES_HASH_SIZE & ( RES_HASH_SIZE - 1 ) ) == 0, "RES_HASH_SIZE must be a power of two" );
ORB_STATIC_ASSERT( RES_HASH_SIZE >= 2 * RES_MAX_ENTRIES,           "RES_HASH_SIZE must be >= 2x RES_MAX_ENTRIES" );

/*==============================================================================================
    Implementation Includes (Unity Build)
==============================================================================================*/

#include "engine/res/res_registry.c"

#ifndef RES_API_C_PRELUDE
#include "engine/res/res_api.c"
#endif

/*============================================================================================*/
// clang-format on
