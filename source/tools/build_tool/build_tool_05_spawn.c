/*==============================================================================================

    build_tool_05_spawn.c -- Child-process spawning and output capture.

    All child-process invocations route through one of two helpers:

        build_run_cmd()                     -- run + wait, output to log or stdout.
        build_run_cmd_capture_includes()    -- run + wait, parse /showIncludes lines.

    Both use CreateProcess directly rather than system() / _popen / _pclose. The
    CRT wrappers internally dup the parent's stdio descriptors as a side effect of
    setting up the child's handles. When the parallel scheduler calls them from N
    worker threads at once, the dups race and children inherit clobbered handles --
    observable as silent exit-code-1 failures with empty output. CreateProcess sets
    child stdio via STARTUPINFO with NO mutation of the parent's fds; N concurrent
    calls are safe.

==============================================================================================*/
// clang-format off

/*  Forward declaration: defined in 09_sched.c.
    Returns the active worker's per-thread log path, or NULL on the serial path. */
const char* sched_log_path( void );

/*==============================================================================================
    --- Child Process Spawning ---

    Spawn a child via CreateProcessA, wait for exit, return the exit code.

    If log_path is non-NULL the child's stdout and stderr are appended to that
    file (FILE_SHARE_WRITE so concurrent workers never block each other).
    NULL falls through to the parent's console handles.

    cmd is wrapped in "cmd.exe /C" so shell builtins (del, for) and
    tool-side glob expansion (*.obj) keep working unchanged.
==============================================================================================*/

static int
spawn_cmd( const char* cmd, const char* log_path )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE hout = NULL;

    if ( log_path )
    {
        hout = CreateFileA( log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );

        if ( hout == INVALID_HANDLE_VALUE ) {
            printf( ORB_INDENT "[orb error] could not open log file %s (err %lu)\n",
                    log_path, GetLastError() );
            return -1;
        }
    }

    char command_string[ CMD_BUF_MAX ];
    snprintf( command_string, sizeof( command_string ), "cmd.exe /C %s", cmd );

    STARTUPINFOA si = { 0 };
    si.cb         = sizeof( si );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = hout ? hout : GetStdHandle( STD_OUTPUT_HANDLE );
    si.hStdError  = hout ? hout : GetStdHandle( STD_ERROR_HANDLE );

    PROCESS_INFORMATION pi = { 0 };
    if ( !CreateProcessA( NULL, command_string, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ) )
    {
        DWORD err = GetLastError();
        if ( hout ) CloseHandle( hout );
        printf( ORB_INDENT "[orb error] CreateProcess failed (err %lu): %s\n", err, cmd );
        return -1;
    }

    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD error_code = 1;
    GetExitCodeProcess( pi.hProcess, &error_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    if ( hout ) CloseHandle( hout );

    return ( int )error_code;
}

/*==============================================================================================
    --- One-Shot Command Execution ---

    Routes to spawn_cmd() with the active worker's log path so child output is
    captured into the per-target log during parallel builds. On failure the exit
    code is also written to the log so post-mortem inspection has a clear marker.
==============================================================================================*/

int
build_run_cmd( const char* cmd )
{
    const char* log = sched_log_path();
    if ( log )
    {
        int rc = spawn_cmd( cmd, log );

        if ( rc != 0 )
        {
            FILE* lf = fopen( log, "a" );
            if ( lf ) { fprintf( lf, ORB_INDENT "[orb exit = %d]\n", rc ); fclose( lf ); }
        }
        return rc;
    }
    return spawn_cmd( cmd, NULL );
}

/* Like build_run_cmd() but suppresses the exit-code marker on failure.
   Used by build_clean() for del/rd where errors are expected (file not found)
   and the caller prints a single summarized line instead of one per call. */

int
build_run_cmd_quiet( const char* cmd )
{
    const char* log = sched_log_path();
    return spawn_cmd( cmd, log );
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

    Run a compile command, intercept every output line, and route each line to
    either the header-path list (includes_path) or the build log. Used exclusively
    by the compile step. Also serves as the general-purpose capture path for
    link/lib where includes_path is NULL (no /showIncludes parsing needed, but
    we still want line-by-line capture so [MSVC] prefixing and ORB_OUT_MSVC_OUTPUT
    gating apply).

    Uses CreatePipe + CreateProcess instead of _popen/_pclose for the same
    thread-safety reason as spawn_cmd().
==============================================================================================*/

int
build_run_cmd_capture_includes( const char* cmd, const char* includes_path )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE              rd = NULL, wr = NULL;
    if ( !CreatePipe( &rd, &wr, &sa, 65536 ) ) return -1;
    SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 );

    char compile_command[ CMD_BUF_MAX ];
    snprintf( compile_command, sizeof( compile_command ), "cmd.exe /C %s", cmd );

    STARTUPINFOA si = { 0 };
    si.cb         = sizeof( si );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = wr;
    si.hStdError  = wr;

    // Submit the CL.exe compile command

    PROCESS_INFORMATION pi = { 0 };
    if ( !CreateProcessA( NULL, compile_command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ) )
    {
        CloseHandle( rd );
        CloseHandle( wr );
        return -1;
    }

    // Close our copy of the write end so ReadFile reaches EOF when the child exits.
    CloseHandle( wr );

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

    // Stream the pipe into a line buffer; classify each full line.
    char   line     [ 4096 ];
    char   buf      [ 4096 ];
    size_t line_len  = 0;
    DWORD  bytes_read = 0;

    while ( ReadFile( rd, buf, sizeof( buf ), &bytes_read, NULL ) && bytes_read > 0 )
    {
        for ( DWORD i = 0; i < bytes_read; ++i )
        {
            char c = buf[ i ];
            if ( c == '\r' ) continue;

            if ( c == '\n' || line_len >= sizeof( line ) - 1 )
            {
                line[ line_len ] = '\0';
                process_includes_line( line, includes, out );
                line_len = 0;

                // If flushed due to buffer overflow (not a true newline), the
                // current character is real content -- don't lose it.
                if ( c != '\n' ) line[ line_len++ ] = c;
            }
            else
            {
                line[ line_len++ ] = c;
            }
        }
    }

    // Flush any trailing partial line (cl may exit without a final newline).
    if ( line_len > 0 )
    {
        line[ line_len ] = '\0';
        process_includes_line( line, includes, out );
    }

    CloseHandle( rd );
    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD error_code = 1;
    GetExitCodeProcess( pi.hProcess, &error_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

    if ( includes  ) fclose( includes );
    if ( owned_log ) fclose( owned_log );

    return ( int )error_code;
}

// clang-format on
/*============================================================================================*/
