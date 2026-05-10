/*==============================================================================================

    win_process.c - Process spawning + output capture for Windows.

==============================================================================================*/

/*==============================================================================================

    Spawns a child process and waits for it to exit, returning its exit code and elapsed time.
    This is a simple wrapper around CreateProcessA + WaitForSingleObject that also measures
    elapsed time using QueryPerformanceCounter.

    It does not capture stdout/stderr; use sys_process_run_capture for that.

==============================================================================================*/

bool
sys_process_run( const char* command_line, const char* working_dir, sys_process_result_t* result )
{
    if ( result )
    {
        result->started         = false;
        result->exit_code       = -1;
        result->elapsed_seconds = 0.0;
    }
    if ( command_line == NULL || *command_line == '\0' )
        return false;

    /* CreateProcessA mutates the command-line buffer, so copy it first. */
    char cmd[ 4096 ];
    snprintf( cmd, sizeof( cmd ), "%s", command_line );

    STARTUPINFOA        si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb                  = sizeof( si );

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &t0 );

    BOOL ok = CreateProcessA( NULL,        /* lpApplicationName */
                              cmd,         /* lpCommandLine (mutable) */
                              NULL,        /* lpProcessAttributes */
                              NULL,        /* lpThreadAttributes */
                              FALSE,       /* bInheritHandles */
                              0,           /* dwCreationFlags */
                              NULL,        /* lpEnvironment */
                              working_dir, /* lpCurrentDirectory */
                              &si, &pi );

    if ( !ok )
        return false;

    /* Wait for the process to exit, then get its exit code and elapsed time. */
    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD exit_code = 0;
    GetExitCodeProcess( pi.hProcess, &exit_code );

    QueryPerformanceCounter( &t1 );

    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

    if ( result )
    {
        result->started         = true;
        result->exit_code       = ( int )exit_code;
        result->elapsed_seconds = ( f64 )( t1.QuadPart - t0.QuadPart ) / ( f64 )freq.QuadPart;
    }
    return true;
}

/*==============================================================================================

    Redirects stdout/stderr to a temp file and reads it back after the child exits.
    This is uglier than CreatePipe + ReadFile but it completely sidesteps pipe-buffer
    deadlocks for long build logs and stays single-threaded.

==============================================================================================*/

bool
sys_process_run_capture( const char*           command_line,
                         const char*           working_dir,
                         char*                 out_buffer,
                         int                   out_buffer_size,
                         int*                  out_written,
                         sys_process_result_t* result )
{
    if ( out_buffer && out_buffer_size > 0 )
        out_buffer[ 0 ] = '\0';

    if ( out_written )
        *out_written = 0;

    /* Pick a temp file path under %TEMP% — caller's CWD is irrelevant here. */
    char tmp_dir[ MAX_PATH ];
    char tmp_file[ MAX_PATH ];
    GetTempPathA( sizeof( tmp_dir ), tmp_dir );
    GetTempFileNameA( tmp_dir, "orb", 0, tmp_file );

    /* Wrap the original command so cmd.exe handles the redirection.
       Outer quotes are required because tmp_file may contain spaces. */
    char wrapped[ 4096 ];
    snprintf( wrapped, sizeof( wrapped ), "cmd /c \"%s > \"%s\" 2>&1\"", command_line, tmp_file );

    bool launched = sys_process_run( wrapped, working_dir, result );

    if ( !launched )
    {
        DeleteFileA( tmp_file );
        return false;
    }

    if ( out_buffer && out_buffer_size > 1 )
    {
        FILE* f = fopen( tmp_file, "rb" );
        if ( f )
        {
            int n           = ( int )fread( out_buffer, 1, ( size_t )( out_buffer_size - 1 ), f );
            out_buffer[ n ] = '\0';
            fclose( f );
            if ( out_written )
                *out_written = n;
        }
    }

    DeleteFileA( tmp_file );
    return true;
}

/*============================================================================================*/