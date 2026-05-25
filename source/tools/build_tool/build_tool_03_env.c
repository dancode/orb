/*==============================================================================================

    build_tool_03_env.c -- Visual Studio environment discovery and import.

    cl.exe requires ~50 environment variables (PATH, INCLUDE, LIB, LIBPATH, etc.)
    before it can run. Microsoft ships vcvarsall.bat to set them up.

    The naive approach of prepending "call vcvarsall.bat x64 &&" to every compiler
    invocation costs 200-500ms per call -- easily 10+ seconds wasted on a full build.

    Instead, we run vcvarsall ONCE at startup, capture every variable it sets by
    running "&& set" in the same sub-shell, and inject them into our own process via
    platform_putenv(). Every child process we spawn afterward inherits the modified
    environment automatically -- no per-invocation prefix needed.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    locate_vcvarsall()

    Try vswhere.exe first (the Microsoft-blessed VS discovery tool), then fall
    back to probing well-known install paths. Returns true and writes the full
    path to vcvarsall.bat into `out` on success.
==============================================================================================*/

static bool
locate_vcvarsall( char* out, size_t out_size )
{
    // Three candidate paths cover default installs and non-standard Program Files locations.
    const char* vswhere_paths[] = {
        "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
        "\"%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe\"",
    };

    for ( int i = 0; i < ( int )( sizeof( vswhere_paths ) / sizeof( vswhere_paths[ 0 ] ) ); ++i )
    {
        // Ask for the latest install across any product (Community / Pro / Build Tools / Preview).
        char cmd[ 1024 ];
        snprintf( cmd, sizeof( cmd ),
                  "%s -latest -products * -property installationPath",
                  vswhere_paths[ i ] );

        // Pipe vswhere's stdout and read the install path it prints.
        char inst[ 512 ] = { 0 };
        {
            FILE* pipe = platform_popen( cmd, "rt" );
            if ( !pipe ) continue;
            if ( fgets( inst, sizeof( inst ), pipe ) )
            {
                char* nl = strpbrk( inst, "\r\n" );
                if ( nl ) *nl = '\0';
            }
            platform_pclose( pipe );
        }
        if ( inst[ 0 ] )
        {
            // vcvarsall.bat is always at <install>\VC\Auxiliary\Build\.
            snprintf( out, out_size, "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", inst );
            if ( platform_file_exists( out ) ) return true;
        }
    }

    // vswhere missing or unhelpful -- fall back to probing well-known install locations.
    const char* common[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
    };
    for ( int i = 0; i < ( int )( sizeof( common ) / sizeof( common[ 0 ] ) ); ++i )
    {
        if ( platform_file_exists( common[ i ] ) )
        {
            snprintf( out, out_size, "%s", common[ i ] );
            return true;
        }
    }
    return false;
}

/*==============================================================================================
    import_vcvars_env()

    Run "vcvarsall x64 && set" in a sub-shell. vcvarsall mutates the sub-shell's
    environment, then set dumps every KEY=VALUE pair to stdout. We read those pairs
    and call platform_putenv() for each one, injecting the VC toolchain env into our own
    process. All child processes spawned after this point inherit it automatically.

    The double-quote idiom `cmd /c "" "<path>" args ""` is required because
    vcvarsall.bat always lives under "Program Files" (a path with spaces). The outer
    empty-string pair prevents cmd from stripping the inner quotes around the path.
==============================================================================================*/

static int
import_vcvars_env( const char* vcvars_path )
{
    char run_cmd[ 1024 ];
    snprintf( run_cmd, sizeof( run_cmd ),
              "cmd /c \"\"%s\" x64 >nul 2>nul && set\"", vcvars_path );

    FILE* pipe = platform_popen( run_cmd, "rt" );
    if ( !pipe )
    {
        printf( ORB_INDENT "[orb warn] could not spawn sub-shell for vcvars import\n" );
        return 0;
    }

    // PATH/INCLUDE/LIB can be very long under a full VS install -- 16KB gives safe headroom.
    char line[ 16384 ];

    int imported = 0;
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        // Strip CR/LF -- a newline-suffixed value would silently break any tool that doesn't trim.
        size_t l = strlen( line );
        while ( l > 0 && ( line[ l - 1 ] == '\n' || line[ l - 1 ] == '\r' ) ) line[ --l ] = '\0';
        if ( l == 0 ) continue;

        // Split KEY=VALUE on the first '='. Skip lines with no key.
        char* eq = strchr( line, '=' );
        if ( !eq || eq == line ) continue;
        *eq = '\0';

        const char* key   = line;
        const char* value = eq + 1;
        if ( platform_putenv( key, value ) == 0 ) ++imported;
    }

    platform_pclose( pipe );
    return imported;
}

/*==============================================================================================
    build_setup_vc_env()

    Idempotent entry point. SearchPathA checks whether cl.exe is already visible on
    PATH -- if so, we're inside a Developer Command Prompt and do nothing. Otherwise,
    locate vcvarsall.bat and import its environment into this process so every
    subsequent compiler spawn works without any per-invocation setup overhead.
==============================================================================================*/

void
build_setup_vc_env( void )
{
#if defined( _WIN32 )

    // Fast path: cl.exe already on PATH (Developer Command Prompt or VS-launched shell).
    char cl_path[ PATH_MAX ];
    if ( SearchPathA( NULL, "cl.exe", NULL, PATH_MAX, cl_path, NULL ) != 0 )
        return;

    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] cl.exe not in PATH, locating Visual Studio...\n" );

    char vcvars_path[ 512 ] = { 0 };
    if ( locate_vcvarsall( vcvars_path, sizeof( vcvars_path ) ) == false )
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
