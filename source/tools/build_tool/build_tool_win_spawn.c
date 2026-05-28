#if defined( _WIN32 )
/*==============================================================================================

    build_tool_win_spawn.c -- Windows process-spawning platform layer for the ORB build tool.

    Both functions use CreateProcess directly rather than system() / _popen / _pclose.
    The CRT wrappers internally dup the parent's stdio descriptors as a side effect of
    setting up the child's handles. When the parallel scheduler calls them from N worker
    threads at once, the dups race and children inherit clobbered handles -- observable as
    silent exit-code-1 failures with empty output. CreateProcess sets child stdio via
    STARTUPINFO with NO mutation of the parent's fds; N concurrent calls are safe.

    The cmd string is passed directly to CreateProcess as lpCommandLine with no cmd.exe
    wrapper. All callers pass fully-formed compiler/linker command lines with no shell
    metacharacters, so the shell layer is unnecessary and would only introduce quoting
    ambiguity. vcvars detection (build_tool_04_env.c) uses platform_popen which still
    goes through cmd.exe because it needs shell builtins and output redirection.

    A future build_tool_posix_spawn.c would provide identical symbols using:
        platform_spawn()         -- open(O_WRONLY|O_APPEND|O_CREAT) + posix_spawn / fork+exec
        platform_spawn_capture() -- pipe() + fork+exec + dup2 + read() loop

    Functions implemented:
        platform_spawn()         -- run cmd, redirect output to log file or inherit console
        platform_spawn_capture() -- run cmd, deliver each output line to a callback

==============================================================================================*/
// clang-format off

#if !defined( _WIN32 )
    #error "build_tool_win_spawn.c is only for Windows / MSVC builds"
#endif

/*==============================================================================================
    --- platform_spawn ---

    Runs cmd directly via CreateProcess and waits for it to finish.

    If log_path is non-NULL, both stdout and stderr are appended to that file
    (FILE_SHARE_WRITE so concurrent workers never block each other on the same log).
    If log_path is NULL, child output inherits the parent's console handles.

    Returns the child's exit code, or -1 if the process could not be created.
==============================================================================================*/

static int
platform_spawn( const char* cmd, const char* log_path )
{
    SECURITY_ATTRIBUTES sa   = { sizeof( sa ), NULL, TRUE };
    HANDLE              hout = NULL;

    if ( log_path )
    {
        hout = CreateFileA( log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if ( hout == INVALID_HANDLE_VALUE )
        {
            printf( ORB_INDENT "[orb error] could not open log file %s (err %lu)\n",
                    log_path, GetLastError() );
            return -1;
        }
    }

    /* CreateProcessA mutates lpCommandLine, so copy it. */
    char command_string[ CMD_BUF_MAX ];
    snprintf( command_string, sizeof( command_string ), "%s", cmd );

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
    DWORD exit_code = 1;
    GetExitCodeProcess( pi.hProcess, &exit_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    if ( hout ) CloseHandle( hout );

    return ( int )exit_code;
}

/*==============================================================================================
    --- platform_spawn_capture ---

    Runs cmd directly via CreateProcess, captures both stdout and stderr via an anonymous
    pipe, and calls fn( line, userdata ) once per complete output line.

    Lines are null-terminated with no trailing newline. CRLF is normalized to LF
    before line assembly so fn() always receives clean text. Lines longer than the
    internal 4096-byte buffer are flushed mid-line (the content is not lost; fn
    just receives it in two consecutive calls without a newline break).

    Returns the child's exit code, or -1 if the process could not be created.
==============================================================================================*/

static int
platform_spawn_capture( const char* cmd, platform_line_fn_t fn, void* userdata )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if ( !CreatePipe( &rd, &wr, &sa, 65536 ) ) return -1;

    // Mark the read end non-inheritable so the child does not hold it open.
    SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 );

    /* CreateProcessA mutates lpCommandLine, so copy it. */
    char command_string[ CMD_BUF_MAX ];
    snprintf( command_string, sizeof( command_string ), "%s", cmd );

    STARTUPINFOA si = { 0 };
    si.cb         = sizeof( si );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = wr;
    si.hStdError  = wr;

    PROCESS_INFORMATION pi = { 0 };
    if ( !CreateProcessA( NULL, command_string, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ) )
    {
        CloseHandle( rd );
        CloseHandle( wr );
        return -1;
    }

    // Close the parent's write end so ReadFile reaches EOF when the child exits.
    CloseHandle( wr );

    // Stream pipe bytes into a line buffer; fire fn() for each complete line.
    char   line[ 4096 ];
    char   buf [ 4096 ];
    size_t line_len  = 0;
    DWORD  bytes_read = 0;

    while ( ReadFile( rd, buf, sizeof( buf ), &bytes_read, NULL ) && bytes_read > 0 )
    {
        for ( DWORD i = 0; i < bytes_read; ++i )
        {
            char c = buf[ i ];
            if ( c == '\r' ) continue;      // normalize CRLF -> LF

            if ( c == '\n' || line_len >= sizeof( line ) - 1 )
            {
                line[ line_len ] = '\0';
                fn( line, userdata );
                line_len = 0;

                // If flushed due to buffer overflow (not a true newline), keep the char.
                if ( c != '\n' ) line[ line_len++ ] = c;
            }
            else
            {
                line[ line_len++ ] = c;
            }
        }
    }

    // Flush any trailing partial line (child may exit without a final newline).
    if ( line_len > 0 )
    {
        line[ line_len ] = '\0';
        fn( line, userdata );
    }

    CloseHandle( rd );
    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD exit_code = 1;
    GetExitCodeProcess( pi.hProcess, &exit_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

    return ( int )exit_code;
}

// clang-format on
/*============================================================================================*/
#endif
