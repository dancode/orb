/*==============================================================================================

    engine/core/log/log_cmd.c - Log Channel Commands

    User-facing console verbs over the channel registry in log.c (same TU), plus the
    writeconfig section writer.  cmd_register/con_printf declarations arrive TU-wide
    through core.c's core_host.h include.

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

/*==============================================================================================
    Config section writer  (called by cvar_write_config, later in the TU)
==============================================================================================*/

/* Write "log reset" + one "log <channel> <level>" line per override, mirroring the bind
   writer's unbindall + bind lines: the unconditional reset keeps a lower-tier config file
   (default.cfg) from leaking overrides into a session whose config.cfg has none.
   void* keeps stdio out of the seam, same convention as cmd_bind_write_config. */

static void
log_channel_write_config( void* file )
{
    FILE* f     = ( FILE* )file;
    u32   count = log_channel_count();

    fprintf( f, "\nlog reset\n" );

    for ( u32 i = 0; i < count; i++ )
    {
        const char* name     = NULL;
        log_level_t override = LOG_LEVEL_INHERIT;
        log_channel_get( i, &name, &override, NULL );
        if ( override == LOG_LEVEL_INHERIT )
            continue;

        fprintf( f, "log %s %s\n", name, log_cmd_level_name( override ) );
    }
}

/*==============================================================================================
    Command Registration
==============================================================================================*/

/* Non-static so sandboxes that assemble core subsystems manually can register the
   command without core_init (see sb_core); declared in core_host.h. */

void
log_register_commands( void )
{
    cmd_register( "log", cmd_log, "Set or list per-channel log levels" );
}

/*============================================================================================*/
