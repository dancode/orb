#ifndef CONSOLE_HEADER_H
#define CONSOLE_HEADER_H
// clang-format off
/*==============================================================================================

    engine/core/console/console.h

    Developer console -- the engine-owned text VIEW over the command backend (cmd.h).

    The console owns presentation state only: the scrollback text and the input history.
    Command dispatch lives in the cmd backend -- con_exec echoes, records history, and
    forwards to cmd_execute_string.  A GUI (or any other front end) renders con_line_get()
    and feeds keystrokes into con_exec().  The console is fully functional with no front
    end at all: every con_print also echoes to stdout.

    - Scrollback: fixed ring of lines.  con_print/con_printf assemble partial writes into
      lines (printf-style multi-call lines stay one line) and commit on '\n'.
    - History: executed lines kept in a ring for front-end up/down recall.
    - Completion: prefix match over backend command names + visible cvar names.

==============================================================================================*/

/*==============================================================================================
    Limits
==============================================================================================*/

#define CON_LINE_CAP        256     // scrollback lines retained (ring)
#define CON_LINE_LEN        192     // bytes per scrollback line (incl. NUL)
#define CON_HISTORY_CAP     32      // input history entries retained (ring)
#define CON_HISTORY_LEN     128     // bytes per history entry (incl. NUL)

/*==============================================================================================
    Lifetime
==============================================================================================*/

                                    /* Register console-owned commands (clear, history) */
void        con_init                ( void );

                                    /* Clear all console state */
void        con_exit                ( void );

/*==============================================================================================
    Output
==============================================================================================*/

                                    /* Append text; lines commit on '\n', partials accumulate */
void        con_print               ( const char* text );

                                    /* printf-style con_print */
void        con_printf              ( const char* fmt, ... );

                                    /* Drop all scrollback lines */
void        con_clear               ( void );

/*==============================================================================================
    Scrollback access (front ends render from this)
==============================================================================================*/

                                    /* Number of retained lines (<= CON_LINE_CAP) */
u32         con_line_count          ( void );

                                    /* Retained line by index; 0 = oldest retained */
const char* con_line_get            ( u32 index );

/*==============================================================================================
    Input submission
==============================================================================================*/

                                    /* Echo, record to history, forward to cmd_execute_string.
                                       Returns true if a command or cvar handled it. */
bool        con_exec                ( const char* line );

/*==============================================================================================
    History
==============================================================================================*/

                                    /* Retained history count (<= CON_HISTORY_CAP) */
u32         con_history_count       ( void );

                                    /* History entry by index; 0 = oldest retained */
const char* con_history_get         ( u32 index );

/*==============================================================================================
    Completion
==============================================================================================*/

                                    /* Fill out_names with command + visible cvar names matching
                                       prefix (case-insensitive).  Returns match count (<= max).
                                       Pointers are stable until the next registration. */
u32         con_complete            ( const char* prefix, const char** out_names, u32 max );

// clang-format on
/*============================================================================================*/
#endif    // CONSOLE_HEADER_H
