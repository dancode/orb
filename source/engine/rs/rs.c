/*==============================================================================================

    engine/rs/rs.c - Unity entry point. See rs.md for architecture overview.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "engine/mod/mod_export.h"
#include "engine/rs/rs.h"

/* Flat bump-allocator for interning type/field names. Kept separate from g_rs so that
   memset(&g_rs) in rs_init does not clobber live strings. O(n) intern, O(1) cstr. */

#define RS_STRING_POOL_SIZE ( 16 * 1024 )

static char     g_rs_str_pool[ RS_STRING_POOL_SIZE ];
static uint32_t g_rs_str_top;

rs_name_t
rs_intern( const char* s )
{
    uint32_t len = ( uint32_t )strlen( s );
    uint32_t i   = 0;
    while ( i < g_rs_str_top )
    {
        if ( strcmp( g_rs_str_pool + i, s ) == 0 )
            return ( rs_name_t )i;
        i += ( uint32_t )strlen( g_rs_str_pool + i ) + 1;
    }
    assert( g_rs_str_top + len + 1 <= RS_STRING_POOL_SIZE && "rs string pool overflow" );
    rs_name_t id = ( rs_name_t )g_rs_str_top;
    memcpy( g_rs_str_pool + g_rs_str_top, s, len + 1 );
    g_rs_str_top += len + 1;
    return id;
}

// clang-format off

const char*
rs_cstr( rs_name_t id )
{
    return g_rs_str_pool + id;
}

/* Registry storage — shared across all TUs in this unity build */
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

    uint16_t    type_hash[   RS_TYPE_HASH_SIZE ];

} rs_registry_t;

/*============================================================================================*/

#include "engine/rs/rs_registry.c"
#include "engine/rs/rs_access.c"
#include "engine/rs/rs_walk.c"
#include "engine/rs/rs_serialize.c"
#include "engine/rs/rs_print.c"

/* rs_test.c is compiled separately as part of sb_engine_reflect, not this library. */

/*============================================================================================*/

const rs_api_t g_rs_api_struct = 
{
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
    Type Record  (28 bytes)
==============================================================================================*/

static bool
rs_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    /* Registry self-bootstraps via rs_ensure_init() — this is a no-op whose only purpose
       is to publish rs_api_t through the standard module gateway. */
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