/*==============================================================================================

    build_tool_cmd.c -- Child-process spawning and output capture.

    All child-process invocations route through one of two helpers:

        spawn_cmd()                  -- run + wait, output to log_path or stdout.
        build_run_cmd_capture_deps() -- run + wait, parse /showIncludes lines.
    
    Both deliberately avoid system() / _popen / _pclose, which the legacy
    implementation used. Those CRT wrappers internally manipulate (dup) the
    parent's stdio descriptors as a side effect of setting up the child's
    stdin/stdout/stderr. When the parallel scheduler calls them from N
    worker threads at once, the dups race and children inherit clobbered
    handles -- observable as silent exit-code-1 failures with empty output.
    CreateProcess sets the child's stdio via STARTUPINFO, with NO mutation
    of the parent's fds, so concurrent calls from N threads are safe.

==============================================================================================*/
// clang-format off

const char* sched_log_path( void );                     // forward declaration 
static bool is_msvc_source_echo( const char* line );    // forward declaration

/*==============================================================================================

    spawn_cmd() -- Spawn a child via CreateProcessA, wait for it, return exit code.

    If log_path is non-NULL, the child's stdout and stderr are appended to that file
    (opened with FILE_SHARE_WRITE so concurrent workers hitting different logs never
    block each other). NULL falls through to the parent's console handles.

    Workers pass their per-target log path so parallel output never interleaves on
    screen -- the scheduler dumps each log atomically when the target finishes.

    cmd is wrapped in "cmd.exe /C" so shell builtins (del, for) and tool-side glob
    expansion (*.obj) keep working unchanged.

==============================================================================================*/

static int
spawn_cmd( const char* cmd, const char* log_path )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE hout = NULL;

    /* open the log (file) for append so the child's HANDLE is inheritable. */

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

    /* Wrap in cmd.exe /C. Buffer is CMD_BUF_MAX + 64 to accommodate the
      "cmd.exe /C " prefix on top of any maximum-length compile/link line.*/

    char wrapped[ CMD_BUF_MAX + 64 ];
    snprintf( wrapped, sizeof( wrapped ), "cmd.exe /C %s", cmd );

    /* Fully populate STARTUPINFO so the child gets explicit, inheritable
        handles for stdio. Without STARTF_USESTDHANDLES, CreateProcess would
        give the child whatever default fds it computes -- usually fine, but
        we need to guarantee the log-file handle is what the child sees. */

    STARTUPINFOA si = { 0 };
    si.cb         = sizeof( si );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = hout ? hout : GetStdHandle( STD_OUTPUT_HANDLE );
    si.hStdError  = hout ? hout : GetStdHandle( STD_ERROR_HANDLE );

    PROCESS_INFORMATION pi = { 0 };
    if ( !CreateProcessA( NULL, wrapped, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ) )
    {
        DWORD err = GetLastError();
        if ( hout ) CloseHandle( hout );
        printf( ORB_INDENT "[orb error] CreateProcess failed (err %lu): %s\n", err, cmd );
        return -1;
    }

    /* Block until the child exits. INFINITE is fine -- none of our tools
       hang indefinitely, and if they do the build is broken anyway.*/

    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD error_code = 1;
    GetExitCodeProcess( pi.hProcess, &error_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    if ( hout ) CloseHandle( hout );

    return ( int )error_code;
}

/*==============================================================================================  

    Public entry point for one-shot command execution. Routes to spawn_cmd() with 
    the active worker's log path (if any) so child output is captured
    into the per-target log during parallel builds. On failure the exit code
    is also written to the log so post-mortem inspection has a clear marker.
    
    Used if a failure should stop the build immediately (e.g. compile/link step)

    This is a blocking command -- the caller waits for it to finish before proceeding.

 ==============================================================================================*/

int build_run_cmd( const char* cmd )
{
    const char* log = sched_log_path();
    if ( log )
    {
        /* this is blocking -- */
        int rc = spawn_cmd( cmd, log );

        /* Surface non-zero exit in the log so post-mortem inspection sees
           a clear failure marker even when the child wrote nothing to stderr. */

        if ( rc != 0 )
        {
            FILE* lf = fopen( log, "a" );
            if ( lf ) { fprintf( lf, ORB_INDENT "[orb exit = %d]\n", rc ); fclose( lf ); }
        }
        return rc;
    }
    return spawn_cmd( cmd, NULL );
}

/*==============================================================================================

    build_run_cmd_quiet() -- Like build_run_cmd() but suppresses the exit-code marker
    on failure. Used by build_clean() for del/rd operations where failures are expected
    and the caller prints a single summarized header rather than one line per call.

==============================================================================================*/

int build_run_cmd_quiet( const char* cmd )
{
    const char* log = sched_log_path();
    return spawn_cmd( cmd, log );
}

/*==============================================================================================  

    -- Process Dependency File Lines --
    
    Classify one line of cl.exe stdout/stderr output:
    
     - "Note: including file: <path>" -- header path of a #include cl just
       resolved. Strip whitespace + CR/LF, filter out paths from the VC
       toolchain / Windows SDK (project source can't invalidate those), and
       record what remains into the per-target _deps.txt file.
    
     - Anything else -- forward to the build sink (worker log file or stdout)
       so compile warnings, errors, and the source-file banner cl emits are
       still visible to the user.
    
    NOTE: The "Note: including file:" prefix is English-only. For other
    locales, set VSLANG=1033 in the environment before launching the build.

 ==============================================================================================*/

static void
process_deps_line( char* line, FILE* deps, FILE* out )
{
    // cl prints header lookups as:  "Note: including file: <path>"
    // We anchor on that exact prefix to split paths from diagnostic output.
    static const char   k_prefix[] = "Note: including file:";
    static const size_t k_prefix_n = sizeof( k_prefix ) - 1;

    // Skip any leading whitespace so prefix matching works even when cl
    // emits the line indented (which it does for indirect includes -- each
    // nesting level adds a leading space).
    char* p = line;
    while ( *p == ' ' || *p == '\t' ) ++p;

    if ( strncmp( p, k_prefix, k_prefix_n ) == 0 )
    {
        // ---- Header-path branch -------------------------------------------
        // Step over the prefix and any whitespace before the actual path.
        char* path = p + k_prefix_n;
        while ( *path == ' ' || *path == '\t' ) ++path;

        // Trim trailing CR/LF; the pipe-feed loop strips \r on the fly but
        // we still see \n here, plus any older \r-terminated input.
        size_t l = strlen( path );
        while ( l > 0 && ( path[ l - 1 ] == '\r' || path[ l - 1 ] == '\n' ) ) path[ --l ] = '\0';
        if ( l == 0 ) return;

        // Filter out VC toolchain and Windows SDK headers: those can never
        // be invalidated by a project source edit, so tracking them as deps
        // is just wasted I/O on every incremental build. We check both
        // slash conventions because cl emits whatever the include used.
        bool is_system = strstr( path, "\\VC\\Tools\\" ) != NULL
                         || strstr( path, "\\Windows Kits\\" ) != NULL
                         || strstr( path, "/VC/Tools/" ) != NULL
                         || strstr( path, "/Windows Kits/" ) != NULL;
        if ( deps && !is_system ) fprintf( deps, "%s\n", path );
    }
    else
    {
        // ---- Diagnostic / banner branch ----------------------------------
        // Anything that isn't a header path is either a real diagnostic
        // (error / warning / note / fatal error) or noise (cl's per-TU
        // source banner). We want diagnostics to always reach the user,
        // even in quiet mode -- losing an error is much worse than printing
        // an extra line. Other lines only print when the verbose flag is set.
        bool is_diagnostic = ( strstr( line, ": error"   ) != NULL )
                          || ( strstr( line, ": warning" ) != NULL )
                          || ( strstr( line, ": note"    ) != NULL )
                          || ( strstr( line, ": fatal error" ) != NULL );

        if ( is_diagnostic || ( g_out_flags & ORB_OUT_MSVC_OUTPUT ) )
        {
            // The first diagnostic prints a blank line first so it visually
            // separates from any preceding banner-noise -- easier to spot.
            // `static` is fine here because parallel workers serialize at
            // the print lock anyway.
            static int issue_count = 0;
            if ( is_diagnostic && issue_count == 0 ) {
                fprintf( out, "\n" );
                issue_count++;
            }
            // Drop bare source-file echoes ("foo.c"); the orb log already
            // shows the source list, so cl's echo just adds duplicate noise.
            if ( !is_msvc_source_echo( line ) )
                fprintf( out, ORB_INDENT "%s\n", line );
        }
    }
}
/*==============================================================================================

    --- Capture Dependencies ---

    Run cl.exe (the compiler), intercept every line it prints, and sort those lines
    into two buckets — "header file paths" go to a deps file, everything else such as
    (errors, warnings) goes to the log.

    Same role as build_run_cmd(), but additionally pipes the child's
    stdout + stderr back through us so /showIncludes lines can be parsed out
    into deps_path. Used exclusively by the compile step.

    Returns 0 on success, non-zero on failure.

 ==============================================================================================*/

int
build_run_cmd_capture_deps( const char* cmd, const char* deps_path )
{
    /*  CreatePipe + CreateProcess instead of _popen/_pclose, for the same
        thread-safety reason explained in spawn_cmd() above.
        
        Pipe layout: write end (wr) goes to the child; read end (rd) stays
        with us and we stream it line-by-line below. SetHandleInformation
        turns OFF inheritance on the read end so the child can never see it
        (which would prevent the pipe from EOF'ing when the child exits). */

    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE              rd = NULL, wr = NULL;
    if ( !CreatePipe( &rd, &wr, &sa, 65536 ) ) return -1;
    SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 );

    /* pad buffer for prefix command */
    char wrapped[ CMD_BUF_MAX + 64 ];
    snprintf( wrapped, sizeof( wrapped ), "cmd.exe /C %s", cmd );

    STARTUPINFOA si = { 0 };
    si.cb         = sizeof( si );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = wr;
    si.hStdError  = wr;

    PROCESS_INFORMATION pi = { 0 };
    if ( !CreateProcessA( NULL, wrapped, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ) )
    {
        CloseHandle( rd );
        CloseHandle( wr );
        return -1;
    }
    // Close OUR copy of the write end. Once the child also closes it
    // (process exit), ReadFile on rd will return EOF. Without this, the
    // ReadFile loop below would block forever.
    CloseHandle( wr );

    // Output sink: per-worker log if active, otherwise stdout.
    const char* log_path  = sched_log_path();
    FILE*       owned_log = NULL;
    FILE*       out       = stdout;
    if ( log_path )
    {
        owned_log = fopen( log_path, "a" );
        if ( owned_log ) out = owned_log;
    }

    FILE* deps = deps_path ? fopen( deps_path, "w" ) : NULL;

    // Stream the pipe into a line buffer; classify each full line via
    // process_deps_line(). The child writes whatever-sized chunks it likes;
    // we accumulate bytes into `line` until we see a newline OR run out of
    // room, then ship the assembled line off for parsing.
    //
    //  `line` ............ the per-line accumulator we hand to process_deps_line.
    //  `buf`  ............ the per-read scratch we pull from the pipe.
    //  `\r` is dropped on the fly so the assembled line contains only the
    //  Unix-style content even when the child emits CRLF.
    //
    // Any line longer than sizeof(line)-1 is force-flushed mid-way -- fine
    // because the only "interesting" lines (the "Note: including file:"
    // markers) are well under 4KB in practice.
    char  line[ 4096 ];
    size_t line_len = 0;
    char  buf[ 4096 ];
    DWORD br = 0;
    while ( ReadFile( rd, buf, sizeof( buf ), &br, NULL ) && br > 0 )
    {
        for ( DWORD i = 0; i < br; ++i )
        {
            char c = buf[ i ];
            if ( c == '\r' ) continue;          // Drop CR; we only care about LF.

            if ( c == '\n' || line_len >= sizeof( line ) - 1 )
            {
                // Either a real line end, or the buffer is full and we have
                // to flush. Either way, null-terminate and dispatch.
                line[ line_len ] = '\0';
                process_deps_line( line, deps, out );
                line_len = 0;

                // If we flushed due to overflow (not a true newline), the
                // current character is real content -- append it to the
                // freshly emptied buffer so we don't lose it.
                if ( c != '\n' ) line[ line_len++ ] = c;
            }
            else
            {
                line[ line_len++ ] = c;
            }
        }
    }
    // Flush any trailing partial line. The child may exit without writing a
    // terminating newline (cl does this on the last line of its banner
    // output), and we'd lose that final line without this catch.
    if ( line_len > 0 )
    {
        line[ line_len ] = '\0';
        process_deps_line( line, deps, out );
    }

    CloseHandle( rd );
    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD error_code = 1;
    GetExitCodeProcess( pi.hProcess, &error_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

    if ( deps ) fclose( deps );
    if ( owned_log ) fclose( owned_log );

    return ( int )error_code; /* 9 == success */
}

// clang-format on
/*============================================================================================*/
