#ifndef CMD_HEADER_H
#define CMD_HEADER_H
// clang-format off
/*==============================================================================================

    engine/core/cmd/cmd.h

    Command execution backend -- the engine's single dispatch point for textual commands.
    Follows the proven Quake3/Doom3 model: a registry of name -> handler, an immediate
    executor, and a deferred text buffer pumped once per frame by the host loop.

    LAYERING: this is the backend.  The developer console, config exec, command-line +args,
    keybinds, and any service push text here.  The console is a pure front end / view --
    it never tokenizes or dispatches; it submits lines and renders output (con_print).

    - Registry:  fixed table of name -> handler.  Names/descriptions are COPIED in, so a
      hot-reloaded DLL registering commands cannot leave dangling pointers.
    - Execute:   cmd_execute_string tokenizes one statement (quoted strings supported) and
      dispatches: registered command first, then cvar get ("name") / set ("name value"),
      then alias expansion, else unknown.
    - Buffer:    single text buffer.  cmd_queue appends, cmd_queue_front inserts (exec/alias
      semantics: file/alias contents run before the rest of the pending text).  cmd_pump
      extracts statements split on '\n' and ';' (quote-aware, '//' comments) and executes them,
      honoring the "wait" command and a per-frame budget.
    - Aliases:   name -> command string (cmd_alias.c).  Checked last in cmd_execute_string so
      an alias can never shadow a real command or cvar; "alias"/"unalias"/"unaliasall" manage
      the table, matching the bind/unbind/unbindall shape below.

==============================================================================================*/

/*==============================================================================================
    Limits
==============================================================================================*/

#define CMD_CAP             256     // max registered commands (input actions register +/- pairs)
#define CMD_NAME_LEN        32      // bytes per command name (incl. NUL)
#define CMD_DESC_LEN        96      // bytes per command description (incl. NUL)
#define CMD_ARG_MAX         16      // max tokens per executed statement
#define CMD_LINE_LEN        1024    // max bytes per executed statement (incl. NUL)
#define CMD_BUF_CAP         8192    // deferred command text buffer size
#define CMD_PUMP_BUDGET     1024    // max statements per pump (runaway guard)
#define CMD_ALIAS_CAP       64      // max user-defined aliases

/*==============================================================================================
    Command handler
==============================================================================================*/

typedef void ( *cmd_fn )( int argc, char** argv );

/*==============================================================================================
    Lifetime
==============================================================================================*/

                                    /* Clear registry + buffer, register built-ins (echo,
                                       help, cmdlist, wait) */
void        cmd_system_init         ( void );

                                    /* Clear all command system state */
void        cmd_system_exit         ( void );

/*==============================================================================================
    Registry
==============================================================================================*/

                                    /* Register a command; name/desc are copied. false = full/dup */
bool        cmd_register            ( const char* name, cmd_fn fn, const char* desc );

                                    /* True if a command with this name is registered */
bool        cmd_exists              ( const char* name );

                                    /* Remove a command by name */
void        cmd_unregister          ( const char* name );

                                    /* Registered command count / name / description by index */
u32         cmd_count               ( void );
const char* cmd_name                ( u32 index );
const char* cmd_desc                ( u32 index );

/*==============================================================================================
    Execution (immediate)
==============================================================================================*/

                                    /* Tokenize and dispatch ONE statement now.
                                       Returns true if a command or cvar handled it. */
bool        cmd_execute_string      ( const char* text );

/*==============================================================================================
    Buffer (deferred)
==============================================================================================*/

                                    /* Append text to the buffer (newline-terminated) */
void        cmd_queue               ( const char* text );

                                    /* Insert text before pending buffer contents (exec) */
void        cmd_queue_front         ( const char* text );

                                    /* Queue "+command arg..." groups from the command line */
void        cmd_queue_args          ( int argc, char** argv );

                                    /* Execute buffered statements; called once per frame.
                                       Honors "wait" and CMD_PUMP_BUDGET. */
void        cmd_pump                ( void );

/*==============================================================================================
    Shared helpers (bind + alias)
==============================================================================================*/

                                    /* Join argv[start..argc) into one space-separated line in
                                       `out` (truncates rather than overflows out_cap). Shared by
                                       "alias name a b c" and "bind key a b c" reconstruction. */
u32         cmd_join_args           ( char* out, u32 out_cap, char** argv, int start, int argc );

/*==============================================================================================
    Key binds (key -> command string, queued through the buffer)
==============================================================================================*/

                                    /* Wire the key-name table (index-matched to the platform
                                       key enum; must outlive the bind system).  Host calls
                                       once at boot with app_key_names()/APP_KEY_COUNT. */
void        cmd_bind_wire_names     ( const char* const* names, u32 count );

                                    /* Feed a key edge (host event drain; filter auto-repeat).
                                       '+cmd' binds queue +cmd/-cmd with the key appended;
                                       plain binds queue on key down only. */
void        cmd_bind_event          ( u32 key, bool down );

                                    /* Write unbindall + bind lines to an open FILE*
                                       (void* keeps stdio out of this header) */
void        cmd_bind_write_config   ( void* file );

                                    /* Write str to an open FILE* as a double-quoted token,
                                       backslash-escaping '"' and '\\' so cmd_tokenize can
                                       round-trip it exactly (void* keeps stdio out of this
                                       header). Does not write the surrounding quotes. */
void        cmd_write_quoted        ( void* file, const char* str );

/*==============================================================================================
    Aliases (name -> command string, expanded through the buffer)
==============================================================================================*/

                                    /* True if a command alias with this name exists */
bool        cmd_alias_exists        ( const char* name );

                                    /* Alias body text, or NULL if `name` is not an alias.
                                       Last resolution step in cmd_execute_string, after
                                       registered commands and cvars. */
const char* cmd_alias_value         ( const char* name );

                                    /* Write "alias <name> <value>" lines for every defined
                                       alias to an open FILE* (void* keeps stdio out of this
                                       header, same convention as cmd_bind_write_config) */
void        cmd_alias_write_config  ( void* file );

// clang-format on
/*============================================================================================*/
#endif    // CMD_HEADER_H
