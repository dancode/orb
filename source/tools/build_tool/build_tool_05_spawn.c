/*==============================================================================================

    build_tool_05_spawn.c -- Child-process spawning and output capture.

    All child-process invocations route through one of two helpers:

        build_run_cmd()                     -- run + wait, output to log or stdout.
        build_run_cmd_capture_includes()    -- run + wait, parse /showIncludes lines.

    Both delegate to the platform layer (build_tool_win_spawn.c / build_tool_posix_spawn.c)
    rather than calling system() / _popen / _pclose directly. The platform layer uses
    OS-native spawning APIs (CreateProcess on Win32, posix_spawn on POSIX) that set child
    stdio without mutating the parent's file descriptors. This makes N concurrent worker
    threads safe -- CRT wrappers race on descriptor duplication and produce silent failures.

==============================================================================================*/
// clang-format off

/*  Forward declaration: defined in 09_sched.c.
    Returns the active worker's per-thread log path, or NULL on the serial path. */
const char* sched_log_path( void );

/*==============================================================================================
    --- One-Shot Command Execution ---

    Delegates to platform_spawn(). When a worker log is active the child's output is
    appended there; otherwise it inherits the parent's console handles.

    build_run_cmd() also appends an exit-code marker on failure so post-mortem log
    inspection always has a clear terminal line.
==============================================================================================*/

int
build_run_cmd( const char* cmd )
{
    const char* log = sched_log_path();
    int rc = platform_spawn( cmd, log );

    if ( log && rc != 0 )
    {
        FILE* lf = fopen( log, "a" );
        if ( lf ) { fprintf( lf, ORB_INDENT "[orb exit = %d]\n", rc ); fclose( lf ); }
    }
    return rc;
}

/* Like build_run_cmd() but routes through cmd.exe /c so shell built-ins (del, rd)
   and output redirections (>nul 2>nul) work. Errors are suppressed -- used by
   build_clean() where "file not found" is expected and the caller prints a summary. */

int
build_run_cmd_quiet( const char* cmd )
{
    char shell_cmd[ PATH_MAX * 2 ];
    snprintf( shell_cmd, sizeof( shell_cmd ), "cmd.exe /c %s", cmd );
    return platform_spawn( shell_cmd, sched_log_path() );
}

/*==============================================================================================
    --- Include Line Classification ---

    Classify one line of cl.exe stdout/stderr:

      "Note: including file: <path>" -- extract the header path, filter out VC
        toolchain and Windows SDK paths (project source can't invalidate those),
        and write what remains to the per-target _includes.txt file.

      Everything else -- forward to the build sink (worker log or stdout) so
        compile warnings, errors, and source banners are still visible.

    NOTE: The "Note: including file:" prefix is English-only. For other locales,
    set VSLANG=1033 in the environment before launching the build.
==============================================================================================*/

static void
process_includes_line( char* line, FILE* includes, FILE* out )
{
    static const char   k_prefix[] = "Note: including file:";
    static const size_t k_prefix_n = sizeof( k_prefix ) - 1;

    // Skip leading whitespace -- cl.exe indents indirect includes.
    char* p = line;
    while ( *p == ' ' || *p == '\t' ) ++p;

    if ( strncmp( p, k_prefix, k_prefix_n ) == 0 )
    {
        // --- Header path branch ---
        char* path = p + k_prefix_n;
        while ( *path == ' ' || *path == '\t' ) ++path;

        size_t l = strlen( path );
        while ( l > 0 && ( path[ l - 1 ] == '\r' || path[ l - 1 ] == '\n' ) ) path[ --l ] = '\0';
        if ( l == 0 ) return;

        // Filter out VC toolchain and Windows SDK headers -- those are never
        // invalidated by project source edits, so tracking them wastes I/O.
        bool is_system =    strstr( path, "\\VC\\Tools\\" )    != NULL
                         || strstr( path, "\\Windows Kits\\" ) != NULL
                         || strstr( path, "/VC/Tools/" )       != NULL
                         || strstr( path, "/Windows Kits/" )   != NULL;
        if ( includes && !is_system ) fprintf( includes, "%s\n", path );
    }
    else
    {
        // --- Diagnostic / banner branch ---
        // Diagnostics always reach the user even in quiet mode; losing an error
        // is far worse than printing an extra line.
        bool is_diagnostic = ( strstr( line, ": error"       ) != NULL )
                          || ( strstr( line, ": warning"     ) != NULL )
                          || ( strstr( line, ": note"        ) != NULL )
                          || ( strstr( line, ": fatal error" ) != NULL );

        if ( is_diagnostic || ( g_out_flags & ORB_OUT_MSVC_OUTPUT ) )
        {
            // First diagnostic prints a blank line first so it stands out
            // from any preceding banner noise. static is safe here because
            // parallel workers serialize at the print lock anyway.
            static int issue_count = 0;
            if ( is_diagnostic && issue_count == 0 ) {
                fprintf( out, "\n" );
                issue_count++;
            }

            // Drop bare source-file echoes ("foo.c"); the orb log already
            // shows the source list, so cl's echo adds duplicate noise.
            if ( !is_msvc_source_echo( line ) )
                fprintf( out, ORB_INDENT "%s\n", line );
        }
    }
}

/*==============================================================================================
    --- Capture Includes ---

    Run a compile command and route each output line through process_includes_line().
    Used exclusively by the compile step. Also serves as the general-purpose line-capture
    path for link/lib where includes_path is NULL (no /showIncludes parsing needed, but
    we still want per-line routing so [MSVC] prefixing and ORB_OUT_MSVC_OUTPUT gating apply).

    Delegates to platform_spawn_capture() which owns the pipe and line assembly.
==============================================================================================*/

typedef struct
{
    FILE* includes;
    FILE* out;

} includes_ctx_t;

static void
on_includes_line( char* line, void* userdata )
{
    includes_ctx_t* ctx = ( includes_ctx_t* )userdata;
    process_includes_line( line, ctx->includes, ctx->out );
}

int
build_run_cmd_capture_includes( const char* cmd, const char* includes_path )
{
    // Output sink: per-worker log if active, otherwise stdout.
    FILE* owned_log = NULL;
    FILE* out       = stdout;

    const char* log_path = sched_log_path();
    if ( log_path )
    {
        owned_log = fopen( log_path, "a" );
        if ( owned_log ) out = owned_log;
    }

    FILE* includes = includes_path ? fopen( includes_path, "w" ) : NULL;

    includes_ctx_t ctx = { includes, out };
    int rc = platform_spawn_capture( cmd, on_includes_line, &ctx );

    if ( includes  ) fclose( includes );
    if ( owned_log ) fclose( owned_log );

    return rc;
}

// clang-format on
/*============================================================================================*/
