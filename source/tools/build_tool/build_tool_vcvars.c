/*==============================================================================================

    build_tool_vcvars.c -- Visual Studio environment discovery and import.

    The legacy approach was to prepend "call vcvarsall.bat x64 && " to every
    cl/link/lib invocation. vcvarsall takes 200-500 ms to run, and a real
    engine build issues dozens of compiler calls, so that overhead can easily
    cost 10+ seconds of wall time per build.

    Instead, we now run vcvarsall ONCE at orchestrator startup, capture the
    environment variables it sets via "&& set", and apply them to our own
    process with _putenv_s(). Every child process we spawn after that inherits
    the modified environment for free -- no per-invocation prefix needed.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- VC Environment Locate ---

    Find a usable vcvarsall.bat on this machine and write its absolute path
    to `out`. Two-stage lookup:

  1. Ask vswhere.exe (the Microsoft-blessed VS discovery tool) for the
     latest installation, then derive the vcvarsall path from it. 
     
     This tracks per-machine installs, side-by-side VS versions, and the
     Preview / Build Tools SKUs without us hard-coding anything.

  2. If vswhere is missing or returned nothing usable, fall back to
     probing a small list of well-known install locations.

    Returns true and fills `out` on success. False means no VS install was
    discovered -- caller should print a warning and let cl.exe lookups fail
    naturally so the error is easy to diagnose.

    vswhere paths are double-quoted because the default install location
    lives under "Program Files (x86)" with a space; cmd.exe needs the quotes
    to treat the whole executable path as a single token. _popen wraps it in
    cmd.exe automatically.
 
==============================================================================================*/

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
        // Ask for the latest install, any product (Community / Pro / Build Tools / Preview), 
        // and print just the install path on stdout.

        char cmd[ 1024 ];
        snprintf( cmd, sizeof( cmd ), 
                  "%s -latest -products * -property installationPath",
                  vswhere_paths[ i ] );

        // Runtime (CRT) function used in Windows console apps to create a pipe and 
        // execute a command. We are trying to run vswhere.exe and capture its output. 
        // The "rt" mode means we want to read text output from the command.

        char inst[ 512 ] = { 0 };
        {
            FILE* pipe = _popen( cmd, "rt" );
            if ( !pipe ) continue;        
            if ( fgets( inst, sizeof( inst ), pipe ) )
            {
                // The install path output by vswhere, or empty.
                // Strip the trailing newline that fgets keeps.                        
                char* nl = strpbrk( inst, "\r\n" );
                if ( nl ) *nl = '\0';
            }
            _pclose( pipe );
        }
        if ( inst[ 0 ] )
        {
            // vcvarsall.bat is always at <install>\VC\Auxiliary\Build\.
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

/**
 * import_vcvars_env()
 *
 * Run "<vcvarsall> x64 && set" in a sub-shell and apply every printed
 * KEY=VALUE line to our own process environment via _putenv_s. Subsequent
 * CreateProcess children inherit the modified env, so cl/link/lib become
 * directly callable for the lifetime of THIS process.
 *
 * One-time cost (~2.5s) replaces the legacy "prepend `call vcvarsall.bat &&`
 * to every compiler call" pattern, which would otherwise add 200-500 ms x N
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
        // means the line has no key -- `set` shouldn't emit these but skip
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

/*==============================================================================================
    --- Build Setup VC Environment ---

    Idempotent one-time setup. If cl.exe is already discoverable in PATH
    (Developer Command Prompt or pre-sourced vcvars), do nothing. 
    
    Otherwise locate vcvarsall.bat and import its environment into THIS process so
    every subsequent child invocation runs with the VC toolchain visible.
 
==============================================================================================*/

void
build_setup_vc_env( void )
{
#if defined( _WIN32 )

    // Fast path: cl.exe already on PATH. SearchPathA walks PATH without
    // spawning a child process, so this is free compared to system().
    char cl_path[ MAX_PATH ];
    if ( SearchPathA( NULL, "cl.exe", NULL, MAX_PATH, cl_path, NULL ) != 0 ) 
        return;

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

// clang-format on
/*============================================================================================*/
