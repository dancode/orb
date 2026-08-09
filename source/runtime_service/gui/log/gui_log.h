#ifndef GUI_LOG_H
#define GUI_LOG_H
/*==============================================================================================

    runtime_service/gui/log/gui_log.h -- GUI_LOG: the leaf diagnostics floor.

    Every diagnostic the gui emits -- pool overflows, load failures, contract violations, the
    stats dumps -- goes through gui_log().  It is the BOTTOM of the stack: it depends on nothing
    but base types, so any unit can call it, and it holds one function pointer so a host can
    route the whole subsystem somewhere other than stdout.

    WHY A HOOK RATHER THAN THE ENGINE LOG: gui deps are { rhi, app } -- deliberately not core --
    so core's LOG_* macros are out of reach and raw printf was the only floor available.  That
    cost the gui everything a log gives you: routing, file capture, severity filtering, and
    suppression in a shipped build.  A sink the host installs buys all of it back without gui
    taking a core dependency; the host binds it in one line and the dep graph is unchanged.

    THE DEFAULT SINK is printf + fflush, so a gui that is never wired behaves exactly as it did
    before -- including reaching a plain console before any engine log is up, which is the case
    the boot-path diagnostics depend on.  Installing a sink is optional, not required.

    MESSAGE CONTRACT (what a sink receives):
        - one message, NUL-terminated, already formatted
        - NO "[gui] " prefix and NO trailing newline -- gui_log strips one if the format string
          carries it, so the sink owns its own framing (a core-log sink adds a category, the
          default sink adds the prefix and the newline)
        - truncated at GUI_LOG_MSG_MAX rather than split or dropped

    Install BEFORE gui()->init() to catch the init-path diagnostics; the sink is a plain static,
    so there is no ordering constraint beyond that.

==============================================================================================*/

#include "orb.h"

// clang-format off
/*==============================================================================================
    Severity + sink
==============================================================================================*/

typedef enum
{
    GUI_LOG_INFO  = 0,   // progress and stats: a font loaded, a dump was requested
    GUI_LOG_WARN  = 1,   // degraded but running: a pool saturated, an asset was rejected
    GUI_LOG_ERROR = 2,   // the call failed and returned: a contract violation at init

} gui_log_level_t;

/* Host-installed sink.  `msg` is valid only for the duration of the call -- copy to retain. */
typedef void ( *gui_log_fn )( gui_log_level_t level, const char* msg, void* user );

#define GUI_LOG_MSG_MAX 1024   /* formatted message cap, including the NUL */

/*==============================================================================================
    The floor (log/gui_log_core.c)
==============================================================================================*/

/* Format and dispatch one diagnostic.  Routes to the installed sink, or to printf + fflush. */
void gui_log( gui_log_level_t level, const char* fmt, ... );

/* Install the sink; fn == NULL restores the default printf sink.  Public via gui()->log_set_fn. */
void gui_log_set_fn( gui_log_fn fn, void* user );

/*==============================================================================================
    Report-once (every unit's pools and the frame lifecycle stand on this)

    Every fixed pool in the gui follows the same saturation rule: never fail hard, never be
    silent.  The overflowing site degrades gracefully (drop / share / evict) but reports ONCE
    per run so the symptom traces to its cap instead of reading as a rendering or input bug.
    This macro is the report half.  It routes through gui_log rather than printf so a host sink
    captures overflows too -- the default sink still flushes, so the message lands before a
    follow-up ORB_ASSERT_MSG_ONCE can trap.
==============================================================================================*/

#define GUI_LOG_ONCE( level, ... )                        \
    do                                                    \
    {                                                     \
        static bool s_gui_logged_once;                    \
        if ( !s_gui_logged_once )                         \
        {                                                 \
            s_gui_logged_once = true;                     \
            gui_log( ( level ), __VA_ARGS__ );            \
        }                                                 \
    } while ( 0 )

#define GUI_WARN_ONCE( ... ) GUI_LOG_ONCE( GUI_LOG_WARN, __VA_ARGS__ )

// clang-format on
/*============================================================================================*/
#endif    // GUI_LOG_H
