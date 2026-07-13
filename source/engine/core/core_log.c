/*==============================================================================================

    engine/core/core_log.c — Unity aggregator for the log subsystem + host bridge glue.

    Mechanism (ring/sinks/channel registry) and console verbs live in log/.  Types stay in
    core.h: reflect_tool's scan is non-recursive, so the REF_ENUM'd log_level_t must remain
    at the target root.  Included by core.c before core_cvar.c -- console.c and cvar_config.c
    reach the statics defined here (con_init -> log_add_sink, cvar_write_config ->
    log_channel_write_config).

==============================================================================================*/

#include "engine/core/log/log.c"
#include "engine/core/log/log_cmd.c"

/*==============================================================================================
    Log sink adapter : public

    log_fn_t-compatible bridge. Routes pre-formatted messages from sys/mod/app
    (which cannot call core() directly) into the core write path.
    Pass to mod_set_log_fn() and app_set_log_fn() after mod_init_all().
==============================================================================================*/

void
core_log_fn( int level, const char* tag, const char* msg )
{
    log_write( ( log_level_t )level, tag, "%s", msg );
}

/*============================================================================================*/
