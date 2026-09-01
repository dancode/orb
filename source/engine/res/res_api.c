/*==============================================================================================

    engine/res/res_api.c - Resource catalogue API struct and module descriptor.

    Included last by res.c so every registry function is visible to g_res_api_struct.

==============================================================================================*/
#ifndef RES_API_C_PRELUDE
    #include "orb.h"
    #include "engine/mod/mod_export.h"
    #include "engine/res/res_host.h"
#endif

/*==============================================================================================
    API Struct
==============================================================================================*/

const res_api_t g_res_api_struct = 
{
    /* Lookup */
    .name   = res_name,
    .exists = res_exists,
    .count  = res_count,
    .each   = res_each,

    /* Registration */
    .register_name  = res_register,
    .register_id    = res_register_id,
    .register_table = res_register_table,

    /* Diagnostics */
    .last_error = res_last_error,
};

/*==============================================================================================
    Module Integration

    res is a leaf with no dependencies and no per-instance state.  The catalogue is usable
    from program start (zero-initialised statics are the empty catalogue), so init only
    publishes the vtable through the standard mod gateway.  exit deliberately does NOT call
    res_exit: the catalogue is cumulative and outlives the modules that named into it, so its
    storage is held for the lifetime of the program.
==============================================================================================*/

static bool
res_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    return true;
}

static void
res_mod_exit( void* state )
{
    UNUSED( state );
}

mod_desc_t*
res_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( res_api_t ),
        .func_api      = ( void* )&g_res_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = res_mod_init,
        .exit          = res_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/
