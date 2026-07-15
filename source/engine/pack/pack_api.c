/*==============================================================================================

    engine/pack/pack_api.c -- pack API struct and module descriptor.

    Included last by pack.c so every implementation function is visible in the TU.

==============================================================================================*/
#ifndef PACK_API_C_PRELUDE
#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/pack/pack_host.h"
#endif

/*==============================================================================================
    API Struct
==============================================================================================*/

const pack_api_t g_pack_api_struct =
{
    .bound   = pack_bound,
    .deflate = pack_deflate,
    .inflate = pack_inflate,
    .crc32   = pack_crc32,
};

/*==============================================================================================
    Module Integration
==============================================================================================*/

static bool
pack_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    /* pack is stateless (pure memory transforms) -- registration only publishes pack_api_t
       through the standard gateway for every module loaded after it. */
    return true;
}

static void
pack_mod_exit( void* state )
{
    UNUSED( state );
}

mod_desc_t*
pack_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( pack_api_t ),
        .func_api      = ( void* )&g_pack_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = pack_mod_init,
        .exit          = pack_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/
