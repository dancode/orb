/*==============================================================================================

    engine/core/core_log.c — Logging subsystem implementation.

    v0: stdout/stderr only, single-threaded, no timestamps, no file output.
    These can be added without changing the public API.

==============================================================================================*/
/*==============================================================================================
    State
==============================================================================================*/

static log_level_t g_min_level = LOG_LEVEL_INFO;

/*==============================================================================================
    Lifecycle
==============================================================================================*/

void
log_init( void )
{
    /* placeholder for future file-handle open, mutex init, etc. */
}

void
log_exit( void )
{
    /* placeholder for future file-handle close */
}

/*==============================================================================================
    Configuration
==============================================================================================*/

void
log_set_min_level( log_level_t level )
{
    g_min_level = level;
}

/*==============================================================================================
    Emission
==============================================================================================*/

static void
log_emit( log_level_t level, const char* prefix, FILE* stream, const char* fmt, va_list ap )
{
    if ( level < g_min_level )
        return;

    fputs( prefix, stream );
    vfprintf( stream, fmt, ap );
    fputc( '\n', stream );
}

void
log_default( const char* fmt, ... )
{
    /* defaults to log info level */

    va_list ap;
    va_start( ap, fmt );
    log_emit( LOG_LEVEL_INFO, "[log ] ", stdout, fmt, ap );
    va_end( ap );
}

void
log_info( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    log_emit( LOG_LEVEL_INFO, "[info ] ", stdout, fmt, ap );
    va_end( ap );
}

void
log_warn( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    log_emit( LOG_LEVEL_WARN, "[warn ] ", stderr, fmt, ap );
    va_end( ap );
}

void
log_error( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    log_emit( LOG_LEVEL_ERROR, "[error] ", stderr, fmt, ap );
    va_end( ap );
}

/*==============================================================================================
    Log Sink Adapter

    Matches log_fn_t so other engine libraries can route their output through core.
    The tag is prefixed in brackets; the message is forwarded to the appropriate level.
==============================================================================================*/

void
core_log_fn( int level, const char* tag, const char* msg )
{
    switch ( level )
    {
        case LOG_LEVEL_WARN:  log_warn( "[%s] %s", tag, msg );  break;
        case LOG_LEVEL_ERROR: log_error( "[%s] %s", tag, msg ); break;
        default:              log_info( "[%s] %s", tag, msg );  break;
    }
}

/*============================================================================================*/
