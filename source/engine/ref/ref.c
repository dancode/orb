/*==============================================================================================

    engine/ref/ref.c - Unity entry point. See ref.md for architecture overview.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "engine/mod/mod_export.h"

#include "engine/ref/ref_host.h"
#include "engine/ref/ref_import.h"

// clang-format off
/*==============================================================================================
    String Pool

    Flat bump-allocator for interning type/field names. Kept separate from g_ref so that
    memset(&g_ref) in ref_init does not clobber live strings. O(n) intern, O(1) cstr.

    ref_name_t is a u32 byte offset into g_ref_str_pool -- the name "id" is literally the
    offset, so ref_cstr is a single pointer add. Equal strings always produce the same id
    within a session, which lets field/type names be compared by value instead of strcmp.
==============================================================================================*/

#define REF_STRING_POOL_SIZE ( 16 * 1024 )

static char     g_ref_str_pool[ REF_STRING_POOL_SIZE ];
static uint32_t g_ref_str_top;

ref_name_t
ref_intern( const char* s )
{
    uint32_t len = ( uint32_t )strlen( s );
    uint32_t i   = 0;

    /* Linear scan: pool is small (16 KB) and type counts are low, so O(n) is acceptable.
       Returns the existing offset immediately on a hit to guarantee stable ids. */
    while ( i < g_ref_str_top )
    {
        if ( strcmp( g_ref_str_pool + i, s ) == 0 )
            return ( ref_name_t )i;
        i += ( uint32_t )strlen( g_ref_str_pool + i ) + 1;
    }

    /* Bump-allocate the new string; null terminator is included in the copy. */
    if ( g_ref_str_top + len + 1 > REF_STRING_POOL_SIZE )
    {
        fprintf( stderr, "ref: FATAL string pool overflow (used %u + need %u > limit %d) -- increase REF_STRING_POOL_SIZE in ref.c\n",
                 g_ref_str_top, len + 1, REF_STRING_POOL_SIZE );
        assert( 0 && "ref: string pool overflow -- increase REF_STRING_POOL_SIZE in ref.c" );
        return 0;
    }
    ref_name_t id = ( ref_name_t )g_ref_str_top;
    memcpy( g_ref_str_pool + g_ref_str_top, s, len + 1 );
    g_ref_str_top += len + 1;
    return id;
}

const char*
ref_cstr( ref_name_t id )
{
    /* id is a direct byte offset -- O(1) pointer add, no table lookup. */
    return g_ref_str_pool + id;
}

/*==============================================================================================
    Registry Storage

    All reflection data lives in a single global struct so that the entire system can be
    reset with one memset. The five arrays are flat pools indexed by uint16_t ids; frames
    mark watermarks into each pool so a module unload just rewinds the count.

    type_hash[] is an open-addressing chain table: types[i].next forms the bucket chains,
    so no separate link storage is needed (the type record itself is the list node).
==============================================================================================*/

typedef struct ref_registry_s
{
    uint16_t    type_count;
    uint16_t    field_count;
    uint16_t    attr_count;
    uint16_t    enum_count;
    uint16_t    frame_count;
    uint8_t     _pad[ 2 ];

    ref_type_t   types       [ REF_MAX_TYPES ];
    ref_field_t  fields      [ REF_MAX_FIELDS ];
    ref_attrib_t attrs       [ REF_MAX_ATTRS ];
    ref_enum_t   enums       [ REF_MAX_ENUMS ];
    ref_frame_t  frames      [ REF_MAX_FRAMES ];

    /* Hash table: maps name_hash -> type_id; bucket chains use ref_type_t.next. */
    uint16_t    type_hash[   REF_TYPE_HASH_SIZE ];

} ref_registry_t;

static ref_registry_t    g_ref;
static const bool       ref_debug = true;

/*==============================================================================================
    Implementation Includes (Unity Build)
==============================================================================================*/

#include "engine/ref/ref_registry.c"
#include "engine/ref/ref_access.c"
#include "engine/ref/ref_walk.c"
#include "engine/ref/ref_serialize.c"
#include "engine/ref/ref_print.c"

/* ref_test.c is compiled separately as part of sb_reflect, not this library. */

/*==============================================================================================
    API Publishing

    g_ref_api_struct is the vtable that DLL modules receive when they call MOD_FETCH_API(ref_api_t).
    Every function pointer here is a direct-call entry point compiled into this TU via
    the unity includes above -- no indirection cost at the call site beyond the pointer load.
    The struct is const because modules never write to it; they cache a pointer in their own state.
==============================================================================================*/

const ref_api_t g_ref_api_struct =
{
     /* Lookup */
    .find_type_by_name  = ref_find_type_by_name,
    .get_type           = ref_get_type,
    .get_field          = ref_get_field,
    .find_field         = ref_find_field,
    .type_get_attr      = ref_type_get_attr,
    .field_get_attr     = ref_field_get_attr,
    .type_get_attr_values  = ref_type_get_attr_values,
    .field_get_attr_values = ref_field_get_attr_values,
    .intern             = ref_intern,
    .cstr               = ref_cstr,

    /* Iteration */
    .each_type          = ref_each_type,
    .each_type_in_frame = ref_each_type_in_frame,
    .each_field         = ref_each_field,
    .each_enumerator    = ref_each_enumerator,

    /* Bitset helpers */
    .bitset_describe    = ref_bitset_describe,

    /* Union discriminant */
    .union_case_field   = ref_union_case_field,

    /* Walkers */
    .walk_refs          = ref_walk_refs,
    .walk               = ref_walk,

    /* Serialization */
    .write              = ref_write,
    .read               = ref_read,
    .peek_type_hash     = ref_peek_type_hash,

    /* Diagnostics */
    .field_describe     = ref_field_describe,
    .print_type         = ref_print_type,
    .print_types        = ref_print_types,
};

/*==============================================================================================
    Module Integration
==============================================================================================*/

static bool
ref_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    /* ref is a leaf module with no dependencies. The registry initializes lazily via
       ref_ensure_init() the first time any registration function is called, so this
       init callback's only job is to publish ref_api_t through the standard mod gateway.
       core declares "ref" as a dependency so the mod system loads ref before core, guaranteeing
       the vtable is live when core -- and every module thereafter -- fetches it. */
    return true;
}

static void
ref_mod_exit( void* state )
{
    UNUSED( state );
    ref_exit();
}

mod_desc_t*
ref_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( ref_api_t ),
        .func_api      = ( void* )&g_ref_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = ref_mod_init,
        .exit          = ref_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
// clang-format on
