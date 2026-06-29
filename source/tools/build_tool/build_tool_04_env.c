/*==============================================================================================

    build_tool_04_env.c -- Visual Studio environment discovery and import.

    cl.exe requires ~50 environment variables (PATH, INCLUDE, LIB, LIBPATH, etc.)
    before it can run. Microsoft ships vcvarsall.bat to set them up.

    The naive approach of prepending "call vcvarsall.bat x64 &&" to every compiler
    invocation costs 200-500ms per call -- easily 10+ seconds wasted on a full build.

    Instead, we run vcvarsall ONCE at startup, capture every variable it sets by
    running "&& set" in the same sub-shell, and inject them into our own process via
    platform_putenv(). Every child process we spawn afterward inherits the modified
    environment automatically -- no per-invocation prefix needed.

    Cache (s_vcvars_cache_enabled):
    When enabled, the KEY=VALUE pairs captured from vcvarsall are written to
    VCVARS_CACHE_PATH. On subsequent cold launches the cache is read directly --
    no subprocess needed (~microseconds vs. 200-500ms). The cache is invalidated
    automatically whenever vcvarsall.bat is newer than the cache file (i.e. after
    a VS update).

==============================================================================================*/
// clang-format off

/* Set to false to always run the full vcvarsall import and never read or write the cache. */
static bool s_vcvars_cache_enabled = true;

#define VCVARS_CACHE_PATH  BUILD_DIR "\\.vcvars_x64"

/*==============================================================================================
    locate_vcvarsall()

    Try vswhere.exe first (the Microsoft-blessed VS discovery tool), then fall
    back to probing well-known install paths. Returns true and writes the full
    path to vcvarsall.bat into `out` on success.
==============================================================================================*/

static bool
locate_vcvarsall( char* out, size_t out_size )
{
#if !defined( BUILD_SAFE_MODE )
    // Three candidate paths cover default installs and non-standard Program Files locations.
    // vswhere spawns cmd.exe via _popen -- skipped in safe mode.
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
#endif

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
    vcvars_cache_load()

    Read KEY=VALUE lines from the cache file and inject each pair into the process
    environment via platform_putenv(). Uses memory-mapped I/O.
    Returns the number of variables imported, or -1 if the file could not be opened.
==============================================================================================*/

static int
vcvars_cache_load( const char* path )
{
    platform_mapped_file_t mf;
    if ( !platform_map_file( path, &mf ) )
        return -1;

    if ( !mf.data )
    {
        platform_unmap_file( &mf );
        return 0;
    }

    // PATH/INCLUDE/LIB can be very long under a full VS install -- 16KB gives safe headroom.
    char line[ 16384 ];

    int         imported = 0;
    const char* p        = mf.data;
    const char* end      = mf.data + mf.size;

    while ( mmap_next_line( &p, end, line, sizeof( line ) ) )
    {
        if ( !line[ 0 ] ) continue;

        // Split KEY=VALUE on the first '='. Skip lines with no key.
        char* eq = strchr( line, '=' );
        if ( !eq || eq == line ) continue;
        *eq = '\0';

        if ( platform_putenv( line, eq + 1 ) == 0 ) ++imported;
    }

    platform_unmap_file( &mf );
    return imported;
}

/*==============================================================================================
    import_vcvars_env()

    Run "vcvarsall x64 && set" in a sub-shell. vcvarsall mutates the sub-shell's
    environment, then set dumps every KEY=VALUE pair to stdout. We read those pairs
    and call platform_putenv() for each one, injecting the VC toolchain env into our own
    process. All child processes spawned after this point inherit it automatically.

    If cache_path is non-NULL each imported pair is also written to that file so
    vcvars_cache_load() can replay them on the next cold launch without a subprocess.

    The double-quote idiom `cmd /c "" "<path>" args ""` is required because
    vcvarsall.bat always lives under "Program Files" (a path with spaces). The outer
    empty-string pair prevents cmd from stripping the inner quotes around the path.
==============================================================================================*/

static int
import_vcvars_env( const char* vcvars_path, const char* cache_path )
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

    // Open cache file for writing if caching is requested.
    FILE* cache = NULL;
    if ( cache_path )
    {
        ensure_dir( BUILD_DIR );
        cache = fopen( cache_path, "w" );
        if ( !cache && ( g_out_flags & ORB_OUT_VCVARS ) )
            printf( ORB_INDENT "[orb vcvars] could not write cache: %s\n", cache_path );
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
        if ( platform_putenv( key, value ) == 0 )
        {
            ++imported;
            if ( cache ) fprintf( cache, "%s=%s\n", key, value );
        }
    }

    if ( cache  ) fclose( cache );
    platform_pclose( pipe );
    return imported;
}

/*==============================================================================================
    build_setup_vc_env()

    Idempotent entry point. Three fast paths avoid the 200-500ms vcvarsall cost:
      1. VSCMD_ARG_TGT_ARCH == "x64": vcvarsall was already run for x64.
      2. cl.exe is in PATH under a \x64\ directory: VS has wired up an x64
         compiler without running vcvarsall (some NMake launch contexts do this).
      3. Cache hit (s_vcvars_cache_enabled): cache file is newer than vcvarsall.bat,
         so the saved KEY=VALUE pairs are loaded directly -- no subprocess needed.
    If no fast path fires, locate vcvarsall.bat, run the full import, and write
    the cache (when enabled) so the next cold launch hits fast path 3.
==============================================================================================*/

void
build_setup_vc_env( void )
{
#if defined( _WIN32 )

    // Fast path 1: vcvarsall already loaded for x64 -- VSCMD_ARG_TGT_ARCH is set by
    // vcvarsall.bat to the target architecture ("x64", "x86", etc.).
    const char* tgt_arch = getenv( "VSCMD_ARG_TGT_ARCH" );
    if ( tgt_arch && strcmp( tgt_arch, "x64" ) == 0 )
        return;

    // Fast path 2: cl.exe is in PATH and lives under a \x64\ directory, meaning VS has
    // already wired up an x64 target compiler without setting VSCMD_ARG_TGT_ARCH.
    // A path check is necessary because VS may also inject HostX86\x86\cl.exe into
    // PATH without vcvars -- accepting that one silently produces 32-bit output.
    char cl_path[ PATH_MAX ];
    if ( SearchPathA( NULL, "cl.exe", NULL, PATH_MAX, cl_path, NULL ) != 0 )
    {
        if ( strstr( cl_path, "\\x64\\" ) )
            return;
    }

#if defined( BUILD_SAFE_MODE )
    /* vcvarsall auto-import uses _popen(cmd.exe) -- disabled in safe mode. */
    printf( ORB_INDENT "[orb warn] BUILD_SAFE_MODE: vcvarsall auto-import disabled.\n" );
    printf( ORB_INDENT "           Run from a Developer Command Prompt to reach cl.exe.\n" );
    return;
#else
    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] VSCMD_ARG_TGT_ARCH != x64, locating Visual Studio...\n" );

    char vcvars_path[ 512 ] = { 0 };
    if ( locate_vcvarsall( vcvars_path, sizeof( vcvars_path ) ) == false )
    {
        printf( ORB_INDENT "[orb warn] could not locate vcvarsall.bat, compiler calls will fail\n" );
        return;
    }

    // Fast path 3: valid cache -- skip the vcvarsall subprocess entirely.
    if ( s_vcvars_cache_enabled )
    {
        // If VS updates and touches vcvarsall.bat, the cache is stale and
        // the next run does a full re-import and rewrites it
        platform_mtime_t cache_mtime  = platform_get_mtime( VCVARS_CACHE_PATH );
        platform_mtime_t vcvars_mtime = platform_get_mtime( vcvars_path );
        if ( cache_mtime > 0 && cache_mtime >= vcvars_mtime )
        {
            int n = vcvars_cache_load( VCVARS_CACHE_PATH );
            if ( n > 0 )
            {
                if ( g_out_flags & ORB_OUT_VCVARS )
                    printf( ORB_INDENT "[orb vcvars] loaded %d variables from cache\n", n );
                return;
            }
            // Cache unreadable or empty -- fall through to full import.
        }
    }

    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] importing from %s\n", vcvars_path );

    const char* write_cache = s_vcvars_cache_enabled ? VCVARS_CACHE_PATH : NULL;
    int n = import_vcvars_env( vcvars_path, write_cache );
    if ( g_out_flags & ORB_OUT_VCVARS )
    {
        printf( ORB_INDENT "[orb vcvars] imported %d environment variables\n", n );
        if ( write_cache )
            printf( ORB_INDENT "[orb vcvars] cache written: %s\n", write_cache );
    }
#endif

#endif
}

/*==============================================================================================
    build_detect_vs_major()

    Returns the VS internal major version for project file generation. Resolution:
      1. g_vs_major_version > 0 -- explicit override from -vs-version <year>.
      2. VisualStudioVersion env var (set by vcvarsall / Developer Command Prompt).
      3. Fallback: 17 (VS 2022).

    VS year-to-major mapping:  2015->14  2017->15  2019->16  2022->17  2026->18
    MSBuild toolset:  major + 126  (17->v143, 18->v144, ...)
==============================================================================================*/

int
build_detect_vs_major( void )
{
    if ( g_vs_major_version > 0 )
        return g_vs_major_version;

    // VisualStudioVersion is set to e.g. "17.12.3.4" by vcvarsall or Dev Cmd Prompt.
    const char* ver = getenv( "VisualStudioVersion" );
    if ( ver )
    {
        int major = atoi( ver );
        if ( major >= 14 ) return major;
    }

    return 17;    // fallback: VS 2022
}

// clang-format on
/*============================================================================================*/
