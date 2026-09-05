/*==============================================================================================

    build_tool_12_gen_vs.c -- Shared Visual Studio solution infrastructure.

    ORB emits two different Visual Studio project formats from the same target registry:

      -gen     Makefile-type projects (build_tool_12_gen_nmake.c)   -> build/proj
      -gen_ms  native MSBuild projects (build_tool_12_gen_msbuild.c) -> build/proj_ms

    The two formats diverge substantially -- one shells every build out to build_tool.exe
    through NMake* properties, the other drives cl.exe through ClCompile/Link item
    definitions and ProjectReference -- so each owns its own emitter. This file holds what
    is the same either way, and both emitters build on it:

      Per-solution context  -- s_ctx: output dir, "..\"-depth prefixes, monolithic flag
      GUIDs                 -- guid_from_name(): name-hashed, stable across reordering
      Directory scan        -- g_files[] / g_filters[]: the source tree behind one project
      IntelliSense values   -- define and include-dir strings matching the real compile
      Shared XML fragments  -- debugger property groups, natvis items, the .filters file
      Navigation project    -- the "Mega" nav .vcxproj, emitted into both solutions
      Solution writer       -- the .sln itself; its format does not depend on project type
      Solution driver       -- run_solution_passes(): the per-solution loop both -gen and
                               -gen_ms run, parameterized by a per-target emitter callback

==============================================================================================*/

/*==============================================================================================
    gen_ctx_t -- Per-solution generation context.

    Set once at the top of each solution's generation pass (run_solution_passes ->
    compute_path_parts). All emitter functions read s_ctx instead of hardcoding depth
    relative paths -- moving a solution's output directory only requires changing
    out_dir in the solution descriptor, no string edits anywhere else.
==============================================================================================*/

typedef struct
{
    char         out_dir[ PATH_MAX ];           // where .sln / .vcxproj files land (copy)
    char         root_prefix[ 32 ];             // "..\..\"-style depth prefix
    char         cd_root[ 32 ];                 // "..\.."-style cd target for NMake commands
    bool         is_monolithic;                 // solution is_monolithic flag
    char         cwd_prefix[ PATH_MAX ];        // CWD with trailing backslash (for scan strip)
    const char** sln_extra_include_dirs;        // solution-level include_dirs
    char         build_tool_exe[ PATH_MAX + 2 ];// NMake cmd format, quoted if absolute path

} gen_ctx_t;

static gen_ctx_t s_ctx = { { 0 }, { 0 }, { 0 }, false, { 0 }, NULL, "bin\\build_tool.exe" };

static void
compute_path_parts( const char* out_dir )
{
    // Count directory components: "build\\proj" has one separator -> depth 2.
    int depth = 1;
    for ( const char* p = out_dir; *p; ++p )
        if ( *p == '\\' || *p == '/' )
            depth++;

    snprintf( s_ctx.out_dir, sizeof( s_ctx.out_dir ), "%s", out_dir );

    s_ctx.root_prefix[ 0 ] = '\0';
    for ( int i = 0; i < depth; ++i ) strcat( s_ctx.root_prefix, "..\\" );

    // cd_root is the same path without the trailing backslash.
    s_ctx.cd_root[ 0 ] = '\0';
    strcat( s_ctx.cd_root, ".." );
    for ( int i = 1; i < depth; ++i ) strcat( s_ctx.cd_root, "\\.." );

    // Capture CWD once so scan can strip the absolute prefix from root_dir paths.
    platform_get_cwd( s_ctx.cwd_prefix, sizeof( s_ctx.cwd_prefix ) );
}

// Build the Include path for a scanned file entry.
// CWD-relative paths get s_ctx.root_prefix; absolute cross-project paths emit as-is.
static void
gen_inc_path( const char* file_path, char* buf, size_t buf_size )
{
    if ( platform_is_abs_path( file_path ) )
        snprintf( buf, buf_size, "%s", file_path );
    else
        snprintf( buf, buf_size, "%s%s", s_ctx.root_prefix, file_path );
}

/*==============================================================================================
    --- GUIDs and Platform Toolset ---
==============================================================================================*/

/*  Buffer size for one GUID string: "{8-4-4-4-12}" is 38 characters plus the NUL.
    Every guid_from_name() destination in all three generator files is declared at this size. */

#define GUID_STR_MAX    40

// Returns the PlatformToolset string: $(DefaultPlatformToolset) by default, or vNNN when
// -vs-version was explicitly passed. Both project formats emit it.
static const char*
gen_platform_toolset( char* buf, size_t buf_size )
{
    if ( g_vs_major_version > 0 )
        snprintf( buf, buf_size, "v%d", g_vs_major_version + 126 );
    else
        snprintf( buf, buf_size, "$(DefaultPlatformToolset)" );
    return buf;
}

/*==============================================================================================
    guid_from_name()

    Produces a deterministic 128-bit GUID from a string in standard 8-4-4-4-12 form.
    The GUID is keyed on the name alone, so a project keeps its identity -- and with it
    the VS user state attached to that identity (.suo files, breakpoints,
    ProjectDependencies) -- no matter where the target sits in g_targets[].

    Two FNV-1a passes with different (seed, prime) pairs fill the 16 bytes.
==============================================================================================*/

static void
guid_from_name( const char* name, char* out, size_t out_size )
{
    // FNV-1a 64-bit variant
    unsigned long long h1 = 0xcbf29ce484222325ULL;
    unsigned long long h2 = 0x9ae16a3b2f90404fULL;
    for ( const unsigned char* p = ( const unsigned char* )name; *p; ++p )
    {
        h1 = ( h1 ^ *p ) * 0x100000001b3ULL;
        h2 = ( h2 ^ *p ) * 0x880355f21e6d1965ULL;
    }

    snprintf( out, out_size, "{%08X-%04X-%04X-%04X-%04X%08X}", ( unsigned int )( h1 >> 32 ),
              ( unsigned int )( ( h1 >> 16 ) & 0xFFFFu ), ( unsigned int )( h1 & 0xFFFFu ),
              ( unsigned int )( h2 >> 48 ), ( unsigned int )( ( h2 >> 32 ) & 0xFFFFu ),
              ( unsigned int )( h2 & 0xFFFFFFFFu ) );
}

/*==============================================================================================
    --- Directory Scan and Filters ---

    One project's source tree, scanned into a flat file table plus the set of virtual
    Solution Explorer folders those files nest under. Both project formats scan the same
    way and emit the same .vcxproj.filters from the result; only the per-file item tags
    in the .vcxproj itself differ.
==============================================================================================*/

#define MAX_FILES   1024    // Max source files scanned per vcxproj.
#define MAX_FILTERS 512     // Max virtual filter folders per vcxproj.

// Metadata for one source file picked up by scan_directory_recursive().
typedef struct
{
    char path[ PATH_MAX ];      // Relative path from project root.
    char filter[ PATH_MAX ];    // Virtual folder path in VS (e.g. "engine\\core").
    bool is_header;             // True for .h files.
    bool is_natvis;             // True for .natvis debugger visualizer files.

} file_info_t;

// Per-scan buffers. Reset via gen_scan_reset() at the start of every project
// emission -- reusable scratch, not persistent state.

static file_info_t g_files[ MAX_FILES ];
static int         g_file_count = 0;
static bool        g_files_full = false;    // MAX_FILES warning already issued this scan

static char        g_filters[ MAX_FILTERS ][ PATH_MAX ];
static int         g_filter_count = 0;
static bool        g_filters_full = false;   // MAX_FILTERS warning already issued this scan

// Clear the scan buffers. Every scan_directory_recursive() caller starts here.
static void
gen_scan_reset( void )
{
    g_file_count   = 0;
    g_files_full   = false;
    g_filter_count = 0;
    g_filters_full = false;
}

/*==============================================================================================
    path_walk_prefixes()

    Calls fn once per ancestor of a separator-joined path, shortest first, then once
    for the whole path: "engine\core\win" yields "engine", "engine\core",
    "engine\core\win". Both VS folder registries need every ancestor present, not just
    the leaf -- a gap in the chain makes the folders below it silently fail to nest.

    fn receives a temporary buffer, so it must copy anything it keeps.
==============================================================================================*/

typedef void ( *path_prefix_fn_t )( const char* prefix, void* ctx );

static void
path_walk_prefixes( const char* path, char sep, path_prefix_fn_t fn, void* ctx )
{
    char tmp[ PATH_MAX ];
    snprintf( tmp, sizeof( tmp ), "%s", path );

    for ( char* p = tmp; *p; ++p )
    {
        if ( *p == sep )
        {
            *p = '\0';
            fn( tmp, ctx );
            *p = sep;
        }
    }
    fn( tmp, ctx );    // The leaf: the whole path.
}

// Register one virtual filter folder in g_filters[]. Idempotent, and case-insensitive
// because filter paths are built from on-disk directory names. ctx is unused -- the
// destination is the per-vcxproj scratch table, not a caller-supplied one.
static void
add_filter( const char* filter, void* ctx )
{
    ( void )ctx;

    if ( filter[ 0 ] == '\0' )
        return;
    for ( int i = 0; i < g_filter_count; ++i )
    {
        if ( str_icmp( g_filters[ i ], filter ) == 0 )
            return;
    }

    if ( g_filter_count >= MAX_FILTERS )
    {
        // Once per scan: add_filters_recursive() runs per file, so the same overflowing
        // folder is re-offered many times over.
        if ( !g_filters_full )
        {
            printf( ORB_INDENT "[orb warn] filter table full (MAX_FILTERS=%d); '%s' and any"
                    " further folders will not appear in the project tree\n", MAX_FILTERS, filter );
            g_filters_full = true;
        }
        return;
    }
    strcpy( g_filters[ g_filter_count++ ], filter );
}

// Register a filter path and every folder above it. Filter paths are backslash-joined.
static void
add_filters_recursive( const char* filter )
{
    path_walk_prefixes( filter, '\\', add_filter, NULL );
}

// Normalize all forward slashes to backslashes in place (vcxproj paths must be consistent).
static void
normalize_slashes( char* s )
{
    for ( ; *s; ++s )
        if ( *s == '/' ) *s = '\\';
}

// Map a physical file path to a virtual VS filter path relative to root_dir.
// Example: path "source/engine/core/core.c", root_dir "source/engine" -> "core"
static void
get_filter_for_path( const char* path, const char* root_dir, char* out_filter )
{
    out_filter[ 0 ] = '\0';

    // Normalize root_dir to backslashes so it matches the already-normalized path.
    char norm_root[ PATH_MAX ];
    snprintf( norm_root, sizeof( norm_root ), "%s", root_dir );
    normalize_slashes( norm_root );

    size_t root_len = strlen( norm_root );
    if ( strncmp( path, norm_root, root_len ) != 0 )
        return;

    const char* sub = path + root_len;
    if ( *sub == '/' || *sub == '\\' )
        sub++;

    const char* last_slash = strrchr( sub, '/' );
    if ( !last_slash )
        last_slash = strrchr( sub, '\\' );
    if ( !last_slash )
        return;    // File is directly under root_dir -> empty filter.

    size_t len = last_slash - sub;
    strncpy( out_filter, sub, len );
    out_filter[ len ] = '\0';
    for ( char* p = out_filter; *p; ++p )
        if ( *p == '/' )
            *p = '\\';
}

// Walk a directory tree and accumulate every .c and .h file into g_files[].
static void
scan_directory_recursive( const char* dir, const char* root_dir )
{
    char search_path[ PATH_MAX ];
    snprintf( search_path, sizeof( search_path ), "%s" PATH_SEP "*", dir );

    platform_find_data_t find_data;
    platform_find_t handle = platform_find_first( search_path, &find_data );
    if ( handle == PLATFORM_FIND_INVALID )
        return;

    do
    {
        if ( strcmp( find_data.name, "." ) == 0 || strcmp( find_data.name, ".." ) == 0 )
            continue;

        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "%s" PATH_SEP "%s", dir, find_data.name );
        normalize_slashes( path );

        if ( find_data.is_dir )
        {
            scan_directory_recursive( path, root_dir );
        }
        else
        {
            const char* ext = strrchr( find_data.name, '.' );
            if ( !ext )
                continue;

            bool is_c      = str_icmp( ext, ".c" ) == 0;
            bool is_h      = str_icmp( ext, ".h" ) == 0;
            bool is_natvis = str_icmp( ext, ".natvis" ) == 0;
            if ( !( is_c || is_h || is_natvis ) )
                continue;
            if ( g_file_count >= MAX_FILES )
            {
                // Warn once per scan: an omitted file looks like a missing source in VS,
                // not like a table that filled up.
                if ( !g_files_full )
                {
                    printf( ORB_INDENT "[orb warn] file table full (MAX_FILES=%d) scanning %s;"
                            " remaining files omitted from the project\n", MAX_FILES, root_dir );
                    g_files_full = true;
                }
                continue;
            }

            file_info_t* f = &g_files[ g_file_count++ ];

            // Strip CWD prefix from absolute paths (produced when root_dir is absolute)
            // so stored paths remain project-root-relative for vcxproj Include emit.
            // Cross-project absolute paths (outside CWD) are kept as-is.
            size_t cwd_len = strlen( s_ctx.cwd_prefix );
            if ( cwd_len > 0 && str_nicmp( path, s_ctx.cwd_prefix, cwd_len ) == 0 )
                strcpy( f->path, path + cwd_len );
            else
                strcpy( f->path, path );

            f->is_header = is_h;
            f->is_natvis = is_natvis;
            get_filter_for_path( path, root_dir, f->filter );
            if ( f->filter[ 0 ] != '\0' )
                add_filters_recursive( f->filter );
        }
    }
    while ( platform_find_next( handle, &find_data ) );

    platform_find_close( handle );
}

// Returns true if path's filename component matches one of target's unity units.
static bool
is_unit_file( const target_info_t* target, const char* path )
{
    const char* filename = path;
    for ( const char* p = path; *p; ++p )
        if ( *p == '/' || *p == '\\' ) filename = p + 1;
    for ( int j = 0; target->units[ j ]; ++j )
    {
        // A unit may carry a subdirectory prefix (e.g. "pack/pack_miniz.c"). Compare by
        // basename -- matching how the object is named (<basename>.obj) -- so a subdir unit
        // is still recognized as a compile unit, not misfiled as a non-compiled include.
        const char* uname = target->units[ j ];
        for ( const char* p = uname; *p; ++p )
            if ( *p == '/' || *p == '\\' ) uname = p + 1;
        if ( str_icmp( filename, uname ) == 0 ) return true;
    }
    return false;
}

/*==============================================================================================
    build_extra_include_dirs_str()

    Build a semicolon-separated string of extra include directories from the
    target's own 'include_dir' declarations and the current solution's 'include_dir'
    declarations. Absolute paths stored in extra_include_dirs[] are emitted as-is.
    Used for both AdditionalIncludeDirectories (vcxproj) and NMakeIncludeSearchPath.
    An empty string is returned when no extras are defined.
==============================================================================================*/

static void
build_extra_include_dirs_str( const target_info_t* target, char* buf, size_t buf_size )
{
    buf[ 0 ]    = '\0';
    size_t used = 0;

    /* Target-level include_dirs first. */
    if ( target )
    {
        for ( int i = 0; i < MAX_EXTRA_INCLUDE_DIRS && target->extra_include_dirs[ i ]; ++i )
            gen_list_append( buf, buf_size, &used, ';', target->extra_include_dirs[ i ] );
    }

    /* Solution-level include_dirs: skip if already present from the target. */
    if ( s_ctx.sln_extra_include_dirs )
    {
        for ( int i = 0; i < MAX_EXTRA_INCLUDE_DIRS && s_ctx.sln_extra_include_dirs[ i ]; ++i )
        {
            const char* dir = s_ctx.sln_extra_include_dirs[ i ];
            if ( buf[ 0 ] && strstr( buf, dir ) ) continue;
            gen_list_append( buf, buf_size, &used, ';', dir );
        }
    }

    /* Engine source root: auto-added for IntelliSense when 'engine' is declared. */
    if ( g_engine_root[ 0 ] )
    {
        char dirs[ 2 ][ PATH_MAX ];
        snprintf( dirs[ 0 ], PATH_MAX, "%s/source",       g_engine_root );
        snprintf( dirs[ 1 ], PATH_MAX, "%s/%s/generated", g_engine_root, BUILD_DIR );
        for ( int i = 0; i < 2; ++i )
        {
            if ( buf[ 0 ] && strstr( buf, dirs[ i ] ) ) continue;
            gen_list_append( buf, buf_size, &used, ';', dirs[ i ] );
        }
    }
}

/*==============================================================================================
    build_intellisense_defines()

    Build the semicolon-separated preprocessor-define value from the shared define tables
    in 02_data.c. Every vcxproj caller in both project formats uses this so none of them
    can diverge from what cc_fill_compile_cmd passes to cl.exe.
==============================================================================================*/

static void
build_intellisense_defines( char* buf, size_t buf_size, config_t config, target_info_t* target )
{
    buf[ 0 ]    = '\0';
    size_t used = 0;

    // Bind closure vars so call sites read identically to the old local macro.
    #define ISDEF_APPEND( s ) gen_list_append( buf, buf_size, &used, ';', s )

    for ( int i = 0; g_defines_always[ i ]; ++i ) ISDEF_APPEND( g_defines_always[ i ] );

    const char** cfg = ( config == CONFIG_DEBUG ) ? g_defines_debug : g_defines_release;
    for ( int i = 0; cfg[ i ]; ++i ) ISDEF_APPEND( cfg[ i ] );

    if ( target )
    {
        char upper[ 128 ];
        str_upper( target->name, upper, sizeof( upper ) );
        char define[ 160 ];
        snprintf( define, sizeof( define ), "%s_STATIC", upper );
        ISDEF_APPEND( define );

        for ( int i = 0; target->deps[ i ]; ++i )
        {
            target_info_t* dep = find_target( target->deps[ i ] );
            if ( !dep )
                continue;
            if ( dep->type == TARGET_STATIC_LIB || ( dep->type == TARGET_DYNAMIC_LIB && s_ctx.is_monolithic ) )
            {
                char dep_upper[ 128 ];
                str_upper( dep->name, dep_upper, sizeof( dep_upper ) );
                snprintf( define, sizeof( define ), "%s_STATIC", dep_upper );
                ISDEF_APPEND( define );
            }
        }
        if ( s_ctx.is_monolithic )
            ISDEF_APPEND( "BUILD_STATIC" );

        // Per-target defines from 'define' directives in orb.targets.
        for ( int i = 0; i < MAX_EXTRA_DEFINES && target->extra_defines[ i ]; ++i )
            ISDEF_APPEND( target->extra_defines[ i ] );
    }
    else
    {
        // Nav project: add _STATIC only for always-static libs (TARGET_STATIC_LIB).
        // These are PUBLIC in the CMake sense -- they propagate to all consumers.
        // PRIVATE module defines (RENDER_STATIC, AUDIO_STATIC, etc.) are excluded
        // because those are only for the module compiling itself (TARGET_DYNAMIC_LIB).
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( g_targets[ i ].type == TARGET_STATIC_LIB )
            {
                char upper[ 128 ];
                str_upper( g_targets[ i ].name, upper, sizeof( upper ) );
                char define[ 160 ];
                snprintf( define, sizeof( define ), "%s_STATIC", upper );
                ISDEF_APPEND( define );
            }
        }
    }

    #undef ISDEF_APPEND
}

/*==============================================================================================
    --- Makefile-Project IntelliSense Groups ---

    The per-config XML groups a Makefile-type project needs. Used by the NMake target
    writer and by the navigation project below -- the nav project is Makefile-type in
    both solutions because it builds nothing. The native MSBuild emitter does not use
    these: it drives IntelliSense from its real ClCompile settings instead.
==============================================================================================*/

/*==============================================================================================
    build_intellisense_nmake_options()

    Build the space-separated NMakeAdditionalOptions value from g_intellisense_flags[].
    Single source of truth: changing g_intellisense_flags[] in 02_data.c updates both
    the NMake PropertyGroup and any future toolchain consumers automatically.
==============================================================================================*/

static void
build_intellisense_nmake_options( char* buf, size_t buf_size )
{
    buf[ 0 ]    = '\0';
    size_t used = 0;
    for ( int i = 0; g_intellisense_flags[ i ]; ++i )
        gen_list_append( buf, buf_size, &used, ' ', g_intellisense_flags[ i ] );
}

/*==============================================================================================
    emit_intellisense_config_groups()

    Emit the four per-config XML groups shared by every Makefile project (target and nav):
      - Two ItemDefinitionGroup/ClCompile blocks (Debug + Release): LanguageStandard_C,
        UseStandardPreprocessor, AdditionalIncludeDirectories, PreprocessorDefinitions.
        The EDG IntelliSense front-end reads these -- NOT the NMake* PropertyGroup entries.
      - Two NMake PropertyGroup blocks (Debug + Release): NMakePreprocessorDefinitions,
        NMakeIncludeSearchPath, IntelliSenseMode, NMakeAdditionalOptions.

    target -- the engine target whose _STATIC define chain is used; NULL for nav projects
              (which use only the always-static-lib defines).
==============================================================================================*/

static void
emit_intellisense_config_groups( FILE* f, target_info_t* target )
{
    static const char* conds[ 2 ] =
    {
        "'$(Configuration)|$(Platform)'=='Debug|x64'",
        "'$(Configuration)|$(Platform)'=='Release|x64'",
    };

    char defines[ 2 ][ 1024 ];
    char nmake_opts[ 256 ];
    build_intellisense_defines( defines[ 0 ], sizeof( defines[ 0 ] ), CONFIG_DEBUG,   target );
    build_intellisense_defines( defines[ 1 ], sizeof( defines[ 1 ] ), CONFIG_RELEASE, target );
    build_intellisense_nmake_options( nmake_opts, sizeof( nmake_opts ) );

    char extra_incs[ 1024 ];
    build_extra_include_dirs_str( target, extra_incs, sizeof( extra_incs ) );
    const char* extra_sep = extra_incs[ 0 ] ? ";" : "";

    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <ItemDefinitionGroup Condition=\"%s\">\n", conds[ ci ] );
        fprintf( f, "    <ClCompile>\n" );
        fprintf( f, "      <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
        if ( g_gen_fwd_compat )
            fprintf( f, "      <LanguageStandard>stdcpp20</LanguageStandard>\n" );
        fprintf( f, "      <UseStandardPreprocessor>true</UseStandardPreprocessor>\n" );
        fprintf( f, "      <AdditionalIncludeDirectories>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n",
                 s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
        fprintf( f, "      <PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>\n",
                 defines[ ci ] );
        fprintf( f, "    </ClCompile>\n" );
        fprintf( f, "  </ItemDefinitionGroup>\n" );
    }

    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <PropertyGroup Condition=\"%s\">\n", conds[ ci ] );
        fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
                 defines[ ci ] );
        fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;$(VC_IncludePath);$(WindowsSDK_IncludePath);$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n",
                 s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
        fprintf( f, "    <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
        if ( g_gen_fwd_compat )
            fprintf( f, "    <LanguageStandard>stdcpp20</LanguageStandard>\n" );
        fprintf( f, "    <IntelliSenseMode>windows-msvc-x64</IntelliSenseMode>\n" );
        fprintf( f, "    <NMakeAdditionalOptions>%s</NMakeAdditionalOptions>\n", nmake_opts );
        fprintf( f, "  </PropertyGroup>\n" );
    }
}

/*==============================================================================================
    --- Shared Project Fragments ---

    XML both project formats emit identically: the debugger property groups, the natvis
    item group, and the whole .vcxproj.filters file.
==============================================================================================*/

/*==============================================================================================
    gen_run_debug_cmd()

    Resolve a target's 'run' line ("<exe_target> [args...]") into the F5 debugger command.
    Imported engine exe targets resolve to the engine bin (absolute); local targets to the
    solution-relative bin. Args pass through verbatim -- they resolve at debug time against
    LocalDebuggerWorkingDirectory (the project root), so 'run host_editor -project .' works.
    Returns false (with a warning) when the exe target is unknown.
==============================================================================================*/

static bool
gen_run_debug_cmd( const target_info_t* target, char* cmd, size_t cmd_size,
                   char* args, size_t args_size )
{
    // Split "<exe> [args...]".
    char        exe[ 128 ];
    const char* p = target->run_cmd;
    size_t      n = 0;
    while ( p[ n ] && p[ n ] != ' ' && p[ n ] != '\t' && n < sizeof( exe ) - 1 )
    {
        exe[ n ] = p[ n ];
        ++n;
    }
    exe[ n ] = '\0';
    p += n;
    while ( *p == ' ' || *p == '\t' ) ++p;
    snprintf( args, args_size, "%s", p );

    target_info_t* run_t = find_target( exe );
    if ( !run_t )
    {
        printf( ORB_INDENT "[orb warn] target '%s': run references unknown target '%s'"
                           " -- no F5 command emitted\n", target->name, exe );
        return false;
    }

    if ( run_t->is_external && g_engine_root[ 0 ] )
        snprintf( cmd, cmd_size, "%s\\bin\\%s.exe", g_engine_root, exe );
    else
        snprintf( cmd, cmd_size, "$(ProjectDir)%sbin\\%s.exe", s_ctx.root_prefix, exe );
    return true;
}

/*==============================================================================================
    gen_emit_debug_property_groups()

    Per-config debugger PropertyGroups shared by every project writer (NMake target,
    MSBuild target, MSBuild alias). Emits LocalDebuggerWorkingDirectory (project root)
    plus LocalDebuggerCommand/Arguments when the target declares a 'run' line.
    with_outdirs adds OutDir/IntDir into the same groups (the native MSBuild writers
    fold them here; the NMake writer emits them in its unconditional group instead).

    These are DEFAULTS: per-user tweaks made in the VS debug property page land in
    .vcxproj.user, which imports later and wins -- regen never stomps user overrides.
==============================================================================================*/

static void
gen_emit_debug_property_groups( FILE* f, target_info_t* target, bool with_outdirs )
{
    char run_exe [ PATH_MAX + 64 ] = { 0 };
    char run_args[ 512 ]           = { 0 };
    bool has_run = target && target->run_cmd &&
                   gen_run_debug_cmd( target, run_exe, sizeof( run_exe ), run_args, sizeof( run_args ) );

    static const char* cfgs[ 2 ] = { "Debug", "Release" };
    for ( int ci = 0; ci < 2; ++ci )
    {
        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='%s|x64'\">\n", cfgs[ ci ] );
        if ( with_outdirs )
        {
            fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_ctx.root_prefix );
            fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
                     s_ctx.root_prefix, g_build_dir, g_int_dir );
        }
        fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_ctx.cd_root );
        if ( has_run )
        {
            fprintf( f, "    <LocalDebuggerCommand>%s</LocalDebuggerCommand>\n", run_exe );
            if ( run_args[ 0 ] )
                fprintf( f, "    <LocalDebuggerCommandArguments>%s</LocalDebuggerCommandArguments>\n", run_args );
        }
        fprintf( f, "  </PropertyGroup>\n" );
    }
}

/*==============================================================================================
    write_natvis_item_group()

    Emits one <ItemGroup> containing a <Natvis Include="..."> entry for every .natvis
    file collected by scan_directory_recursive(). No-op if g_files[] has none.
    Shared by both vcxproj generators (filters omits this: it handles natvis separately
    because it also needs <Filter> children).
==============================================================================================*/

static bool
files_have_natvis( void )
{
    for ( int i = 0; i < g_file_count; ++i )
        if ( g_files[ i ].is_natvis ) return true;
    return false;
}

static void
write_natvis_item_group( FILE* f )
{
    if ( !files_have_natvis() )
        return;

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        if ( !g_files[ i ].is_natvis )
            continue;
        char inc[ PATH_MAX + 32 ];
        gen_inc_path( g_files[ i ].path, inc, sizeof( inc ) );
        fprintf( f, "    <Natvis Include=\"%s\" />\n", inc );
    }
    fprintf( f, "  </ItemGroup>\n" );
}

/*==============================================================================================
    write_vcxproj_filters_file()

    Writes one .vcxproj.filters file. Shared by the NMake target, the nav project, and
    the MSBuild target generators -- all three produce identical filters content.

    target == NULL means a navigation project: every file is <ClInclude> and no
    generated items are appended. Otherwise is_unit_file() selects the tag per entry,
    reflect items are appended when target->has_reflect is set, and the resource table
    when the target carries one.

    Caller must populate g_files[]/g_filters[] via scan_directory_recursive() first.
==============================================================================================*/

static void
write_vcxproj_filters_file( const char* filters_path, target_info_t* target )
{
    FILE* f = fopen( filters_path, "w" );
    if ( !f ) return;

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_filter_count; ++i )
    {
        fprintf( f, "    <Filter Include=\"%s\">\n", g_filters[ i ] );
        fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n",
                 ( unsigned int )i );
        fprintf( f, "    </Filter>\n" );
    }
    if ( target && target->has_reflect )
    {
        fprintf( f, "    <Filter Include=\"generated\">\n" );
        fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n",
                 ( unsigned int )g_filter_count );
        fprintf( f, "    </Filter>\n" );
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        if ( g_files[ i ].is_natvis )
            continue;    // emitted in a separate Natvis ItemGroup below
        bool        is_unit = target && is_unit_file( target, g_files[ i ].path );
        const char* tag     = is_unit ? "ClCompile" : "ClInclude";
        char        inc[ PATH_MAX + 32 ];
        gen_inc_path( g_files[ i ].path, inc, sizeof( inc ) );
        fprintf( f, "    <%s Include=\"%s\">\n", tag, inc );
        if ( g_files[ i ].filter[ 0 ] != '\0' )
            fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
        fprintf( f, "    </%s>\n", tag );
    }
    if ( target && target->has_reflect )
    {
        const char* rname = target_reflect_name( target );
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\">\n",
                 s_ctx.root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "      <Filter>generated</Filter>\n" );
        fprintf( f, "    </ClCompile>\n" );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\">\n",
                 s_ctx.root_prefix, g_build_dir, g_gen_dir, rname );
        fprintf( f, "      <Filter>generated</Filter>\n" );
        fprintf( f, "    </ClInclude>\n" );
    }
    fprintf( f, "  </ItemGroup>\n" );

    // Natvis visualizer files: separate ItemGroup so VS loads them as debugger visualizers.
    if ( files_have_natvis() )
    {
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            if ( !g_files[ i ].is_natvis )
                continue;
            char inc[ PATH_MAX + 32 ];
            gen_inc_path( g_files[ i ].path, inc, sizeof( inc ) );
            fprintf( f, "    <Natvis Include=\"%s\">\n", inc );
            if ( g_files[ i ].filter[ 0 ] != '\0' )
                fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
            fprintf( f, "    </Natvis>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
    }

    fprintf( f, "</Project>\n" );
    fclose( f );
}

/*==============================================================================================
    gen_proj_engine_navigation()

    Emit the "Mega" navigation .vcxproj for a solution. Scans nav_dir recursively
    and lists EVERY .c/.h as <ClInclude> (never ClCompile) so the developer gets a
    unified Solution Explorer view, but the nav project never competes with a target
    .vcxproj for any file's IntelliSense TU context.

    Both solution flavors emit this same project: it builds nothing, so there is no
    Makefile-vs-MSBuild distinction for it to make. build_gen_solution() calls it.
==============================================================================================*/

static void
gen_proj_engine_navigation( const char* sln_name, const char* nav_dir, const char* nav_guid )
{
    gen_scan_reset();
    scan_directory_recursive( nav_dir, nav_dir );

    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s_nav.vcxproj", s_ctx.out_dir, sln_name );
    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );
    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", nav_guid );
    fprintf( f, "    <Platform Condition=\"'$(Platform)'==''\">x64</Platform>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    { char ts[ 32 ]; fprintf( f, "    <PlatformToolset>%s</PlatformToolset>\n", gen_platform_toolset( ts, sizeof( ts ) ) ); }
    // LanguageStandard_C and IntelliSenseMode are emitted per-config below.
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Nav build/clean are deliberate no-ops: per-target .vcxproj projects own their
    // own build and clean. Running a global build_tool.exe here in parallel with them
    // would race on shared dirs (e.g. build\\generated\\*).
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_ctx.root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
             s_ctx.root_prefix, g_build_dir, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>echo       [nav] navigation-only project, nothing to build.</NMakeBuildCommandLine>\n" );
    fprintf( f, "    <NMakeOutput>$(ProjectDir)%s%s\\$(ProjectName)\\$(Configuration)\\nav.stamp</NMakeOutput>\n",
             s_ctx.root_prefix, g_int_dir );
    fprintf( f, "    <NMakeCleanCommandLine>echo       [nav] navigation-only project, nothing to clean.</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakeCompileFile>echo       [nav] navigation-only project.</NMakeCompileFile>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    emit_intellisense_config_groups( f, NULL );

    // All files as ClInclude regardless of extension. Listing .c files as ClCompile
    // would create a competing TU context; VS picks last-loaded-wins per file, so
    // headers would resolve under this empty context instead of the real target's
    // context (wrong defines, wrong API visible). ClInclude has no TU semantics.
    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
        fprintf( f, "    <ClInclude Include=\"%s%s\" />\n", s_ctx.root_prefix, g_files[ i ].path );
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s_nav.vcxproj.filters", s_ctx.out_dir, sln_name );
    write_vcxproj_filters_file( filters_path, NULL );
}

/*==============================================================================================
    --- Solution Writer ---

    The .sln file. Its format carries project GUIDs, dependency edges, config mappings and
    virtual folders -- none of which depend on whether the referenced .vcxproj files are
    Makefile-type or native MSBuild, so both flavors write the same .sln from here.
==============================================================================================*/

/*  Virtual VS folders per solution. Each '/' segment of a target's virtual_folder
    registers separately, so "03_RUNTIME/SERVICE" costs two entries. */

#define MAX_SLN_FOLDERS 64

/* True when the named target is one of the solution's own projects.  Solution-level
   ProjectDependencies may only reference projects present in the .sln -- VS ignores a
   dangling GUID, but msbuild.exe fails the whole solution with MSB4051. */
static bool
sln_has_target( const solution_info_t* sln, const char* name )
{
    for ( const char* const* tn = sln->target_names; *tn; ++tn )
        if ( strcmp( *tn, name ) == 0 )
            return true;
    return false;
}

/* Writes one ProjectDependencies entry for `name`, or nothing when the solution has no such
   project -- the MSB4051 guard above, applied at every site that emits a dep GUID. A NULL
   name is the "no such tool is registered" case and is likewise nothing to emit. */
static void
sln_emit_dep_guid( FILE* f, const solution_info_t* sln, const char* name )
{
    if ( !name || !sln_has_target( sln, name ) )
        return;
    char dep_guid[ GUID_STR_MAX ];
    guid_from_name( name, dep_guid, sizeof( dep_guid ) );
    fprintf( f, "\t\t%s = %s\n", dep_guid, dep_guid );
}

/* The virtual folders of one solution, in registration order. Parallel arrays rather
   than an array of structs so a folder path stays a plain char[PATH_MAX] for strcmp.
   sln_name lives here because every GUID is keyed on it. */

typedef struct
{
    char        folders[ MAX_SLN_FOLDERS ][ PATH_MAX ];     // full '/'-joined path, e.g. "03_RUNTIME/SERVICE"
    char        guids  [ MAX_SLN_FOLDERS ][ GUID_STR_MAX ]; // GUID hashed from sln_name + folder path
    int         count;                                      // valid entries in both arrays
    const char* sln_name;                                   // owning solution; part of every GUID key
    bool        full_warned;                                // MAX_SLN_FOLDERS warning already issued

} sln_folders_t;

/* Find-or-insert one folder path. Keying the GUID on the solution name gives the same
   folder path distinct GUIDs in different solutions. Overflow warns and drops the
   folder: its targets then appear at the solution root rather than nested. */

static void
sln_folder_intern( const char* path, void* ctx )
{
    sln_folders_t* fl = ( sln_folders_t* )ctx;

    if ( path[ 0 ] == '\0' )
        return;    // A target with no virtual_folder; nothing to nest it under.

    for ( int i = 0; i < fl->count; ++i )
        if ( strcmp( fl->folders[ i ], path ) == 0 )
            return;

    if ( fl->count >= MAX_SLN_FOLDERS )
    {
        // Once per solution: every target in an overflowing folder re-offers it.
        if ( !fl->full_warned )
        {
            printf( ORB_INDENT "[orb warn] solution '%s' folder table full (MAX_SLN_FOLDERS=%d);"
                    " '%s' and any further folders will not be nested\n",
                    fl->sln_name, MAX_SLN_FOLDERS, path );
            fl->full_warned = true;
        }
        return;
    }

    snprintf( fl->folders[ fl->count ], PATH_MAX, "%s", path );

    char key[ 192 ];
    snprintf( key, sizeof( key ), "folder:%s:%s", fl->sln_name, path );
    guid_from_name( key, fl->guids[ fl->count ], GUID_STR_MAX );
    fl->count++;
}

/*==============================================================================================
    build_gen_solution()

    Write the .sln descriptor for one entry of g_solutions[]. Pipeline:
      1. Generate the nav .vcxproj file (if nav_dir is set) -- deferred .sln entry.
      2. For each target: emit Project entry + ProjectDependencies cross-references.
      3. Emit nav Project entry last (so targets win first-project IntelliSense priority).
      4. Emit virtual SLN folder entries.
      5. Emit GlobalSection blocks: configuration mapping + NestedProjects.
==============================================================================================*/

static void
build_gen_solution( solution_info_t* sln, const char* out_name )
{
    char sln_path[ PATH_MAX ];
    snprintf( sln_path, sizeof( sln_path ), "%s\\%s.sln", s_ctx.out_dir, out_name );
    FILE* f = fopen( sln_path, "w" );
    if ( !f )
        return;

    fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
    fprintf( f, "# Visual Studio Version %d\n", build_detect_vs_major() );

    // These GUIDs are FIXED by Visual Studio -- do not regenerate.
    const char* folder_type_guid = "{2150E333-8FDC-42A3-9474-1A3956D46DE8}";
    const char* cpp_type_guid    = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

    char nav_guid[ GUID_STR_MAX ] = { 0 };
    {
        char key[ 128 ];
        snprintf( key, sizeof( key ), "nav:%s", out_name );
        guid_from_name( key, nav_guid, sizeof( nav_guid ) );
    }

    // 1. Generate the nav .vcxproj file now so the file exists when VS opens the .sln,
    //    but defer writing the nav Project() entry until after all target entries so that
    //    VS gives target projects first-project priority for IntelliSense ownership.
    if ( sln->nav_dir )
        gen_proj_engine_navigation( out_name, sln->nav_dir, nav_guid );

    // 2. Target projects.
    // VS uses file order (not ExtensibilityGlobals) to pick the default startup project
    // on first open, so emit the declared startup target before all others.
    sln_folders_t folders = { .sln_name = out_name };

    target_info_t* startup_first =
        sln->startup_project ? find_target( sln->startup_project ) : NULL;

    // 2a. Project block emission: two-pass so startup target appears first in the file.
    for ( int emit_startup = 1; emit_startup >= 0; --emit_startup )
    {
        for ( const char* const* tn = sln->target_names; *tn; ++tn )
        {
            target_info_t* target = find_target( *tn );
            if ( !target ) continue;
            bool is_startup = ( target == startup_first );
            if (  emit_startup && !is_startup ) continue;
            if ( !emit_startup &&  is_startup ) continue;

            char guid[ GUID_STR_MAX ];
            guid_from_name( target->name, guid, sizeof( guid ) );
            fprintf( f, "Project(\"%s\") = \"%s\", \"%s.vcxproj\", \"%s\"\n", cpp_type_guid, target->name,
                     target->name, guid );

            // Every target but build_tool lists build_tool itself below, so only build_tool
            // can have an empty section to skip.
            if ( !target->is_build_tool || target->deps[ 0 ] || target->tool_deps[ 0 ] || target->has_reflect )
            {
                // Every entry below must name a project actually in this solution: VS silently
                // ignores dangling dep GUIDs, but msbuild.exe on the .sln hard-fails with
                // MSB4051 (child-project solutions don't carry reflect_tool, for example --
                // there the reflect pre-build event / build_tool owns that ordering instead).
                fprintf( f, "\tProjectSection(ProjectDependencies) = postProject\n" );

                // Alias launchers depend on the target they build so VS builds it (with
                // its full dep chain) before the alias's fast skip-rebuild runs.
                sln_emit_dep_guid( f, sln, target->alias_for );

                for ( int i = 0; target->deps[ i ]; ++i )
                    sln_emit_dep_guid( f, sln, target->deps[ i ] );

                for ( int i = 0; target->tool_deps[ i ]; ++i )
                    sln_emit_dep_guid( f, sln, target->tool_deps[ i ] );

                // Implicit deps on the build's own tools. Each is resolved through its
                // find_*_tool(), so the GUID and the membership test name the same target:
                // taking one from a flag scan and the other from a string literal is how a
                // dangling GUID gets emitted.
                const target_info_t* bt = target->is_build_tool ? NULL : find_build_tool();
                sln_emit_dep_guid( f, sln, bt ? bt->name : NULL );

                if ( target->has_reflect )
                {
                    const target_info_t* rt = find_reflect_tool();
                    sln_emit_dep_guid( f, sln, rt ? rt->name : NULL );
                }

                if ( target_wants_res_manifest( target ) )
                {
                    const target_info_t* rt = find_res_tool();
                    sln_emit_dep_guid( f, sln, rt ? rt->name : NULL );
                }

                fprintf( f, "\tEndProjectSection\n" );
            }

            fprintf( f, "EndProject\n" );
        }
    }

    // 2b. Collect SLN folders for nesting (order-independent; covers all targets).
    // Register every path segment so "A/B" creates both "A" and "A/B" folders.
    for ( const char* const* tn = sln->target_names; *tn; ++tn )
    {
        target_info_t* target = find_target( *tn );
        if ( !target ) continue;

        char tmp[ PATH_MAX ];
        snprintf( tmp, sizeof( tmp ), "%s", target->virtual_folder );
        path_to_fwd( tmp );

        path_walk_prefixes( tmp, '/', sln_folder_intern, &folders );
    }

    // 3. Navigation project entry (listed last so target projects get first-project
    //    priority for IntelliSense ownership when VS opens a file).
    if ( sln->nav_dir )
    {
        fprintf( f, "Project(\"%s\") = \"%s_nav\", \"%s_nav.vcxproj\", \"%s\"\n", cpp_type_guid, out_name,
                 out_name, nav_guid );
        fprintf( f, "EndProject\n" );
    }

    // 4. Virtual SLN folders.
    // Display name is the leaf segment only; nesting is expressed via NestedProjects below.
    for ( int i = 0; i < folders.count; ++i )
    {
        const char* leaf = strrchr( folders.folders[ i ], '/' );
        const char* display = leaf ? leaf + 1 : folders.folders[ i ];
        fprintf( f, "Project(\"%s\") = \"%s\", \"%s\", \"%s\"\n", folder_type_guid, display, display,
                 folders.guids[ i ] );
        fprintf( f, "EndProject\n" );
    }

    // 5. Global sections.
    fprintf( f, "Global\n" );
    fprintf( f, "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n" );
    fprintf( f, "\t\tDebug|x64 = Debug|x64\n" );
    fprintf( f, "\t\tRelease|x64 = Release|x64\n" );
    fprintf( f, "\tEndGlobalSection\n" );

    fprintf( f, "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n" );
    for ( const char* const* tn = sln->target_names; *tn; ++tn )
    {
        target_info_t* t = find_target( *tn );
        if ( t )
        {
            char guid[ GUID_STR_MAX ];
            guid_from_name( t->name, guid, sizeof( guid ) );
            fprintf( f, "\t\t%s.Debug|x64.ActiveCfg = Debug|x64\n", guid );
            fprintf( f, "\t\t%s.Debug|x64.Build.0 = Debug|x64\n", guid );
            fprintf( f, "\t\t%s.Release|x64.ActiveCfg = Release|x64\n", guid );
            fprintf( f, "\t\t%s.Release|x64.Build.0 = Release|x64\n", guid );
        }
    }
    // Nav project config listed last, consistent with its Project() entry ordering.
    if ( sln->nav_dir )
    {
        fprintf( f, "\t\t%s.Debug|x64.ActiveCfg = Debug|x64\n", nav_guid );
        fprintf( f, "\t\t%s.Debug|x64.Build.0 = Debug|x64\n", nav_guid );
        fprintf( f, "\t\t%s.Release|x64.ActiveCfg = Release|x64\n", nav_guid );
        fprintf( f, "\t\t%s.Release|x64.Build.0 = Release|x64\n", nav_guid );
    }
    fprintf( f, "\tEndGlobalSection\n" );

    fprintf( f, "\tGlobalSection(NestedProjects) = preSolution\n" );

    // Map each project to its leaf folder.
    for ( const char* const* tn = sln->target_names; *tn; ++tn )
    {
        target_info_t* t = find_target( *tn );
        if ( t )
        {
            // Normalize virtual_folder to forward slashes for comparison.
            char norm[ PATH_MAX ];
            snprintf( norm, sizeof( norm ), "%s", t->virtual_folder );
            path_to_fwd( norm );

            char proj_guid[ GUID_STR_MAX ];
            guid_from_name( t->name, proj_guid, sizeof( proj_guid ) );
            for ( int j = 0; j < folders.count; ++j )
            {
                if ( strcmp( folders.folders[ j ], norm ) == 0 )
                {
                    fprintf( f, "\t\t%s = %s\n", proj_guid, folders.guids[ j ] );
                    break;
                }
            }
        }
    }

    // Map each non-root folder to its parent folder.
    for ( int i = 0; i < folders.count; ++i )
    {
        const char* slash = strrchr( folders.folders[ i ], '/' );
        if ( !slash )
            continue;
        char parent[ PATH_MAX ];
        int  parent_len = ( int )( slash - folders.folders[ i ] );
        strncpy( parent, folders.folders[ i ], parent_len );
        parent[ parent_len ] = '\0';
        for ( int j = 0; j < folders.count; ++j )
        {
            if ( strcmp( folders.folders[ j ], parent ) == 0 )
            {
                fprintf( f, "\t\t%s = %s\n", folders.guids[ i ], folders.guids[ j ] );
                break;
            }
        }
    }

    fprintf( f, "\tEndGlobalSection\n" );

    // ExtensibilityGlobals: startup project (optional).
    if ( sln->startup_project )
    {
        target_info_t* startup_t = find_target( sln->startup_project );
        if ( startup_t )
        {
            char sln_guid[ GUID_STR_MAX ];
            guid_from_name( out_name, sln_guid, sizeof( sln_guid ) );
            fprintf( f, "\tGlobalSection(ExtensibilityGlobals) = postSolution\n" );
            fprintf( f, "\t\tSolutionGuid = %s\n", sln_guid );
            fprintf( f, "\t\tStartupProject = %s\n", startup_t->name );
            fprintf( f, "\tEndGlobalSection\n" );
        }
    }

    fprintf( f, "EndGlobal\n" );
    fclose( f );
}

/*==============================================================================================
    run_solution_passes()

    Shared iteration loop for both project formats. For each local solution, sets s_ctx
    state, computes path parts, ensures the output dir, and calls per_target_fn per target
    then build_gen_solution. The format-specific half is entirely in per_target_fn.

    name_suffix -- appended to sln->name for the .sln filename (e.g. "_nm", "_ms")
    dir_suffix  -- appended to sln->out_dir for the output directory ("" or "_ms")
    label       -- printed in the progress line ("Solution" or "MSBuild Solution")
==============================================================================================*/

typedef void ( *per_target_fn_t )( target_info_t* );

static void
run_solution_passes( const gen_manifest_t* m, const char* name_suffix,
                     const char* dir_suffix, const char* label,
                     per_target_fn_t per_target_fn )
{
    for ( int i = 0; i < m->solution_count; ++i )
    {
        const gen_sln_entry_t* entry = &m->solutions[ i ];
        solution_info_t*       sln   = entry->sln;

        s_ctx.is_monolithic          = sln->is_monolithic;
        s_ctx.sln_extra_include_dirs = sln->extra_include_dirs;

        char out_dir[ PATH_MAX ];
        snprintf( out_dir, sizeof( out_dir ), "%s%s", sln->out_dir, dir_suffix );

        char sln_name[ 256 ];
        snprintf( sln_name, sizeof( sln_name ), "%s%s", sln->name, name_suffix );

        compute_path_parts( out_dir );
        ensure_dir( out_dir );

        printf( "Generating %s '%s' in %s/...\n", label, sln_name, out_dir );

        for ( int j = 0; j < entry->target_count; ++j )
            per_target_fn( entry->targets[ j ] );

        build_gen_solution( sln, sln_name );
    }
}

/*============================================================================================*/
