/*==============================================================================================

    engine/rs/rs.c - Unity build entry point for the rs_ reflection library.

    engine_rs is a standalone static library with no link-time dependency on engine_core.
    It carries its own flat string pool for interning type and field names so it does not
    need sid or any other external string system.  rs_hash_str is a local static inline
    so hash computation needs no pointer indirection.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "engine/sys/sys.h"          /* lib_handle_t, sys_library_get_symbol */
#include "engine/mod/mod_export.h"   /* mod_desc_t, get_api_fn */
#include "engine/rs/rs.h"

/*==============================================================================================
    Internal string pool

    A flat bump-allocator for interning type and field names.  O(n) intern, O(1) cstr.
    Sufficient for the small, bounded set of reflected type names in a game engine session.
    Stored as a separate global so memset(&g_rs) in rs_init does not clobber live strings.
==============================================================================================*/

#define RS_STRING_POOL_SIZE (16 * 1024)

static char     g_rs_str_pool[ RS_STRING_POOL_SIZE ];
static uint32_t g_rs_str_top;

rs_name_t
rs_intern( const char* s )
{
    uint32_t len = (uint32_t)strlen( s );
    uint32_t i   = 0;
    while ( i < g_rs_str_top )
    {
        if ( strcmp( g_rs_str_pool + i, s ) == 0 ) return (rs_name_t)i;
        i += (uint32_t)strlen( g_rs_str_pool + i ) + 1;
    }
    assert( g_rs_str_top + len + 1 <= RS_STRING_POOL_SIZE && "rs string pool overflow" );
    rs_name_t id = (rs_name_t)g_rs_str_top;
    memcpy( g_rs_str_pool + g_rs_str_top, s, len + 1 );
    g_rs_str_top += len + 1;
    return id;
}

const char*
rs_cstr( rs_name_t id )
{
    return g_rs_str_pool + id;
}

/*==============================================================================================
    Registry storage  (shared across all TUs in this unity build)
==============================================================================================*/

typedef struct rs_registry_s
{
    uint16_t    type_count;
    uint16_t    field_count;
    uint16_t    attr_count;
    uint16_t    enum_count;
    uint16_t    frame_count;
    uint8_t     _pad[ 2 ];

    rs_type_t   types[ RS_MAX_TYPES ];
    rs_field_t  fields[ RS_MAX_FIELDS ];
    rs_attrib_t attrs[ RS_MAX_ATTRS ];
    rs_enum_t   enums[ RS_MAX_ENUMS ];
    rs_frame_t  frames[ RS_MAX_FRAMES ];

    uint16_t    type_hash[ RS_TYPE_HASH_SIZE ];

} rs_registry_t;

/*============================================================================================*/

#include "engine/rs/rs_registry.c"
#include "engine/rs/rs_access.c"
#include "engine/rs/rs_walk.c"
#include "engine/rs/rs_serialize.c"
#include "engine/rs/rs_print.c"

/* rs_test.c is intentionally NOT part of the library unity build.
   It is compiled as a separate TU in the test sandbox (sb_engine_core_reflect). */

/*==============================================================================================
    Module API struct
==============================================================================================*/

const rs_api_t g_rs_api_struct = {
    .find_type_by_name  = rs_find_type_by_name,
    .get_type           = rs_get_type,
    .get_field          = rs_get_field,
    .find_field         = rs_find_field,
    .type_get_attr      = rs_type_get_attr,
    .field_get_attr     = rs_field_get_attr,
    .intern             = rs_intern,
    .cstr               = rs_cstr,
    .each_type          = rs_each_type,
    .each_type_in_frame = rs_each_type_in_frame,
    .each_field         = rs_each_field,
    .each_enumerator    = rs_each_enumerator,
    .bitset_describe    = rs_bitset_describe,
    .walk_refs          = rs_walk_refs,
    .walk               = rs_walk,
    .write              = rs_write,
    .read               = rs_read,
    .peek_type_hash     = rs_peek_type_hash,
    .field_describe     = rs_field_describe,
    .print_type         = rs_print_type,
    .print_types        = rs_print_types,
};

/*==============================================================================================
    Module lifecycle
==============================================================================================*/

static bool
rs_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    rs_init();
    return true;
}

static void
rs_mod_exit( void* state )
{
    UNUSED( state );
    rs_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

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
