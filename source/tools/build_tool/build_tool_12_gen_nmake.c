/*==============================================================================================

    build_tool_12_gen_nmake.c -- Visual Studio .sln / .vcxproj generator.

    The build system "hijacks" Visual Studio: instead of letting MSBuild own
    the build, each .vcxproj is emitted as a Makefile-style project whose
    NMakeBuildCommandLine shells out to bin\build_tool.exe. The IDE provides
    IntelliSense, the debugger, and source navigation; we keep absolute
    control over the actual compile/link pipeline.

    What gets emitted, per solution in g_solutions[]:

      <name>.sln                           -- references all member projects
      <name>_nav.vcxproj  (if nav_dir set) -- "Mega" navigation project,
                                              every file as ClInclude so it
                                              never competes with a target
                                              for IntelliSense ownership
      <target>.vcxproj                     -- one per target referenced by sln
      <target>.vcxproj.filters             -- mirrors the on-disk folder tree

    GUID strategy (see guid_from_name): every project GUID is a hash of its
    name, so reordering g_targets[] is a no-op for VS user state.

    IntelliSense strategy (see write_vcxproj_common_header): per-config
    PropertyGroups emit /std:c11 /Zc:preprocessor + the same defines cl.exe
    sees at build time, so squigglies match build errors.

==============================================================================================*/

#define MAX_FILES   1024    // Max source files scanned per vcxproj.
#define MAX_FILTERS 512     // Max virtual filter folders per vcxproj.

/*==============================================================================================
    --- Per-Solution Path State ---

    Set once at the top of each solution's generation pass in build_gen_projects().
    All emitter functions read these instead of hardcoding depth relative paths.
    Moving a solution's output directory only requires changing out_dir in the
    solution descriptor -- no string edits anywhere else in this file.

    TLDR: we calculate the depth and escape string manually before generating.

    s_out_dir     -- where .sln and .vcxproj files land     (e.g. "build\\proj")
    s_root_prefix -- "../..\\" go back to the project root  (e.g. "..\\..\\")
    s_cd_root     -- "cd ..\\.." for NMake commands         (e.g. "..\\..")
==============================================================================================*/

/*==============================================================================================
    gen_ctx_t -- Per-solution generation context.

    Replaces seven separate file-statics so the state is clearly grouped.
    compute_path_parts() fills it once per solution pass; every emitter reads from s_ctx.
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
    s_ctx.cwd_prefix[ 0 ] = '\0';
#if defined( _WIN32 )
    char cwd[ PATH_MAX ];
    if ( GetCurrentDirectoryA( sizeof( cwd ), cwd ) )
    {
        for ( char* p = cwd; *p; ++p ) if ( *p == '/' ) *p = '\\';
        size_t n = strlen( cwd );
        if ( n > 0 && cwd[ n - 1 ] != '\\' ) { cwd[ n++ ] = '\\'; cwd[ n ] = '\0'; }
        snprintf( s_ctx.cwd_prefix, sizeof( s_ctx.cwd_prefix ), "%s", cwd );
    }
#endif
}

// Build the Include path for a scanned file entry.
// CWD-relative paths get s_root_prefix; absolute cross-project paths emit as-is.
static void
gen_inc_path( const char* file_path, char* buf, size_t buf_size )
{
    if ( platform_is_abs_path( file_path ) )
        snprintf( buf, buf_size, "%s", file_path );
    else
        snprintf( buf, buf_size, "%s%s", s_ctx.root_prefix, file_path );
}

// Metadata for one source file picked up by scan_directory_recursive().
typedef struct
{
    char path[ PATH_MAX ];      // Relative path from project root.
    char filter[ PATH_MAX ];    // Virtual folder path in VS (e.g. "engine\\core").
    bool is_header;             // True for .h files.

} file_info_t;

// Per-scan buffers. Reset (g_file_count = 0, g_filter_count = 0) at the
// start of every project emission -- reusable scratch, not persistent state.

static file_info_t g_files[ MAX_FILES ];
static int         g_file_count = 0;

static char        g_filters[ MAX_FILTERS ][ PATH_MAX ];
static int         g_filter_count = 0;

/*==============================================================================================
    guid_from_name()

    Produces a deterministic 128-bit GUID from a string in standard 8-4-4-4-12 form.
    The previous index-based scheme (GUID derived from position in g_targets[]) meant
    inserting or reordering a target shifted every subsequent GUID, invalidating VS user
    state (.suo files, breakpoints, ProjectDependencies). Hashing the name means the GUID
    is stable for the life of the name.

    Two FNV-1a passes with different (seed, prime) pairs fill the 16 bytes.
==============================================================================================*/

static void
guid_from_name( const char* name, char* out )
{
    // FNV-1a 64-bit variant
    unsigned long long h1 = 0xcbf29ce484222325ULL;
    unsigned long long h2 = 0x9ae16a3b2f90404fULL;
    for ( const unsigned char* p = ( const unsigned char* )name; *p; ++p )
    {
        h1 = ( h1 ^ *p ) * 0x100000001b3ULL;
        h2 = ( h2 ^ *p ) * 0x880355f21e6d1965ULL;
    }

    snprintf( out, 64, "{%08X-%04X-%04X-%04X-%04X%08X}", ( unsigned int )( h1 >> 32 ),
              ( unsigned int )( ( h1 >> 16 ) & 0xFFFFu ), ( unsigned int )( h1 & 0xFFFFu ),
              ( unsigned int )( h2 >> 48 ), ( unsigned int )( ( h2 >> 32 ) & 0xFFFFu ),
              ( unsigned int )( h2 & 0xFFFFFFFFu ) );
}

// Register a virtual VS folder. Idempotent -- no-op if empty or already present.
static void
add_filter( const char* filter )
{
    if ( filter[ 0 ] == '\0' )
        return;
    for ( int i = 0; i < g_filter_count; ++i )
    {
        if ( platform_stricmp( g_filters[ i ], filter ) == 0 )
            return;
    }
    if ( g_filter_count < MAX_FILTERS )
        strcpy( g_filters[ g_filter_count++ ], filter );
}

// Register all parent folders of a filter path. e.g. "engine\\core\\win"
// -> adds "engine", "engine\\core", "engine\\core\\win". Necessary for VS to
// display nested folder structure correctly.
static void
add_filters_recursive( const char* filter )
{
    char  tmp[ PATH_MAX ];
    char* p = tmp;
    snprintf( tmp, sizeof( tmp ), "%s", filter );

    while ( *p )
    {
        if ( *p == '\\' )
        {
            *p = '\0';
            add_filter( tmp );
            *p = '\\';
        }
        p++;
    }
    add_filter( tmp );    // Register the leaf segment.
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

            bool is_c = platform_stricmp( ext, ".c" ) == 0;
            bool is_h = platform_stricmp( ext, ".h" ) == 0;
            if ( !( is_c || is_h ) )
                continue;
            if ( g_file_count >= MAX_FILES )
                continue;

            file_info_t* f = &g_files[ g_file_count++ ];

            // Strip CWD prefix from absolute paths (produced when root_dir is absolute)
            // so stored paths remain project-root-relative for vcxproj Include emit.
            // Cross-project absolute paths (outside CWD) are kept as-is.
            size_t cwd_len = strlen( s_ctx.cwd_prefix );
            if ( cwd_len > 0 && platform_strnicmp( path, s_ctx.cwd_prefix, cwd_len ) == 0 )
                strcpy( f->path, path + cwd_len );
            else
                strcpy( f->path, path );

            f->is_header = is_h;
            get_filter_for_path( path, root_dir, f->filter );
            if ( f->filter[ 0 ] != '\0' )
                add_filters_recursive( f->filter );
        }
    }
    while ( platform_find_next( handle, &find_data ) );

    platform_find_close( handle );
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
        {
            const char* dir = target->extra_include_dirs[ i ];
            size_t len = strlen( dir );
            if ( used + len + 2 < buf_size )
            {
                if ( used ) buf[ used++ ] = ';';
                memcpy( buf + used, dir, len );
                used += len;
                buf[ used ] = '\0';
            }
        }
    }

    /* Solution-level include_dirs: skip if already present from the target. */
    if ( s_ctx.sln_extra_include_dirs )
    {
        for ( int i = 0; i < MAX_EXTRA_INCLUDE_DIRS && s_ctx.sln_extra_include_dirs[ i ]; ++i )
        {
            const char* dir = s_ctx.sln_extra_include_dirs[ i ];
            if ( buf[ 0 ] && strstr( buf, dir ) ) continue;
            size_t len = strlen( dir );
            if ( used + len + 2 < buf_size )
            {
                if ( used ) buf[ used++ ] = ';';
                memcpy( buf + used, dir, len );
                used += len;
                buf[ used ] = '\0';
            }
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
            size_t len = strlen( dirs[ i ] );
            if ( used + len + 2 < buf_size )
            {
                if ( used ) buf[ used++ ] = ';';
                memcpy( buf + used, dirs[ i ], len );
                used += len;
                buf[ used ] = '\0';
            }
        }
    }
}

/*==============================================================================================
    build_intellisense_defines()

    Build the semicolon-separated NMakePreprocessorDefinitions value from the
    shared define tables in 02_data.c. Both the per-target and the nav project
    vcxproj callers use this so neither can diverge from what cc_fill_compile_cmd
    passes to cl.exe.
==============================================================================================*/

static inline void
isdef_append( char* buf, size_t buf_size, size_t* used, const char* s )
{
    size_t slen = strlen( s );
    if ( *used + slen + 2 < buf_size )
    {
        if ( *used )
            buf[ ( *used )++ ] = ';';
        memcpy( buf + *used, s, slen );
        *used += slen;
        buf[ *used ] = '\0';
    }
}

static void
build_intellisense_defines( char* buf, size_t buf_size, config_t config, target_info_t* target )
{
    buf[ 0 ]    = '\0';
    size_t used = 0;

    // Bind closure vars so call sites read identically to the old local macro.
    #define ISDEF_APPEND( s ) isdef_append( buf, buf_size, &used, s )

    for ( int i = 0; g_defines_always[ i ]; ++i ) ISDEF_APPEND( g_defines_always[ i ] );

    const char** cfg = ( config == CONFIG_DEBUG ) ? g_defines_debug : g_defines_release;
    for ( int i = 0; cfg[ i ]; ++i ) ISDEF_APPEND( cfg[ i ] );

    if ( target )
    {
        char upper[ 128 ];
        get_target_upper( target->name, upper, sizeof( upper ) );
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
                get_target_upper( dep->name, dep_upper, sizeof( dep_upper ) );
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
                get_target_upper( g_targets[ i ].name, upper, sizeof( upper ) );
                char define[ 160 ];
                snprintf( define, sizeof( define ), "%s_STATIC", upper );
                ISDEF_APPEND( define );
            }
        }
    }

    #undef ISDEF_APPEND
}

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
    {
        size_t slen = strlen( g_intellisense_flags[ i ] );
        if ( used + slen + 2 < buf_size )
        {
            if ( used ) buf[ used++ ] = ' ';
            memcpy( buf + used, g_intellisense_flags[ i ], slen );
            used += slen;
            buf[ used ] = '\0';
        }
    }
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
    char dbg_defines[ 1024 ];
    char rel_defines[ 1024 ];
    char nmake_opts[ 256 ];
    build_intellisense_defines( dbg_defines, sizeof( dbg_defines ), CONFIG_DEBUG,   target );
    build_intellisense_defines( rel_defines, sizeof( rel_defines ), CONFIG_RELEASE, target );
    build_intellisense_nmake_options( nmake_opts, sizeof( nmake_opts ) );

    char extra_incs[ 1024 ];
    build_extra_include_dirs_str( target, extra_incs, sizeof( extra_incs ) );
    const char* extra_sep = extra_incs[ 0 ] ? ";" : "";

    fprintf( f, "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <ClCompile>\n" );
    fprintf( f, "      <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
    if ( g_gen_fwd_compat )
        fprintf( f, "      <LanguageStandard>stdcpp20</LanguageStandard>\n" );
    fprintf( f, "      <UseStandardPreprocessor>true</UseStandardPreprocessor>\n" );
    fprintf( f, "      <AdditionalIncludeDirectories>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n",
             s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
    fprintf( f, "      <PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>\n",
             dbg_defines );
    fprintf( f, "    </ClCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    fprintf( f, "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <ClCompile>\n" );
    fprintf( f, "      <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
    if ( g_gen_fwd_compat )
        fprintf( f, "      <LanguageStandard>stdcpp20</LanguageStandard>\n" );
    fprintf( f, "      <UseStandardPreprocessor>true</UseStandardPreprocessor>\n" );
    fprintf( f, "      <AdditionalIncludeDirectories>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;%%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n",
             s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
    fprintf( f, "      <PreprocessorDefinitions>%s;%%(PreprocessorDefinitions)</PreprocessorDefinitions>\n",
             rel_defines );
    fprintf( f, "    </ClCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
             dbg_defines );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;$(VC_IncludePath);$(WindowsSDK_IncludePath);$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n",
             s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
    fprintf( f, "    <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
    if ( g_gen_fwd_compat )
        fprintf( f, "    <LanguageStandard>stdcpp20</LanguageStandard>\n" );
    fprintf( f, "    <IntelliSenseMode>windows-msvc-x64</IntelliSenseMode>\n" );
    fprintf( f, "    <NMakeAdditionalOptions>%s</NMakeAdditionalOptions>\n", nmake_opts );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
             rel_defines );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(ProjectDir)%s%s\\%s%s%s;$(VC_IncludePath);$(WindowsSDK_IncludePath);$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n",
             s_ctx.root_prefix, s_ctx.root_prefix, g_build_dir, g_gen_dir, extra_sep, extra_incs );
    fprintf( f, "    <LanguageStandard_C>stdc11</LanguageStandard_C>\n" );
    if ( g_gen_fwd_compat )
        fprintf( f, "    <LanguageStandard>stdcpp20</LanguageStandard>\n" );
    fprintf( f, "    <IntelliSenseMode>windows-msvc-x64</IntelliSenseMode>\n" );
    fprintf( f, "    <NMakeAdditionalOptions>%s</NMakeAdditionalOptions>\n", nmake_opts );
    fprintf( f, "  </PropertyGroup>\n" );
}

/*==============================================================================================
    write_vcxproj_common_header()

    Writes the boilerplate XML required for a Visual Studio Makefile project.
    Three layers of config data:
      1. Unconditional PropertyGroup: OutDir/IntDir and the NMake build/clean commands.
      2. Per-config IntelliSense groups (ItemDefinitionGroup + NMake PropertyGroup):
         see emit_intellisense_config_groups() above.
      3. Per-config LocalDebuggerWorkingDirectory so F5 launches from the project root.
==============================================================================================*/

static void
write_vcxproj_common_header( FILE* f, const char* guid, const char* out_name, 
                             target_type_t type, target_info_t* target )
{
    const char* ext = ".exe";
    if ( type == TARGET_STATIC_LIB )    ext = ".lib";
    if ( type == TARGET_DYNAMIC_LIB )   ext = s_ctx.is_monolithic ? ".lib" : ".dll";

    const char* mono_flag = s_ctx.is_monolithic ? " -monolithic" : "";

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <PreferredToolArchitecture>x64</PreferredToolArchitecture>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", guid );
    fprintf( f, "    <Keyword>Win32Proj</Keyword>\n" );
    fprintf( f, "    <Platform Condition=\"'$(Platform)'==''\">x64</Platform>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    fprintf( f, "    <PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>\n" );
    // LanguageStandard_C and IntelliSenseMode are emitted per-config below.
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Unconditional: build commands. -no-deps lets MSBuild's scheduler (which honors
    // ProjectDependencies in the .sln) own build order so parallel solution builds
    // never race on shared dep outputs.
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_ctx.root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
             s_ctx.root_prefix, g_build_dir, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd %s &amp;&amp; %s -no-deps -config $(Configuration) -target %s%s</NMakeBuildCommandLine>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, out_name, mono_flag );
    fprintf( f, "    <NMakeOutput>%sbin\\%s%s</NMakeOutput>\n", s_ctx.root_prefix, out_name, ext );
    fprintf( f, "    <NMakeCleanCommandLine>cd %s &amp;&amp; %s -clean -target %s</NMakeCleanCommandLine>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, out_name );
    fprintf( f, "    <NMakeCompileFile>cd %s &amp;&amp; %s -no-deps -config $(Configuration) -target %s%s</NMakeCompileFile>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, out_name, mono_flag );
    fprintf( f, "  </PropertyGroup>\n" );

    // Single-file compile (Ctrl+F7). Unconditional so the command is available
    // regardless of active configuration.
    fprintf( f, "  <ItemDefinitionGroup>\n" );
    fprintf( f, "    <NMakeCompile>\n" );
    fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; %s -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
             s_ctx.cd_root, s_ctx.build_tool_exe, out_name, mono_flag );
    fprintf( f, "    </NMakeCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    emit_intellisense_config_groups( f, target );

    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_ctx.cd_root );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_ctx.cd_root );
    fprintf( f, "  </PropertyGroup>\n" );
}

// Returns true if path's filename component matches one of target's unity units.
static bool
is_unit_file( const target_info_t* target, const char* path )
{
    const char* filename = path;
    for ( const char* p = path; *p; ++p )
        if ( *p == '/' || *p == '\\' ) filename = p + 1;
    for ( int j = 0; target->units[ j ]; ++j )
        if ( platform_stricmp( filename, target->units[ j ] ) == 0 ) return true;
    return false;
}

/*==============================================================================================
    write_vcxproj_filters_file()

    Writes one .vcxproj.filters file. Shared by the NMake target, NMake nav, and
    MSBuild target generators -- all three produce identical filters content.

    target == NULL means a navigation project: every file is <ClInclude> and no
    reflect-generated items are appended. Otherwise is_unit_file() selects the tag
    per entry and reflect items are appended when target->has_reflect is set.

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
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
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
    fprintf( f, "</Project>\n" );
    fclose( f );
}

/*==============================================================================================
    build_gen_proj_target()

    Emit one .vcxproj + matching .vcxproj.filters for a specific engine target.
    The vcxproj's <ClCompile> entry is the target's unity TU. Non-unity .c files
    are emitted as <ClInclude> (same as CMake) so IntelliSense context flows from
    the unity TU rather than per-file overrides. Per-file <ClCompile ExcludedFromBuild>
    with AdditionalOptions bypasses NMakeAdditionalOptions (/TC /std:c11) and causes
    the EDG parser to reject designated initializers and compound literals. Pure
    header files are also <ClInclude> for navigation.
==============================================================================================*/

static void
build_gen_proj_target( target_info_t* target )
{
    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_ctx.out_dir, target->name );

    char guid[ 64 ];
    guid_from_name( target->name, guid );

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    write_vcxproj_common_header( f, guid, target->name, target->type, target );

    g_file_count   = 0;
    g_filter_count = 0;

    scan_directory_recursive( target->root_dir, target->root_dir );

    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        bool is_unit = is_unit_file( target, g_files[ i ].path );

        char inc[ PATH_MAX + 32 ];
        gen_inc_path( g_files[ i ].path, inc, sizeof( inc ) );
        if ( is_unit )
        {
            const char* item_mono = s_ctx.is_monolithic ? " -monolithic" : "";
            fprintf( f, "    <ClCompile Include=\"%s\">\n", inc );
            fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; %s -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
                     s_ctx.cd_root, s_ctx.build_tool_exe, target->name, item_mono );
            fprintf( f, "    </ClCompile>\n" );
        }
        else
        {
            // All non-unity files (both .c and .h) are <ClInclude>. IntelliSense
            // context flows from the unity TU, inheriting NMakeAdditionalOptions
            // (/TC /std:c11 /Zc:preprocessor) without per-file overrides that
            // bypass the C mode and break designated initializers.
            fprintf( f, "    <ClInclude Include=\"%s\" />\n", inc );
        }
    }

    // For has_reflect targets, list the reflection-generated files explicitly.
    // The .c is a compile unit; the .h is a header for F12 navigation.
    // Both live in <build_dir>\<gen_dir>\ and are not under root_dir, so the
    // directory scan above misses them. They may not exist until the first build.
    if ( target->has_reflect )
    {
        const char* rname     = target->reflect_name ? target->reflect_name : target->name;
        const char* item_mono = s_ctx.is_monolithic ? " -monolithic" : "";
        fprintf( f, "    <ClCompile Include=\"%s%s\\%s\\%s.generated.c\">\n", s_ctx.root_prefix, g_build_dir,
                 g_gen_dir, rname );
        fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; %s -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
                 s_ctx.cd_root, s_ctx.build_tool_exe, target->name, item_mono );
        fprintf( f, "    </ClCompile>\n" );
        fprintf( f, "    <ClInclude Include=\"%s%s\\%s\\%s.generated.h\" />\n", s_ctx.root_prefix, g_build_dir,
                 g_gen_dir, rname );
    }

    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // .filters file mirrors the folder structure in Solution Explorer.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s.vcxproj.filters", s_ctx.out_dir, target->name );
    write_vcxproj_filters_file( filters_path, target );
}

/*==============================================================================================
    gen_proj_engine_navigation()

    Emit the "Mega" navigation .vcxproj for a solution. Scans nav_dir recursively
    and lists EVERY .c/.h as <ClInclude> (never ClCompile) so the developer gets a
    unified Solution Explorer view, but the nav project never competes with a target
    .vcxproj for any file's IntelliSense TU context.
==============================================================================================*/

static void
gen_proj_engine_navigation( const char* sln_name, const char* nav_dir, const char* default_target, const char* nav_guid )
{
    g_file_count   = 0;
    g_filter_count = 0;
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
    fprintf( f, "    <PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>\n" );
    // LanguageStandard_C and IntelliSenseMode are emitted per-config below.
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Nav build/clean are deliberate no-ops: per-target .vcxproj projects own their
    // own build and clean. Running a global build_tool.exe here in parallel with them
    // would race on shared dirs (e.g. build\\generated\\*).
    ( void )default_target;
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
    fprintf( f, "# Visual Studio Version 17\n" );

    // These GUIDs are FIXED by Visual Studio -- do not regenerate.
    const char* folder_type_guid = "{2150E333-8FDC-42A3-9474-1A3956D46DE8}";
    const char* cpp_type_guid    = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

    char        nav_guid[ 64 ]   = { 0 };
    {
        char key[ 128 ];
        snprintf( key, sizeof( key ), "nav:%s", out_name );
        guid_from_name( key, nav_guid );
    }

    // 1. Generate the nav .vcxproj file now so the file exists when VS opens the .sln,
    //    but defer writing the nav Project() entry until after all target entries so that
    //    VS gives target projects first-project priority for IntelliSense ownership.
    if ( sln->nav_dir )
    {
        const char* default_target =
            sln->target_names[ 0 ] ? sln->target_names[ 0 ] : "unknown";
        gen_proj_engine_navigation( out_name, sln->nav_dir, default_target, nav_guid );
    }

    // 2. Target projects.
    char folders[ 64 ][ PATH_MAX ];
    char folder_guids[ 64 ][ 64 ];
    int  folder_count = 0;

    for ( const char* const* tn = sln->target_names; *tn; ++tn )
    {
        target_info_t* target = find_target( *tn );
        if ( target )
        {
            char guid[ 64 ];
            guid_from_name( target->name, guid );
            fprintf( f, "Project(\"%s\") = \"%s\", \"%s.vcxproj\", \"%s\"\n", cpp_type_guid, target->name,
                     target->name, guid );

            if ( target->deps[ 0 ] || target->tool_deps[ 0 ] || target->has_reflect || !target->is_build_tool )
            {
                fprintf( f, "\tProjectSection(ProjectDependencies) = postProject\n" );

                for ( int i = 0; target->deps[ i ]; ++i )
                {
                    char dep_guid[ 64 ];
                    guid_from_name( target->deps[ i ], dep_guid );
                    fprintf( f, "\t\t%s = %s\n", dep_guid, dep_guid );
                }

                for ( int i = 0; target->tool_deps[ i ]; ++i )
                {
                    char tool_guid[ 64 ];
                    guid_from_name( target->tool_deps[ i ], tool_guid );
                    fprintf( f, "\t\t%s = %s\n", tool_guid, tool_guid );
                }

                // Implicit dep: every target depends on build_tool when it is in the same solution.
                if ( !target->is_build_tool )
                {
                    bool bt_in_sln = false;
                    for ( const char** tn2 = sln->target_names; *tn2; ++tn2 )
                        if ( strcmp( *tn2, "build_tool" ) == 0 )
                        {
                            bt_in_sln = true;
                            break;
                        }

                    if ( bt_in_sln )
                    {
                        for ( int k = 0; k < g_target_count; ++k )
                        {
                            if ( g_targets[ k ].is_build_tool )
                            {
                                char bt_guid[ 64 ];
                                guid_from_name( g_targets[ k ].name, bt_guid );
                                fprintf( f, "\t\t%s = %s\n", bt_guid, bt_guid );
                                break;
                            }
                        }
                    }
                }

                // Implicit dep: has_reflect targets always depend on the reflect tool.
                if ( target->has_reflect )
                {
                    for ( int k = 0; k < g_target_count; ++k )
                    {
                        if ( g_targets[ k ].is_reflect_tool )
                        {
                            char refl_guid[ 64 ];
                            guid_from_name( g_targets[ k ].name, refl_guid );
                            fprintf( f, "\t\t%s = %s\n", refl_guid, refl_guid );
                            break;
                        }
                    }
                }

                fprintf( f, "\tEndProjectSection\n" );
            }

            fprintf( f, "EndProject\n" );

            // Collect SLN folders for nesting.
            // Register every path segment so "A/B" creates both "A" and "A/B" folders.
            {
                char tmp[ PATH_MAX ];
                snprintf( tmp, sizeof( tmp ), "%s", target->virtual_folder );
                for ( char* p = tmp; *p; p++ )
                    if ( *p == '\\' ) *p = '/';

                char* p = tmp;
                while ( *p )
                {
                    if ( *p == '/' )
                    {
                        *p = '\0';
                        bool found = false;
                        for ( int j = 0; j < folder_count; ++j )
                            if ( strcmp( folders[ j ], tmp ) == 0 ) { found = true; break; }
                        if ( !found && folder_count < 64 )
                        {
                            snprintf( folders[ folder_count ], PATH_MAX, "%s", tmp );
                            char key[ 192 ];
                            snprintf( key, sizeof( key ), "folder:%s:%s", out_name, tmp );
                            guid_from_name( key, folder_guids[ folder_count ] );
                            folder_count++;
                        }
                        *p = '/';
                    }
                    p++;
                }
                // Register the leaf (full path).
                bool found = false;
                for ( int j = 0; j < folder_count; ++j )
                    if ( strcmp( folders[ j ], tmp ) == 0 ) { found = true; break; }
                if ( !found && folder_count < 64 )
                {
                    snprintf( folders[ folder_count ], PATH_MAX, "%s", tmp );
                    char key[ 192 ];
                    snprintf( key, sizeof( key ), "folder:%s:%s", out_name, tmp );
                    guid_from_name( key, folder_guids[ folder_count ] );
                    folder_count++;
                }
            }
        }
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
    for ( int i = 0; i < folder_count; ++i )
    {
        const char* leaf = strrchr( folders[ i ], '/' );
        const char* display = leaf ? leaf + 1 : folders[ i ];
        fprintf( f, "Project(\"%s\") = \"%s\", \"%s\", \"%s\"\n", folder_type_guid, display, display,
                 folder_guids[ i ] );
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
            char guid[ 64 ];
            guid_from_name( t->name, guid );
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
            for ( char* p = norm; *p; p++ )
                if ( *p == '\\' ) *p = '/';

            char proj_guid[ 64 ];
            guid_from_name( t->name, proj_guid );
            for ( int j = 0; j < folder_count; ++j )
            {
                if ( strcmp( folders[ j ], norm ) == 0 )
                {
                    fprintf( f, "\t\t%s = %s\n", proj_guid, folder_guids[ j ] );
                    break;
                }
            }
        }
    }

    // Map each non-root folder to its parent folder.
    for ( int i = 0; i < folder_count; ++i )
    {
        const char* slash = strrchr( folders[ i ], '/' );
        if ( !slash )
            continue;
        char parent[ PATH_MAX ];
        int  parent_len = ( int )( slash - folders[ i ] );
        strncpy( parent, folders[ i ], parent_len );
        parent[ parent_len ] = '\0';
        for ( int j = 0; j < folder_count; ++j )
        {
            if ( strcmp( folders[ j ], parent ) == 0 )
            {
                fprintf( f, "\t\t%s = %s\n", folder_guids[ i ], folder_guids[ j ] );
                break;
            }
        }
    }

    fprintf( f, "\tEndGlobalSection\n" );
    fprintf( f, "EndGlobal\n" );
    fclose( f );
}

/*==============================================================================================
    run_solution_passes()

    Shared iteration loop for NMake and MSBuild generation. For each local solution,
    sets s_ctx state, computes path parts, ensures the output dir, and calls per_target_fn
    per target then build_gen_solution.

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

/*==============================================================================================

    build_gen_projects()

    Top-level entry point invoked by `build_tool.exe -gen`. Regenerates every NMake
    .vcxproj and every .sln from the current target/solution registry.

    Safe to re-run anytime; the generated XML is fully deterministic given the
    registry contents. VS user state survives regen as long as target names
    don't change (see guid_from_name).

==============================================================================================*/

void
build_gen_projects( const gen_manifest_t* m )
{
    snprintf( s_ctx.build_tool_exe, sizeof( s_ctx.build_tool_exe ), "%s", m->build_tool_exe );
    run_solution_passes( m, "_nm", "", "Solution", build_gen_proj_target );
}

/*============================================================================================*/
