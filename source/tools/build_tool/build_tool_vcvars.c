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
#include "build_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

// Forward decl from build_tool_sched.c (later in the unity build). Returns
// NULL outside a parallel worker — in that case we write to stdout directly.
const char* sched_log_path( void );

/*============================================================================================*/
// --- VC Environment Import ---

// Locate vcvarsall.bat. First try `vswhere` (the modern, future-proof path),
// then fall back to scanning well-known installation paths. Returns true and
// fills `out` with an absolute path on success.
static bool
locate_vcvarsall( char* out, size_t out_size )
{
    const char* vswhere_paths[] = {
        "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
    };

    for ( int i = 0; i < ( int )( sizeof( vswhere_paths ) / sizeof( vswhere_paths[ 0 ] ) ); ++i )
    {
        char cmd[ 1024 ];
        snprintf( cmd, sizeof( cmd ),
                  "%s -latest -products * -property installationPath",
                  vswhere_paths[ i ] );

        FILE* pipe = _popen( cmd, "rt" );
        if ( !pipe ) continue;

        char inst[ 512 ] = { 0 };
        if ( fgets( inst, sizeof( inst ), pipe ) )
        {
            char* nl = strpbrk( inst, "\r\n" );
            if ( nl ) *nl = '\0';
        }
        _pclose( pipe );

        if ( inst[ 0 ] )
        {
            snprintf( out, out_size, "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", inst );
            if ( _access( out, 0 ) == 0 ) return true;
        }
    }

    // Fallback: probe a few common, hard-coded install paths.
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

// Run "<vcvarsall> x64 && set" in a sub-shell and apply every printed
// KEY=VALUE to our own process environment via _putenv_s. Subsequent
// CreateProcess / system children inherit it.
//
// The double-double-quote cmd /c "" "<path>" args "" idiom is required when
// the vcvarsall path itself contains spaces (it always does on default
// installations under "Program Files").
static int
import_vcvars_env( const char* vcvars_path )
{
    char run_cmd[ 1024 ];
    snprintf( run_cmd, sizeof( run_cmd ),
              "cmd /c \"\"%s\" x64 >nul 2>nul && set\"", vcvars_path );

    FILE* pipe = _popen( run_cmd, "rt" );
    if ( !pipe )
    {
        printf( "Warning: could not spawn sub-shell for vcvars import.\n" );
        return 0;
    }

    int imported = 0;
    // Each `set` line may be up to ~8KB (PATH/INCLUDE/LIB get long).
    char line[ 16384 ];
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        size_t l = strlen( line );
        while ( l > 0 && ( line[ l - 1 ] == '\n' || line[ l - 1 ] == '\r' ) ) line[ --l ] = '\0';
        if ( l == 0 ) continue;

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
    // Fast path: cl.exe already on PATH. system() returns 0 because cl.exe
    // with no args prints its banner and exits 0.
    if ( system( "cl.exe >nul 2>nul" ) == 0 ) return;

    printf( "cl.exe not in PATH. Locating Visual Studio...\n" );

    char vcvars_path[ 512 ] = { 0 };
    if ( !locate_vcvarsall( vcvars_path, sizeof( vcvars_path ) ) )
    {
        printf( "Warning: Could not locate vcvarsall.bat. Compiler calls will fail.\n" );
        return;
    }

    printf( "Importing VC environment from %s\n", vcvars_path );
    int n = import_vcvars_env( vcvars_path );
    printf( "Imported %d environment variables (in-process).\n", n );
#endif
}

/*============================================================================================*/
// --- Command Execution ---

/**
 * build_run_cmd()
 *
 * Run a shell command and return its exit code. The orchestrator's own
 * environment (set up by build_setup_vc_env above) is inherited by the
 * spawned cmd.exe, so cl/link/lib are directly callable with no prefix.
 */
/**
 * spawn_cmd()
 *
 * Spawn a child via CreateProcessA. Output is appended to log_path if
 * non-NULL, else inherits the parent's stdout/stderr.
 *
 * IMPORTANT: We avoid system()/_popen here because both internally dup the
 * parent's stdio descriptors as a side effect of spawning. When several
 * worker threads call system() simultaneously, those dups race and the
 * children end up inheriting clobbered or closed handles — manifesting as
 * mysterious exit-code-1 failures with empty output (which is exactly what
 * we hit before this rewrite). CreateProcess sets up the child's stdio
 * via the STARTUPINFO passed to it, with no parent-side mutation, so
 * concurrent calls from N threads don't interact.
 *
 * The command is wrapped with "cmd.exe /C" so shell builtins (del, for)
 * and glob expansion in tool args (`*.obj`) keep working unchanged.
 */
static int
spawn_cmd( const char* cmd, const char* log_path )
{
    SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
    HANDLE              hout = NULL;

    if ( log_path )
    {
        hout = CreateFileA( log_path, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if ( hout == INVALID_HANDLE_VALUE )
        {
            printf( "Error: could not open log file %s (err %lu)\n",
                    log_path, GetLastError() );
            return -1;
        }
    }

    char wrapped[ CMD_BUF_MAX + 64 ];
    snprintf( wrapped, sizeof( wrapped ), "cmd.exe /C %s", cmd );

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
        printf( "Error: CreateProcess failed (err %lu): %s\n", err, cmd );
        return -1;
    }

    WaitForSingleObject( pi.hProcess, INFINITE );
    DWORD ec = 1;
    GetExitCodeProcess( pi.hProcess, &ec );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    if ( hout ) CloseHandle( hout );
    return ( int )ec;
}

int
build_run_cmd( const char* cmd )
{
    const char* log = sched_log_path();
    if ( log )
    {
        // Worker thread: redirect stdout+stderr into this target's log file
        // so parallel workers don't interleave on the shared console. The
        // log gets flushed in one atomic block when the target completes.
        FILE* lf = fopen( log, "a" );
        if ( lf )
        {
            fprintf( lf, "[CMD] %s\n", cmd );
            fclose( lf );
        }
        int rc = spawn_cmd( cmd, log );
        if ( rc != 0 )
        {
            FILE* lf2 = fopen( log, "a" );
            if ( lf2 ) { fprintf( lf2, "[CMD exit=%d]\n", rc ); fclose( lf2 ); }
        }
        return rc;
    }
    printf( "[CMD] %s\n", cmd );
    return spawn_cmd( cmd, NULL );
}

/**
 * build_run_cmd_capture_deps()
 *
 * Same as build_run_cmd, but pipes stdout+stderr through and extracts
 * /showIncludes "Note: including file:" markers into deps_path. Non-marker
 * lines are forwarded to stdout so the user still sees compile warnings
 * and errors.
 *
 * System headers from the MSVC toolchain and Windows SDK are filtered out;
 * they cannot be invalidated by edits to project sources.
 *
 * NOTE: The "Note: including file:" prefix is English-only. For other
 * locales, set VSLANG=1033 in the environment before launching the build.
 */
// Process one /showIncludes output line: either record an included header
// into the deps file (filtering system headers), or forward it to the
// log/stdout stream as normal compiler output.
static void
process_deps_line( char* line, FILE* deps, FILE* out )
{
    static const char   k_prefix[] = "Note: including file:";
    static const size_t k_prefix_n = sizeof( k_prefix ) - 1;

    char* p = line;
    while ( *p == ' ' || *p == '\t' ) ++p;

    if ( strncmp( p, k_prefix, k_prefix_n ) == 0 )
    {
        char* path = p + k_prefix_n;
        while ( *path == ' ' || *path == '\t' ) ++path;
        size_t l = strlen( path );
        while ( l > 0 && ( path[ l - 1 ] == '\r' || path[ l - 1 ] == '\n' ) ) path[ --l ] = '\0';
        if ( l == 0 ) return;

        bool is_system = strstr( path, "\\VC\\Tools\\" ) != NULL
                         || strstr( path, "\\Windows Kits\\" ) != NULL
                         || strstr( path, "/VC/Tools/" ) != NULL
                         || strstr( path, "/Windows Kits/" ) != NULL;
        if ( deps && !is_system ) fprintf( deps, "%s\n", path );
    }
    else
    {
        fputs( line, out );
        // line may not have a trailing newline (we may have flushed mid-line
        // due to a long line). Add one so the log stays readable.
        size_t l = strlen( line );
        if ( l == 0 || line[ l - 1 ] != '\n' ) fputc( '\n', out );
    }
}

int
build_run_cmd_capture_deps( const char* cmd, const char* deps_path )
{
    // CreatePipe + CreateProcess instead of _popen/_pclose, for the same
    // thread-safety reason explained in spawn_cmd(): _popen mutates the
    // parent's stdio fds during setup, and concurrent calls from multiple
    // worker threads race on that mutation.
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
    CloseHandle( wr );   // Parent's copy; child has its own.

    // Output sink: per-worker log if active, otherwise stdout.
    const char* log_path  = sched_log_path();
    FILE*       owned_log = NULL;
    FILE*       out       = stdout;
    if ( log_path )
    {
        owned_log = fopen( log_path, "a" );
        if ( owned_log ) out = owned_log;
    }
    fprintf( out, "[CMD] %s\n", cmd );

    FILE* deps = deps_path ? fopen( deps_path, "w" ) : NULL;

    // Stream the pipe into a line buffer; flush each full line.
    char  line[ 4096 ];
    size_t line_len = 0;
    char  buf[ 4096 ];
    DWORD br = 0;
    while ( ReadFile( rd, buf, sizeof( buf ), &br, NULL ) && br > 0 )
    {
        for ( DWORD i = 0; i < br; ++i )
        {
            char c = buf[ i ];
            if ( c == '\r' ) continue;
            if ( c == '\n' || line_len >= sizeof( line ) - 1 )
            {
                line[ line_len ] = '\0';
                process_deps_line( line, deps, out );
                line_len = 0;
            }
            else
            {
                line[ line_len++ ] = c;
            }
        }
    }
    // Flush any trailing partial line.
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
