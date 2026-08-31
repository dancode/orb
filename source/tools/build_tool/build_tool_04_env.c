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

    BUILD_SAFE_MODE and the cache:
    Safe mode exists because some EDR software flags build_tool.exe spawning
    cmd.exe (_popen for vswhere / the vcvarsall import). The cache sidesteps that:
    reading it is plain file I/O + putenv, and it is SEEDED without a subprocess --
    when a run is already inside a working VC environment (Developer Command
    Prompt / VS build), the process dumps its own environment to the cache file
    (vcvars_cache_save_current_env). One dev-prompt run seeds it; every later
    plain-terminal run loads it. Only cache *population via vcvarsall* stays
    disabled in safe mode.

==============================================================================================*/
// clang-format off

/* Set to false to always run the full vcvarsall import and never read or write the cache. */
static bool s_vcvars_cache_enabled = true;

#define VCVARS_CACHE_PATH  BUILD_DIR "\\.vcvars_x64"

/* The vcvars cache captures a machine-wide VS environment -- it is not project-specific.  When
   this run builds a CHILD project, g_engine_root is set (from the 'engine' directive) and the CWD
   is the project, whose build/ has no cache; the ENGINE's cache is the one to use.  Resolving the
   cache under g_engine_root lets a plain-terminal or launcher-driven child build reuse the engine's
   seeded cache instead of dropping into safe mode.  The engine's own build (g_engine_root empty)
   keeps the original CWD-relative path. */
static const char*
vcvars_cache_path( void )
{
    static char path[ PATH_MAX ];
    if ( g_engine_root[ 0 ] )
    {
        snprintf( path, sizeof( path ), "%s" PATH_SEP BUILD_DIR PATH_SEP ".vcvars_x64", g_engine_root );
        return path;
    }
    return VCVARS_CACHE_PATH;
}

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

    // -prerelease makes Insiders and Preview instances visible; without it they are invisible
    // to vswhere entirely.  -requires skips an install that carries no C++ toolset, which
    // -latest alone will happily return on a machine that also has a .NET-only VS.  The second
    // query is the retry for a vswhere old enough to reject either switch.
    const char* vswhere_args[] = {
        "-latest -prerelease -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        "-property installationPath",
        "-latest -products * -property installationPath",
    };

    for ( int q = 0; q < ( int )( sizeof( vswhere_args ) / sizeof( vswhere_args[ 0 ] ) ); ++q )
    {
        for ( int i = 0; i < ( int )( sizeof( vswhere_paths ) / sizeof( vswhere_paths[ 0 ] ) ); ++i )
        {
            char cmd[ 1024 ];
            snprintf( cmd, sizeof( cmd ), "%s %s", vswhere_paths[ i ], vswhere_args[ q ] );

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
    }
#endif

    // vswhere missing or unhelpful -- fall back to probing well-known install locations.
    // Dev18 (VS 2026) drops the year from the path and uses the major version instead.
    const char* common[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Preview\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Professional\\VC\\Auxiliary\\Build\\vcvarsall.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat",
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
    vcvars_cache_save_current_env()

    Seed the cache from the CURRENT process environment -- used when this run is
    already inside a working VC environment (Developer Command Prompt, VS build),
    where vcvarsall's variables are simply our own. Pure file I/O, no subprocess,
    so it also runs under BUILD_SAFE_MODE -- this is what makes later plain-terminal
    builds possible there.

    Written to a per-process temp file and renamed into place: VS solution builds
    spawn several build_tool processes in parallel, and racing direct writers on
    one cache file would interleave into a corrupt environment.

    Returns the number of variables written, 0 on failure.
==============================================================================================*/

#if defined( _WIN32 )
static int
vcvars_cache_save_current_env( const char* cache_path )
{
    /* Ensure the cache file's own directory exists -- BUILD_DIR under the CWD for the engine's
       build, or the engine's build/ when cache_path is engine-rooted (child project build). */
    char dir[ PATH_MAX ];
    snprintf( dir, sizeof( dir ), "%s", cache_path );
    char* sep = strrchr( dir, '\\' );
    if ( !sep ) sep = strrchr( dir, '/' );
    if ( sep ) { *sep = '\0'; ensure_dir( dir ); }
    else       ensure_dir( BUILD_DIR );

    char tmp_path[ PATH_MAX ];
    snprintf( tmp_path, sizeof( tmp_path ), "%s.%lu.tmp", cache_path,
              ( unsigned long )GetCurrentProcessId() );

    FILE* cache = fopen( tmp_path, "w" );
    if ( !cache )
        return 0;

    int   written = 0;
    char* block   = GetEnvironmentStringsA();
    if ( block )
    {
        // Double-NUL-terminated list of "KEY=VALUE" entries. Entries starting with
        // '=' are cmd.exe's hidden per-drive CWD variables ("=C:=C:\...") -- skip.
        for ( const char* p = block; *p; p += strlen( p ) + 1 )
        {
            if ( *p == '=' ) continue;
            fprintf( cache, "%s\n", p );
            ++written;
        }
        FreeEnvironmentStringsA( block );
    }
    fclose( cache );

    if ( written == 0 || !MoveFileExA( tmp_path, cache_path, MOVEFILE_REPLACE_EXISTING ) )
    {
        remove( tmp_path );
        return 0;
    }
    return written;
}
#endif

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

    Compiled out under BUILD_SAFE_MODE: the only call site is in the non-safe-mode
    branch of build_setup_vc_env(), and _popen is unavailable there anyway.
==============================================================================================*/

#if !defined( BUILD_SAFE_MODE )
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
#endif

/*==============================================================================================
    build_setup_vc_env()

    Idempotent entry point. Three fast paths avoid the 200-500ms vcvarsall cost:
      1. VSCMD_ARG_TGT_ARCH == "x64": vcvarsall was already run for x64.
      2. cl.exe is in PATH under a \x64\ directory: VS has wired up an x64
         compiler without running vcvarsall (some NMake launch contexts do this).
         Paths 1/2 also SEED the cache from the live environment when it is
         missing (no subprocess -- see vcvars_cache_save_current_env).
      3. Cache hit (s_vcvars_cache_enabled): cache file exists and is not older
         than vcvarsall.bat, so the saved KEY=VALUE pairs are loaded directly.
         This path is available in BUILD_SAFE_MODE too -- it spawns nothing.
    If no fast path fires: locate vcvarsall.bat, run the full import, and write
    the cache so the next cold launch hits fast path 3. Safe mode cannot import
    (it needs _popen), so there it warns with the seed instructions instead.
==============================================================================================*/

void
build_setup_vc_env( void )
{
    // Several command paths call this; the first one does the work and the rest are free.
    static bool s_env_ready = false;
    if ( s_env_ready ) return;
    s_env_ready = true;

#if defined( _WIN32 )

    // Fast path 1: vcvarsall already loaded for x64 -- VSCMD_ARG_TGT_ARCH is set by
    // vcvarsall.bat to the target architecture ("x64", "x86", etc.).
    // Fast path 2: cl.exe is in PATH and lives under a \x64\ directory, meaning VS has
    // already wired up an x64 target compiler without setting VSCMD_ARG_TGT_ARCH.
    // A path check is necessary because VS may also inject HostX86\x86\cl.exe into
    // PATH without vcvars -- accepting that one silently produces 32-bit output.
    /* Resolve once: engine-rooted for a child build, CWD-relative for the engine's own. */
    const char* cache_path = vcvars_cache_path();

    const char* tgt_arch  = getenv( "VSCMD_ARG_TGT_ARCH" );
    bool        env_ready = ( tgt_arch && strcmp( tgt_arch, "x64" ) == 0 );
    if ( !env_ready )
    {
        char cl_path[ PATH_MAX ];
        if ( SearchPathA( NULL, "cl.exe", NULL, PATH_MAX, cl_path, NULL ) != 0 )
            env_ready = ( strstr( cl_path, "\\x64\\" ) != NULL );
    }

    if ( env_ready )
    {
        // Seed the cache from this live environment when none exists yet -- the only
        // way the cache gets populated under BUILD_SAFE_MODE. Missing-only (no
        // staleness probe) keeps this path free of vswhere/_popen and avoids
        // rewrite races across VS's parallel per-project build_tool spawns.
        // Delete the file (or -clean, or a normal import) to force a re-seed.
        if ( s_vcvars_cache_enabled && platform_get_mtime( cache_path ) == 0 )
        {
            int n = vcvars_cache_save_current_env( cache_path );
            if ( n > 0 && ( g_out_flags & ORB_OUT_VCVARS ) )
                printf( ORB_INDENT "[orb vcvars] cache seeded from current env (%d variables)\n", n );
        }
        return;
    }

    // Locate vcvarsall for the staleness check and the (non-safe) import. In safe
    // mode locate_vcvarsall skips vswhere and only probes fixed paths -- no _popen.
    char vcvars_path[ 512 ] = { 0 };
    bool have_vcvars        = locate_vcvarsall( vcvars_path, sizeof( vcvars_path ) );

    // Fast path 3: valid cache -- no subprocess, so safe mode may take it too.
    // When vcvarsall cannot be located (nonstandard install + safe mode) the cache
    // is trusted as-is: worst case is stale paths, fixed by re-seeding.
    if ( s_vcvars_cache_enabled )
    {
        platform_mtime_t cache_mtime = platform_get_mtime( cache_path );
        bool cache_ok = ( cache_mtime > 0 ) &&
                        ( !have_vcvars || cache_mtime >= platform_get_mtime( vcvars_path ) );
        if ( cache_ok )
        {
            int n = vcvars_cache_load( cache_path );
            if ( n > 0 )
            {
                if ( g_out_flags & ORB_OUT_VCVARS )
                    printf( ORB_INDENT "[orb vcvars] loaded %d variables from cache\n", n );
                return;
            }
            // Cache unreadable or empty -- fall through.
        }
    }

#if defined( BUILD_SAFE_MODE )
    /* vcvarsall auto-import uses _popen(cmd.exe) -- disabled in safe mode. */
    printf( ORB_INDENT "[orb warn] BUILD_SAFE_MODE: vcvarsall auto-import disabled and no cache (%s).\n",
            cache_path );
    printf( ORB_INDENT "           Run any build_tool command once from a Developer Command Prompt\n" );
    printf( ORB_INDENT "           to seed the cache; plain-terminal builds work from then on.\n" );
    return;
#else
    if ( !have_vcvars )
    {
        printf( ORB_INDENT "[orb warn] could not locate vcvarsall.bat, compiler calls will fail\n" );
        return;
    }

    if ( g_out_flags & ORB_OUT_VCVARS )
        printf( ORB_INDENT "[orb vcvars] importing from %s\n", vcvars_path );

    const char* write_cache = s_vcvars_cache_enabled ? cache_path : NULL;
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
