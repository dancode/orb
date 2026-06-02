/*==============================================================================================

    engine/ref/ref_api.c - Reflection API struct and module descriptor.

    Included last by ref.c. By the time this file is processed in the unity build, every
    subsystem's static functions are visible in the TU and can be assigned to g_ref_api_struct.

==============================================================================================*/
#ifndef REF_API_C_PRELUDE
#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/ref/ref_host.h"
#endif

/*==============================================================================================
    API Struct
==============================================================================================*/

const ref_api_t g_ref_api_struct =
{
    /* Lookup */
    .find_type_by_name      = ref_find_type_by_name,
    .get_type               = ref_get_type,
    .get_field              = ref_get_field,
    .find_field             = ref_find_field,
    .type_get_attr          = ref_type_get_attr,
    .field_get_attr         = ref_field_get_attr,
    .type_get_attr_values   = ref_type_get_attr_values,
    .field_get_attr_values  = ref_field_get_attr_values,
    .intern                 = ref_intern,
    .cstr                   = ref_cstr,

    /* Iteration */
    .each_type              = ref_each_type,
    .each_type_in_frame     = ref_each_type_in_frame,
    .each_field             = ref_each_field,
    .each_enumerator        = ref_each_enumerator,

    /* Bitset helpers */
    .bitset_describe        = ref_bitset_describe,

    /* Union discriminant */
    .union_case_field       = ref_union_case_field,

    /* Walkers */
    .walk_refs              = ref_walk_refs,
    .walk                   = ref_walk,

    /* Serialization */
    .write                  = ref_write,
    .read                   = ref_read,
    .peek_type_hash         = ref_peek_type_hash,

    /* Diagnostics */
    .field_describe         = ref_field_describe,
    .print_type             = ref_print_type,
    .print_types            = ref_print_types,
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
