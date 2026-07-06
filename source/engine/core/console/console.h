#ifndef CONSOLE_HEADER_H
#define CONSOLE_HEADER_H
// clang-format off
/*==============================================================================================

    engine/core/console/console.h

    Developer console -- the engine-owned command/text hub the cvar system talks through.

    ALL console state lives here, inside core: the scrollback text, the command registry,
    and the input history.  A GUI (or any other front end) is a pure VIEW over this data --
    it renders con_line_get() and feeds keystrokes into con_exec().  The console is fully
    functional with no front end at all: every con_print also echoes to stdout.

    - Scrollback: fixed ring of lines.  con_print/con_printf assemble partial writes into
      lines (printf-style multi-call lines stay one line) and commit on '\n'.
    - Commands: fixed registry of name -> handler.  Names/descriptions are COPIED in, so a
      hot-reloaded DLL registering commands cannot leave dangling pointers.
    - Execution: con_exec tokenizes one line (quoted strings supported) and dispatches:
      registered command first, then cvar get ("name") / set ("name value"), else unknown.
    - History: executed lines kept in a ring for front-end up/down recall.
    - Completion: prefix match over command names + visible cvar names.

==============================================================================================*/

/*==============================================================================================
    Limits
==============================================================================================*/

#define CON_LINE_CAP        256     // scrollback lines retained (ring)
#define CON_LINE_LEN        192     // bytes per scrollback line (incl. NUL)
#define CON_HISTORY_CAP     32      // input history entries retained (ring)
#define CON_HISTORY_LEN     128     // bytes per history entry (incl. NUL)
#define CON_CMD_CAP         64      // max registered commands
#define CON_CMD_NAME_LEN    32      // bytes per command name (incl. NUL)
#define CON_CMD_DESC_LEN    96      // bytes per command description (incl. NUL)
#define CON_ARG_MAX         16      // max tokens per executed line

/*==============================================================================================
    Command handler
==============================================================================================*/

typedef void ( *con_cmd_fn )( int argc, char** argv );

/*==============================================================================================
    Lifetime
==============================================================================================*/

                                    /* Register built-in commands (help, echo, clear, history) */
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
    Commands
==============================================================================================*/

                                    /* Register a command; name/desc are copied. false = full/dup */
bool        con_cmd_register        ( const char* name, con_cmd_fn fn, const char* desc );

                                    /* Remove a command by name */
void        con_cmd_unregister      ( const char* name );

                                    /* Registered command count / name / description by index */
u32         con_cmd_count           ( void );
const char* con_cmd_name            ( u32 index );
const char* con_cmd_desc            ( u32 index );

/*==============================================================================================
    Execution
==============================================================================================*/

                                    /* Echo, record to history, tokenize, dispatch one line.
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
