/*==============================================================================================

    runtime_service/gui/log/gui_log_core.c -- the diagnostics floor's one implementation.

    Compiled into the gui_log.c unit root.  Holds the sink pointer, formats the message, and
    dispatches.  Nothing else in the gui writes to stdout directly.

==============================================================================================*/
// clang-format off

/* The sink, and the user pointer handed back to it.  A plain static rather than context state:
   diagnostics predate the first context and outlive the last one, and the boot-path messages
   this exists to capture are emitted before any context is created. */
static gui_log_fn s_log_fn;
static void*      s_log_user;

void
gui_log_set_fn( gui_log_fn fn, void* user )
{
    s_log_fn   = fn;
    s_log_user = user;
}

/*==============================================================================================
    Format + dispatch.

    The message is built on the stack -- no allocation on a diagnostic path, which may be a
    pool-exhaustion report where allocating is exactly the wrong move.  fmt_vsnprintf is the
    engine's bounded stb_sprintf (base/fmt.h): locale-free, always NUL-terminating, and it does
    not call into the CRT's debug-heap formatting the way the /MDd snprintf does.
==============================================================================================*/

void
gui_log( gui_log_level_t level, const char* fmt, ... )
{
    char    msg[ GUI_LOG_MSG_MAX ];
    va_list args;

    va_start( args, fmt );
    int n = fmt_vsnprintf( msg, (int)sizeof( msg ), fmt, args );
    va_end( args );

    if ( n < 0 )
        return;                                  /* malformed format; nothing safe to report */

    /* Clip to what actually landed: fmt_vsnprintf returns the WOULD-BE length, so a truncated
       message reports past the buffer. */
    u32 len = ( (u32)n < sizeof( msg ) ) ? (u32)n : (u32)( sizeof( msg ) - 1u );

    /* Strip the trailing newline the call sites carry.  The sink owns its own framing, so the
       migrated printf format strings keep their "\n" and still deliver a clean message. */
    while ( len > 0u && ( msg[ len - 1u ] == '\n' || msg[ len - 1u ] == '\r' ) )
        msg[ --len ] = '\0';

    if ( s_log_fn )
    {
        s_log_fn( level, msg, s_log_user );
        return;
    }

    /* Default sink -- the pre-hook behavior, unchanged: the "[gui] " prefix every one of these
       messages used to carry inline, and the flush the boot-path diagnostics depend on (an
       unflushed warning going unnoticed is how a silent font-upload failure survived once). */
    printf( "[gui] %s%s\n", ( level == GUI_LOG_WARN  ) ? "WARNING: "
                          : ( level == GUI_LOG_ERROR ) ? "ERROR: " : "", msg );
    fflush( stdout );
}

/*============================================================================================*/
