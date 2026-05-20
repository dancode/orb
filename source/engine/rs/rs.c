/*==============================================================================================

    engine/rs/rs.c - Unity entry point. See rs.md for architecture overview.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "engine/mod/mod_export.h"

#include "engine/rs/rs_host.h"
#include "engine/rs/rs_import.h"

// clang-format off
/*==============================================================================================
    String Pool

    Flat bump-allocator for interning type/field names. Kept separate from g_rs so that
    memset(&g_rs) in rs_init does not clobber live strings. O(n) intern, O(1) cstr.

    rs_name_t is a u32 byte offset into g_rs_str_pool -- the name "id" is literally the
    offset, so rs_cstr is a single pointer add. Equal strings always produce the same id
    within a session, which lets field/type names be compared by value instead of strcmp.
==============================================================================================*/

#define RS_STRING_POOL_SIZE ( 16 * 1024 )

static char     g_rs_str_pool[ RS_STRING_POOL_SIZE ];
static uint32_t g_rs_str_top;

rs_name_t
rs_intern( const char* s )
{
    uint32_t len = ( uint32_t )strlen( s );
    uint32_t i   = 0;

    /* Linear scan: pool is small (16 KB) and type counts are low, so O(n) is acceptable.
       Returns the existing offset immediately on a hit to guarantee stable ids. */
    while ( i < g_rs_str_top )
    {
        if ( strcmp( g_rs_str_pool + i, s ) == 0 )
            return ( rs_name_t )i;
        i += ( uint32_t )strlen( g_rs_str_pool + i ) + 1;
    }

    /* Bump-allocate the new string; null terminator is included in the copy. */
    assert( g_rs_str_top + len + 1 <= RS_STRING_POOL_SIZE && "rs string pool overflow" );
    rs_name_t id = ( rs_name_t )g_rs_str_top;
    memcpy( g_rs_str_pool + g_rs_str_top, s, len + 1 );
    g_rs_str_top += len + 1;
    return id;
}

const char*
rs_cstr( rs_name_t id )
{
    /* id is a direct byte offset -- O(1) pointer add, no table lookup. */
    return g_rs_str_pool + id;
}

/*==============================================================================================
    Registry Storage

    All reflection data lives in a single global struct so that the entire system can be
    reset with one memset. The five arrays are flat pools indexed by uint16_t ids; frames
    mark watermarks into each pool so a module unload just rewinds the count.

    type_hash[] is an open-addressing chain table: types[i].next forms the bucket chains,
    so no separate link storage is needed (the type record itself is the list node).
==============================================================================================*/

typedef struct rs_registry_s
{
    uint16_t    type_count;
    uint16_t    field_count;
    uint16_t    attr_count;
    uint16_t    enum_count;
    uint16_t    frame_count;
    uint8_t     _pad[ 2 ];

    rs_type_t   types       [ RS_MAX_TYPES ];
    rs_field_t  fields      [ RS_MAX_FIELDS ];
    rs_attrib_t attrs       [ RS_MAX_ATTRS ];
    rs_enum_t   enums       [ RS_MAX_ENUMS ];
    rs_frame_t  frames      [ RS_MAX_FRAMES ];

    /* Hash table: maps name_hash -> type_id; bucket chains use rs_type_t.next. */
    uint16_t    type_hash[   RS_TYPE_HASH_SIZE ];

} rs_registry_t;

static rs_registry_t    g_rs;
static const bool       rs_debug = true;

/*==============================================================================================
    Implementation Includes (Unity Build)
==============================================================================================*/

#include "engine/rs/rs_registry.c"
#include "engine/rs/rs_access.c"
#include "engine/rs/rs_walk.c"
#include "engine/rs/rs_serialize.c"
#include "engine/rs/rs_print.c"

/* rs_test.c is compiled separately as part of sb_engine_reflect, not this library. */

/*==============================================================================================
    API Publishing

    g_rs_api_struct is the vtable that DLL modules receive when they call MOD_FETCH_API(rs_api_t).
    Every function pointer here is a direct-call entry point compiled into this TU via
    the unity includes above -- no indirection cost at the call site beyond the pointer load.
    The struct is const because modules never write to it; they cache a pointer in their own state.
==============================================================================================*/

const rs_api_t g_rs_api_struct =
{
     /* Lookup */
    .find_type_by_name  = rs_find_type_by_name,
    .get_type           = rs_get_type,
    .get_field          = rs_get_field,
    .find_field         = rs_find_field,
    .type_get_attr      = rs_type_get_attr,
    .field_get_attr     = rs_field_get_attr,
    .intern             = rs_intern,
    .cstr               = rs_cstr,

    /* Iteration */
    .each_type          = rs_each_type,
    .each_type_in_frame = rs_each_type_in_frame,
    .each_field         = rs_each_field,
    .each_enumerator    = rs_each_enumerator,

    /* Bitset helpers */
    .bitset_describe    = rs_bitset_describe,

    /* Walkers */
    .walk_refs          = rs_walk_refs,
    .walk               = rs_walk,

    /* Serialization */
    .write              = rs_write,
    .read               = rs_read,
    .peek_type_hash     = rs_peek_type_hash,

    /* Diagnostics */
    .field_describe     = rs_field_describe,
    .print_type         = rs_print_type,
    .print_types        = rs_print_types,
};

/*==============================================================================================
    Module Integration
==============================================================================================*/

static bool
rs_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    /* rs is a leaf module with no dependencies. The registry initializes lazily via
       rs_ensure_init() the first time any registration function is called, so this
       init callback's only job is to publish rs_api_t through the standard mod gateway.
       core declares "rs" as a dependency so the mod system loads rs before core, guaranteeing
       the vtable is live when core -- and every module thereafter -- fetches it. */
    return true;
}

static void
rs_mod_exit( void* state )
{
    UNUSED( state );
    rs_exit();
}

mod_desc_t*
rs_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( rs_api_t ),
        .func_api      = ( void* )&g_rs_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = rs_mod_init,
        .exit          = rs_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
// clang-format on
