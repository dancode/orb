/*==============================================================================================

    cvar_system.c

==============================================================================================*/

#include "engine/core/cvar/string_pool.h"
#include "engine/core/cvar/string_pool.c"

#include "engine/core/cvar/cvar.h"
#include "engine/core/cvar/cvar_hash.c"
#include "engine/core/cvar/cvar_priority.c"
#include "engine/core/cvar/cvar_callback.c"
#include "engine/core/cvar/cvar_register.c"
#include "engine/core/cvar/cvar.c"
#include "engine/core/cvar/cvar_cmd.c"

#include "engine/core/cmd/cmd.h"
#include "engine/core/cmd/cmd_buffer.c"
#include "engine/core/cmd/cmd_bind.c"
#include "engine/core/cmd/cmd_alias.c"
#include "engine/core/cmd/cmd.c"

#include "engine/core/cvar/cvar_config.c"

#include "engine/core/console/console.h"
#include "engine/core/console/console.c"

#include "engine/core/test/test_core_cvar.c"

/*==============================================================================================

    Core-owned 'log' command -- per-channel runtime verbosity.

    The registry lives in core_log.c; its statics are visible here (same unity TU, included
    earlier by core.c), matching how console.c reaches log_add_sink directly.

    Usage:
        log list                    list channels with override + effective level
        log <channel>               print one channel's state
        log <channel> <level|off>   set override (trace|debug|info|warn|error|off or 0-4)
        log <channel> reset         clear override (channel follows log_level again)
        log reset                   clear all overrides

==============================================================================================*/

static const char* s_log_level_names[] = { "trace", "debug", "info", "warn", "error" };

static const char*
log_cmd_level_name( log_level_t level )
{
    if ( level == LOG_LEVEL_OFF )     return "off";
    if ( level == LOG_LEVEL_INHERIT ) return "-";
    if ( level <= LOG_LEVEL_ERROR )   return s_log_level_names[ level ];
    return "?";
}

static bool
log_cmd_parse_level( const char* s, log_level_t* out )
{
    for ( u32 i = 0; i <= LOG_LEVEL_ERROR; i++ )
    {
        if ( strcmp( s, s_log_level_names[ i ] ) == 0 )
        {
            *out = ( log_level_t )i;
            return true;
        }
    }
    if ( strcmp( s, "off" ) == 0 )
    {
        *out = LOG_LEVEL_OFF;
        return true;
    }
    if ( s[ 0 ] >= '0' && s[ 0 ] <= '4' && s[ 1 ] == '\0' )
    {
        *out = ( log_level_t )( s[ 0 ] - '0' );
        return true;
    }
    return false;
}

static void
cmd_log( int argc, char** argv )
{
    if ( argc < 2 )
    {
        con_printf( "Usage: log list | log reset | log <channel> [level|off|reset]\n" );
        con_printf( "  Levels: trace debug info warn error (0-4). Channels register on first use.\n" );
        return;
    }

    if ( strcmp( argv[ 1 ], "list" ) == 0 )
    {
        u32 count = log_channel_count();
        con_printf( "\n%-24s %-9s %s\n", "channel", "override", "effective" );
        con_printf( "------------------------------------------\n" );
        for ( u32 i = 0; i < count; i++ )
        {
            const char* name      = NULL;
            log_level_t override  = LOG_LEVEL_INHERIT;
            log_level_t effective = LOG_LEVEL_INFO;
            log_channel_get( i, &name, &override, &effective );
            con_printf( "%-24s %-9s %s\n", name, log_cmd_level_name( override ),
                        log_cmd_level_name( effective ) );
        }
        con_printf( "------------------------------------------\n" );
        con_printf( "%u channel(s), global floor %s (log_level)\n\n",
                    count, log_cmd_level_name( g_min_level ) );
        return;
    }

    if ( strcmp( argv[ 1 ], "reset" ) == 0 )
    {
        log_channel_reset( NULL );
        con_printf( "All log channel overrides cleared\n" );
        return;
    }

    const char* name = argv[ 1 ];

    if ( argc == 2 )    /* query form, like a bare cvar name */
    {
        log_channel_t* ch = log_channel_find( name );
        if ( !ch )
        {
            con_printf( "No log channel '%s' registered yet\n", name );
            return;
        }
        log_level_t override  = ( log_level_t )ch->override;
        log_level_t effective = ( override != LOG_LEVEL_INHERIT ) ? override : g_min_level;
        con_printf( "log channel '%s': override %s, effective %s\n",
                    ch->name, log_cmd_level_name( override ), log_cmd_level_name( effective ) );
        return;
    }

    if ( strcmp( argv[ 2 ], "reset" ) == 0 )
    {
        log_channel_reset( name );
        con_printf( "log channel '%s' reset to inherit\n", name );
        return;
    }

    log_level_t level;
    if ( !log_cmd_parse_level( argv[ 2 ], &level ) )
    {
        con_printf( "Unknown log level '%s' (trace|debug|info|warn|error|off or 0-4)\n", argv[ 2 ] );
        return;
    }

    log_channel_set( name, level );
    con_printf( "log channel '%s' -> %s\n", name, log_cmd_level_name( level ) );
}

/* Non-static so sandboxes that assemble core subsystems manually can register the
   command without core_init (see sb_core); declared in core_host.h. */

void
log_register_commands( void )
{
    cmd_register( "log", cmd_log, "Set or list per-channel log levels" );
}

/*==============================================================================================

    Core-owned cvars -- registered by core_init after cvar_system_init/con_init.

==============================================================================================*/

/* log_level tracks the runtime log filter: setting it from the console retunes the logger. */

static void
core_cvar_log_level_changed( cvar_t* cv )
{
    log_set_min_level( ( log_level_t )cvar_get_int( cv ) );
}

/* con_log_level tracks the console's ambient-log intake floor (see con_set_log_filter) --
   separate from log_level above, which gates what reaches the log ring/sinks at all. */

static void
core_cvar_con_log_level_changed( cvar_t* cv )
{
    con_set_log_filter( ( log_level_t )cvar_get_int( cv ) );
}

static void
core_register_cvars( void )
{
    cvar_register_r( "version", "Engine version string", "ORB 0.1.0", CVAR_ROM );
    cvar_register_b( "developer", "Enable developer diagnostics", false, 0 );

    cvar_t* cv = cvar_register_i(
        "log_level", "Minimum log level (0=trace 1=debug 2=info 3=warn 4=error)",
        LOG_LEVEL_INFO, LOG_LEVEL_TRACE, LOG_LEVEL_ERROR, 0 );

    cvar_callback_register( cv, core_cvar_log_level_changed );

    cvar_t* cv_con = cvar_register_i(
        "con_log_level", "Minimum ambient log level shown in the console (0=trace 1=debug 2=info 3=warn 4=error)",
        LOG_LEVEL_WARN, LOG_LEVEL_TRACE, LOG_LEVEL_ERROR, 0 );

    cvar_callback_register( cv_con, core_cvar_con_log_level_changed );
}

/*============================================================================================*/
