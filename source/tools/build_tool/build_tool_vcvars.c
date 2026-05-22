/*==============================================================================================

    build_tool_vcvars.c -- Visual Studio environment discovery and command execution.

    The legacy approach was to prepend "call vcvarsall.bat x64 && " to every
    cl/link/lib invocation. vcvarsall takes 200-500 ms to run, and a real
    engine build issues dozens of compiler calls, so that overhead can easily
    cost 10+ seconds of wall time per build.

    Instead, we now run vcvarsall ONCE at orchestrator startup, capture the
    environment variables it sets via "&& set", and apply them to our own
    process with _putenv_s(). Every child process we spawn after that inherits
    the modified environment for free — no per-invocation prefix needed.

==============================================================================================*/
// clang-format off

// Forward decl from build_tool_sched.c (later in the unity build). Returns
// NULL outside a parallel worker — in that case we write to stdout directly.
const char* sched_log_path( void );

/*============================================================================================*/
// --- VC Environment Import ---

/**
 * locate_vcvarsall()
 *
 * Find a usable vcvarsall.bat on this machine and write its absolute path
 * to `out`. Two-stage lookup:
 *
 *   1. Ask vswhere.exe (the Microsoft-blessed VS discovery tool) for the
 *      latest installation, then derive the vcvarsall path from it. This
 *      tracks per-machine installs, side-by-side VS versions, and the
 *      Preview / Build Tools SKUs without us hard-coding anything.
 *   2. If vswhere is missing or returned nothing usable, fall back to
 *      probing a small list of well-known install locations.
 *
 * Returns true and fills `out` on success. False means no VS install was
 * discovered — caller should print a warning and let cl.exe lookups fail
 * naturally so the error is easy to diagnose.
 *
 * vswhere paths are double-quoted because the default install location
 * lives under "Program Files (x86)" with a space; cmd.exe needs the quotes
 * to treat the whole executable path as a single token. _popen wraps it in
 * cmd.exe automatically.
 */
static bool
locate_vcvarsall( char* out, size_t out_size )
{
    // Three vswhere candidates: literal path, plus two env-var forms in case
    // the user has installed VS to a non-default Program Files location.
    const char* vswhere_paths[] = {
        "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
    };

    for ( int i = 0; i < ( int )( sizeof( vswhere_paths ) / sizeof( vswhere_paths[ 0 ] ) ); ++i )
    {
        // Ask for the latest install, any product (Community / Pro / Build
        // Tools / Preview), and print just the install path on stdout.
        char cmd[ 1024 ];
        snprintf( cmd, sizeof( cmd ),
                  "%s -latest -products * -property installationPath",
                  vswhere_paths[ i ] );

        FILE* pipe = _popen( cmd, "rt" );
        if ( !pipe ) continue;

        char inst[ 512 ] = { 0 };
        if ( fgets( inst, sizeof( inst ), pipe ) )
        {
            // Strip the trailing newline that fgets keeps.
            char* nl = strpbrk( inst, "\r\n" );
            if ( nl ) *nl = '\0';
        }
        _pclose( pipe );

        if ( inst[ 0 ] )
        {
            // vcvarsall.bat is always at <install>\VC\Auxiliary\Build\.
            snprintf( out, out_size, "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", inst );
            if ( _access( out, 0 ) == 0 ) return true;
        }
    }

    // Fallback: probe a few common, hard-coded install paths. Catches the
    // case where vswhere is missing (rare but possible on Build-Tools-only
    // installs) or unreachable for whatever reason.
    const char* common[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
    };
    for ( int i = 0; i < ( int )( sizeof( common ) / sizeof( common[ 0 ] ) ); ++i )
    {
        if ( _access( common[ i ], 0 ) == 0 )
        {
            snprintf( out, out_size, "%s", common[ i ] );
            return true;
        }
    }

    return false;
}

/**
 * import_vcvars_env()
 *
 * Run "<vcvarsall> x64 && set" in a sub-shell and apply every printed
 * KEY=VALUE line to our own process environment via _putenv_s. Subsequent
 * CreateProcess children inherit the modified env, so cl/link/lib become
 * directly callable for the lifetime of THIS process.
 *
 * One-time cost (~2.5s) replaces the legacy "prepend `call vcvarsall.bat &&`
 * to every compiler call" pattern, which would otherwise add 200-500 ms × N
 * tool invocations to every build.
 *
 * The double-double-quote `cmd /c "" "<path>" args ""` idiom is required
 * when the vcvarsall path itself contains spaces (which it always does on
 * default installations under "Program Files"). The outer pair of empty
 * quotes tells cmd not to strip the second-pair quotes.
 */
static int
import_vcvars_env( const char* vcvars_path )
{
    char run_cmd[ 1024 ];
    snprintf( run_cmd, sizeof( run_cmd ),
              "cmd /c \"\"%s\" x64 >nul 2>nul && set\"", vcvars_path );

    FILE* pipe = _popen( run_cmd, "rt" );
    if ( !pipe )
    {
        printf( ORB_INDENT "[orb warn] could not spawn sub-shell for vcvars import\n" );
        return 0;
    }

    int imported = 0;
    // Each `set` line may be up to ~8KB (PATH/INCLUDE/LIB get long under
    // a full VS install). 16KB gives comfortable headroom.
    char line[ 16384 ];
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        // Strip trailing CR/LF so the value doesn't get newline-suffixed
        // into the env (would break tools that don't trim).
        size_t l = strlen( line );
        while ( l > 0 && ( line[ l - 1 ] == '\n' || line[ l - 1 ] == '\r' ) ) line[ --l ] = '\0';
        if ( l == 0 ) continue;

        // Split on the first '=' to produce key/value. An '=' at position 0
        // means the line has no key — `set` shouldn't emit these but skip
        // defensively.
        char* eq = strchr( line, '=' );
        if ( !eq || eq == line ) continue;
        *eq = '\0';

        const char* key   = line;
        const char* value = eq + 1;
        if ( _putenv_s( key, value ) == 0 ) ++imported;
    }
    _pclose( pipe );
    return imported;
}

/**
 * build_setup_vc_env()
 *
 * Idempotent one-time setup. If cl.exe is already discoverable in PATH
 * (Developer Command Prompt or pre-sourced vcvars), do nothing. Otherwise
 * locate vcvarsall.bat and import its environment into THIS process so
 * every subsequent child invocation runs with the VC toolchain visible.
 */
void
build_setup_vc_env( void )
{
#if defined( _WIN32 )
    // Fast path: cl.exe already on PATH. SearchPathA walks PATH without
    // spawning a child process, so this is free compared to system().
    char cl_path[ MAX_PATH ];
    if ( SearchPathA( NULL, "cl.exe", NULL, MAX_PATH, cl_path, NULL ) != 0 ) return;

    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] cl.exe not in PATH, locating Visual Studio...\n" );

    char vcvars_path[ 512 ] = { 0 };
    if ( !locate_vcvarsall( vcvars_path, sizeof( vcvars_path ) ) )
    {
        printf( ORB_INDENT "[orb warn] could not locate vcvarsall.bat, compiler calls will fail\n" );
        return;
    }

    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] importing from %s\n", vcvars_path );
    int n = import_vcvars_env( vcvars_path );
    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] imported %d environment variables\n", n );
#endif
}

/*============================================================================================*/
// --- Command Execution ---
//
// All child-process invocations route through one of two helpers below:
//   spawn_cmd()                  -- run + wait, output to log_path or stdout.
//   build_run_cmd_capture_deps() -- run + wait, parse /showIncludes lines.
//
// Both deliberately avoid system() / _popen / _pclose, which the legacy
// implementation used. Those CRT wrappers internally manipulate (dup) the
// parent's stdio descriptors as a side effect of setting up the child's
// stdin/stdout/stderr. When the parallel scheduler calls them from N
// worker threads at once, the dups race and children inherit clobbered
// handles — observable as silent exit-code-1 failures with empty output.
// CreateProcess sets the child's stdio via STARTUPINFO, with NO mutation
// of the parent's fds, so concurrent calls from N threads are safe.

/**
 * spawn_cmd()
 *
 * Spawn a child via CreateProcessA, wait for it, and return its exit code.
 *
 * If `log_path` is non-NULL the child's stdout AND stderr are appended to
 * that file. NULL routes them to whatever stdout/stderr the parent
 * currently owns (the shared console). Workers pass their per-target log
 * path so parallel output never interleaves on screen — the log is dumped
 * atomically by the scheduler when the target finishes.
 *
 * `cmd` is wrapped with "cmd.exe /C" so shell builtins (del, for) and
 * tool-side glob expansion (`*.obj`) keep working unchanged.
 */
static int
spawn_cmd( const char* cmd, const char* log_path )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE              hout = NULL;

    // If a log path was given, open it for append with SHARE_WRITE so two
    // workers writing to DIFFERENT log paths can never block each other,
    // and so that we can pass the HANDLE to the child for inheritance.
    if ( log_path )
    {
        hout = CreateFileA( log_path, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if ( hout == INVALID_HANDLE_VALUE )
        {
            printf( ORB_INDENT "[orb error] could not open log file %s (err %lu)\n",
                    log_path, GetLastError() );
            return -1;
        }
    }

    // Wrap in cmd.exe /C. Buffer is CMD_BUF_MAX + 64 to accommodate the
    // "cmd.exe /C " prefix on top of any maximum-length compile/link line.
    char wrapped[ CMD_BUF_MAX + 64 ];
    snprintf( wrapped, sizeof( wrapped ), "cmd.exe /C %s", cmd );

    // Fully populate STARTUPINFO so the child gets explicit, inheritable
    // handles for stdio. Without STARTF_USESTDHANDLES, CreateProcess would
    // give the child whatever default fds it computes — usually fine, but
    // we need to guarantee the log-file handle is what the child sees.
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

    // Block until the child exits. INFINITE is fine — none of our tools
    // hang indefinitely, and if they do the build is broken anyway.
    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD ec = 1;
    GetExitCodeProcess( pi.hProcess, &ec );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    if ( hout ) CloseHandle( hout );
    return ( int )ec;
}

/**
 * build_run_cmd()
 *
 * Public entry point for one-shot command execution. Routes to spawn_cmd()
 * with the active worker's log path (if any) so child output is captured
 * into the per-target log during parallel builds. On failure the exit code
 * is also written to the log so post-mortem inspection has a clear marker.
 */
int
build_run_cmd( const char* cmd )
{
    const char* log = sched_log_path();
    if ( log )
    {
        int rc = spawn_cmd( cmd, log );
        // Surface non-zero exit in the log so post-mortem inspection sees
        // a clear failure marker even when the child wrote nothing to stderr.
        if ( rc != 0 )
        {
            FILE* lf = fopen( log, "a" );
            if ( lf ) { fprintf( lf, ORB_INDENT "[orb exit=%d]\n", rc ); fclose( lf ); }
        }
        return rc;
    }
    return spawn_cmd( cmd, NULL );
}

/**
 * build_run_cmd_quiet()
 *
 * Identical to build_run_cmd but does not write an exit-code marker to the
 * log on failure. Used by build_clean() for trivial del/rd operations where
 * the caller prints a single summarized header instead of one line per call.
 */
int
build_run_cmd_quiet( const char* cmd )
{
    const char* log = sched_log_path();
    return spawn_cmd( cmd, log );
}

/**
 * process_deps_line()
 *
 * Classify one line of cl.exe stdout/stderr output:
 *
 *  - "Note: including file: <path>" → header path of a #include cl just
 *    resolved. Strip whitespace + CR/LF, filter out paths from the VC
 *    toolchain / Windows SDK (project source can't invalidate those), and
 *    record what remains into the per-target _deps.txt file.
 *
 *  - Anything else → forward to the build sink (worker log file or stdout)
 *    so compile warnings, errors, and the source-file banner cl emits are
 *    still visible to the user.
 *
 * NOTE: The "Note: including file:" prefix is English-only. For other
 * locales, set VSLANG=1033 in the environment before launching the build.
 */
static bool is_msvc_source_echo( const char* line );

static void
process_deps_line( char* line, FILE* deps, FILE* out )
{
    // cl prints header lookups as:  "Note: including file: <path>"
    // We anchor on that exact prefix to split paths from diagnostic output.
    static const char   k_prefix[] = "Note: including file:";
    static const size_t k_prefix_n = sizeof( k_prefix ) - 1;

    // Skip any leading whitespace so prefix matching works even when cl
    // emits the line indented (which it does for indirect includes — each
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
        // even in quiet mode — losing an error is much worse than printing
        // an extra line. Other lines only print when the verbose flag is set.
        bool is_diagnostic = ( strstr( line, ": error"   ) != NULL )
                          || ( strstr( line, ": warning" ) != NULL )
                          || ( strstr( line, ": note"    ) != NULL )
                          || ( strstr( line, ": fatal error" ) != NULL );

        if ( is_diagnostic || ( g_out_flags & ORB_OUT_MSVC_OUTPUT ) )
        {
            // The first diagnostic prints a blank line first so it visually
            // separates from any preceding banner-noise — easier to spot.
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

/**
 * build_run_cmd_capture_deps()
 *
 * Same role as build_run_cmd(), but additionally pipes the child's
 * stdout+stderr back through us so /showIncludes lines can be parsed out
 * into deps_path. Used exclusively by the compile step — see
 * build_target_compile() in build_tool_cc.c.
 */
int
build_run_cmd_capture_deps( const char* cmd, const char* deps_path )
{
    // CreatePipe + CreateProcess instead of _popen/_pclose, for the same
    // thread-safety reason explained in spawn_cmd() above.
    //
    // Pipe layout: write end (wr) goes to the child; read end (rd) stays
    // with us and we stream it line-by-line below. SetHandleInformation
    // turns OFF inheritance on the read end so the child can never see it
    // (which would prevent the pipe from EOF'ing when the child exits).
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE              rd = NULL, wr = NULL;
    if ( !CreatePipe( &rd, &wr, &sa, 65536 ) ) return -1;
    SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 );

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
    // Any line longer than sizeof(line)-1 is force-flushed mid-way — fine
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
                // current character is real content — append it to the
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
    DWORD ec = 1;
    GetExitCodeProcess( pi.hProcess, &ec );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

    if ( deps ) fclose( deps );
    if ( owned_log ) fclose( owned_log );
    return ( int )ec;
}

// clang-format off
/*============================================================================================*/
