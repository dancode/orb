/*==============================================================================================

    engine/prof/prof_api.c - Profiler API struct and module descriptor.

    Included last by prof.c so every implementation function is visible in the TU.

==============================================================================================*/
#ifndef PROF_API_C_PRELUDE
#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/prof/prof_host.h"
#endif

/*==============================================================================================
    API Struct
==============================================================================================*/

const prof_api_t g_prof_api_struct =
{
    /* Zones */
    .zone_begin      = prof_zone_begin,
    .zone_begin_name = prof_zone_begin_name,
    .zone_end        = prof_zone_end,
    .name_register   = prof_name_register,
    .name_lookup     = prof_name_lookup,

    /* Frame + counters */
    .frame_mark      = prof_frame_mark,
    .frame_number    = prof_frame_number,
    .counter_set     = prof_counter_set,
    .counter_add     = prof_counter_add,
    .counters        = prof_counters,

    /* Capture control */
    .set_enabled     = prof_set_enabled,
    .is_enabled      = prof_is_enabled,
    .thread_name     = prof_thread_name,
    .thread_release  = prof_thread_release,

    /* Drain */
    .thread_count    = prof_thread_count,
    .thread_label    = prof_thread_label,
    .thread_dropped  = prof_thread_dropped,
    .drain           = prof_drain,

    /* Chrome-trace dump */
    .dump_begin      = prof_dump_begin,
    .dump_flush      = prof_dump_flush,
    .dump_end        = prof_dump_end,
    .dump_active     = prof_dump_active,
};

/*==============================================================================================
    Module Integration
==============================================================================================*/

static bool
prof_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    /* prof is a leaf module (deps: sys, statically linked alongside it in the host). All
       internals boot lazily on first capture, so init only publishes prof_api_t through
       the standard gateway for every module loaded after it. */
    prof_init();
    return true;
}

static void
prof_mod_exit( void* state )
{
    UNUSED( state );
    prof_exit();
}

mod_desc_t*
prof_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( prof_api_t ),
        .func_api      = ( void* )&g_prof_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = prof_mod_init,
        .exit          = prof_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/
