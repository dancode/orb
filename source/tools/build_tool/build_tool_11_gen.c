/*==============================================================================================

    build_tool_11_gen.c -- Visual Studio .sln / .vcxproj generator.

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

static const char* s_out_dir           = NULL;
static char        s_root_prefix[ 32 ] = { 0 };
static char        s_cd_root[ 32 ]     = { 0 };
static bool        s_is_monolithic     = false;

static void
compute_path_parts( const char* out_dir )
{
    // Count directory components: "build\\proj" has one separator -> depth 2.
    int depth = 1;
    for ( const char* p = out_dir; *p; ++p )
        if ( *p == '\\' || *p == '/' )
            depth++;

    s_out_dir          = out_dir;

    s_root_prefix[ 0 ] = '\0';
    for ( int i = 0; i < depth; ++i ) strcat( s_root_prefix, "..\\" );

    // cd_root is the same path without the trailing backslash.
    s_cd_root[ 0 ] = '\0';
    strcat( s_cd_root, ".." );
    for ( int i = 1; i < depth; ++i ) strcat( s_cd_root, "\\.." );
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

// Map a physical file path to a virtual VS filter path relative to root_dir.
// Example: path "source/engine/core/core.c", root_dir "source/engine" -> "core"
static void
get_filter_for_path( const char* path, const char* root_dir, char* out_filter )
{
    out_filter[ 0 ] = '\0';

    size_t root_len = strlen( root_dir );
    if ( strncmp( path, root_dir, root_len ) != 0 )
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
    snprintf( search_path, sizeof( search_path ), "%s\\*", dir );

    platform_find_data_t find_data;
    platform_find_t handle = platform_find_first( search_path, &find_data );
    if ( handle == PLATFORM_FIND_INVALID )
        return;

    do
    {
        if ( strcmp( find_data.name, "." ) == 0 || strcmp( find_data.name, ".." ) == 0 )
            continue;

        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "%s\\%s", dir, find_data.name );

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
    build_intellisense_defines()

    Build the semicolon-separated NMakePreprocessorDefinitions value from the
    shared define tables in 02_data.c. Both the per-target and the nav project
    vcxproj callers use this so neither can diverge from what cc_fill_compile_cmd
    passes to cl.exe.
==============================================================================================*/

static void
build_intellisense_defines( char* buf, size_t buf_size, config_t config, target_info_t* target )
{
    buf[ 0 ]    = '\0';
    size_t used = 0;

#define ISDEF_APPEND( s )                  \
    do                                     \
    {                                      \
        size_t slen = strlen( s );         \
        if ( used + slen + 2 < buf_size )  \
        {                                  \
            if ( used )                    \
                buf[ used++ ] = ';';       \
            memcpy( buf + used, s, slen ); \
            used += slen;                  \
            buf[ used ] = '\0';            \
        }                                  \
    }                                      \
    while ( 0 )

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
            if ( dep->type == TARGET_STATIC_LIB || ( dep->type == TARGET_DYNAMIC_LIB && s_is_monolithic ) )
            {
                char dep_upper[ 128 ];
                get_target_upper( dep->name, dep_upper, sizeof( dep_upper ) );
                snprintf( define, sizeof( define ), "%s_STATIC", dep_upper );
                ISDEF_APPEND( define );
            }
        }
        if ( s_is_monolithic )
            ISDEF_APPEND( "BUILD_STATIC" );
    }

#undef ISDEF_APPEND
}

/*==============================================================================================
    write_vcxproj_common_header()

    Writes the boilerplate XML required for a Visual Studio Makefile project.
    Two layers of PropertyGroup:
      1. Unconditional: OutDir/IntDir and the NMake build/clean/compile commands.
      2. Per-configuration (Debug|x64, Release|x64): preprocessor defines, include
         paths, and AdditionalOptions that feed IntelliSense.
==============================================================================================*/

static void
write_vcxproj_common_header( FILE* f, const char* guid, const char* out_name, 
                             target_type_t type, target_info_t* target )
{
    const char* ext = ".exe";
    if ( type == TARGET_STATIC_LIB )    ext = ".lib";
    if ( type == TARGET_DYNAMIC_LIB )   ext = s_is_monolithic ? ".lib" : ".dll";

    const char* mono_flag = s_is_monolithic ? " -monolithic" : "";

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", guid );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    fprintf( f, "    <PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Unconditional: build commands. -no-deps lets MSBuild's scheduler (which honors
    // ProjectDependencies in the .sln) own build order so parallel solution builds
    // never race on shared dep outputs.
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
             s_root_prefix, g_build_dir, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -config $(Configuration) -target %s%s</NMakeBuildCommandLine>\n",
             s_cd_root, out_name, mono_flag );
    fprintf( f, "    <NMakeOutput>%sbin\\%s%s</NMakeOutput>\n", s_root_prefix, out_name, ext );
    fprintf( f, "    <NMakeCleanCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -clean -target %s</NMakeCleanCommandLine>\n",
             s_cd_root, out_name );
    fprintf( f, "    <NMakeCompileFile>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -config $(Configuration) -target %s%s</NMakeCompileFile>\n",
             s_cd_root, out_name, mono_flag );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n",
             s_root_prefix );
    fprintf( f, "  </PropertyGroup>\n" );

    // Single-file compile (Ctrl+F7). NMakeCompileSelectedFiles creates a NMakeCompile
    // item at runtime and reads %(NMakeCompile.NMakeCompileFileCommandLine) as the command.
    // VS does not inject the selected file path, so we compile all unity units (-compile-only).
    fprintf( f, "  <ItemDefinitionGroup>\n" );
    fprintf( f, "    <NMakeCompile>\n" );
    fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
             s_cd_root, out_name, mono_flag );
    fprintf( f, "    </NMakeCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    // Per-config IntelliSense context.
    {
        char additional_opts[ 256 ] = { 0 };
        for ( int i = 0; g_intellisense_flags[ i ]; ++i )
        {
            if ( i )
                strcat( additional_opts, " " );
            strcat( additional_opts, g_intellisense_flags[ i ] );
        }

        char dbg_defines[ 1024 ];
        char rel_defines[ 1024 ];
        build_intellisense_defines( dbg_defines, sizeof( dbg_defines ), CONFIG_DEBUG, target );
        build_intellisense_defines( rel_defines, sizeof( rel_defines ), CONFIG_RELEASE, target );

        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
        fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
                 dbg_defines );
        fprintf( f, "    <AdditionalOptions>%s %%(AdditionalOptions)</AdditionalOptions>\n", additional_opts );
        fprintf( f, "  </PropertyGroup>\n" );

        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
        fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
                 rel_defines );
        fprintf( f, "    <AdditionalOptions>%s %%(AdditionalOptions)</AdditionalOptions>\n", additional_opts );
        fprintf( f, "  </PropertyGroup>\n" );
    }

    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_cd_root );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_cd_root );
    fprintf( f, "  </PropertyGroup>\n" );
}

/*==============================================================================================
    build_gen_proj_target()

    Emit one .vcxproj + matching .vcxproj.filters for a specific engine target.
    The vcxproj's <ClCompile> entry is the target's unity TU; every other file
    under target->root_dir is added as <ClInclude> for Solution Explorer visibility
    without claiming compile ownership.
==============================================================================================*/

static void
build_gen_proj_target( target_info_t* target, int index )
{
    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_out_dir, target->name );

    char guid[ 64 ];
    guid_from_name( target->name, guid );
    ( void )index;

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
        bool        is_unit  = false;
        const char* filename = g_files[ i ].path;
        for ( const char* p = g_files[ i ].path; *p; ++p )
            if ( *p == '/' || *p == '\\' )
                filename = p + 1;

        for ( int j = 0; target->units[ j ]; ++j )
        {
            if ( platform_stricmp( filename, target->units[ j ] ) == 0 )
            {
                is_unit = true;
                break;
            }
        }

        if ( is_unit )
        {
            const char* item_mono = s_is_monolithic ? " -monolithic" : "";
            fprintf( f, "    <ClCompile Include=\"%s%s\">\n", s_root_prefix, g_files[ i ].path );
            fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n",
                     s_cd_root, target->name, item_mono );
            fprintf( f, "    </ClCompile>\n" );
        }
        else
        {
            fprintf( f, "    <ClInclude Include=\"%s%s\" />\n", s_root_prefix, g_files[ i ].path );
        }
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // .filters file mirrors the folder structure in Solution Explorer.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s.vcxproj.filters", s_out_dir, target->name );
    f = fopen( filters_path, "w" );
    if ( f )
    {
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
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            bool        is_unit  = false;
            const char* filename = strrchr( g_files[ i ].path, '/' );
            if ( !filename )
                filename = strrchr( g_files[ i ].path, '\\' );
            if ( filename )
                filename++;
            else
                filename = g_files[ i ].path;

            for ( int j = 0; target->units[ j ]; ++j )
            {
                if ( platform_stricmp( filename, target->units[ j ] ) == 0 )
                {
                    is_unit = true;
                    break;
                }
            }

            const char* tag = is_unit ? "ClCompile" : "ClInclude";
            fprintf( f, "    <%s Include=\"%s%s\">\n", tag, s_root_prefix, g_files[ i ].path );
            if ( g_files[ i ].filter[ 0 ] != '\0' )
                fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
            fprintf( f, "    </%s>\n", tag );
        }
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }
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
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s_nav.vcxproj", s_out_dir, sln_name );
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
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );
    fprintf( f, "    <PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // Nav build/clean are deliberate no-ops: per-target .vcxproj projects own their
    // own build and clean. Running a global build_tool.exe here in parallel with them
    // would race on shared dirs (e.g. build\\generated\\*).
    ( void )default_target;
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n",
             s_root_prefix, g_build_dir, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>echo       [nav] navigation-only project, nothing to build.</NMakeBuildCommandLine>\n" );
    fprintf( f, "    <NMakeOutput>$(ProjectDir)%s%s\\$(ProjectName)\\$(Configuration)\\nav.stamp</NMakeOutput>\n",
             s_root_prefix, g_int_dir );
    fprintf( f, "    <NMakeCleanCommandLine>echo       [nav] navigation-only project, nothing to clean.</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakeCompileFile>echo       [nav] navigation-only project.</NMakeCompileFile>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n",
             s_root_prefix );
    fprintf( f, "  </PropertyGroup>\n" );

    {
        char additional_opts[ 256 ] = { 0 };
        for ( int i = 0; g_intellisense_flags[ i ]; ++i )
        {
            if ( i )
                strcat( additional_opts, " " );
            strcat( additional_opts, g_intellisense_flags[ i ] );
        }

        char dbg_defines[ 1024 ];
        char rel_defines[ 1024 ];
        build_intellisense_defines( dbg_defines, sizeof( dbg_defines ), CONFIG_DEBUG, NULL );
        build_intellisense_defines( rel_defines, sizeof( rel_defines ), CONFIG_RELEASE, NULL );

        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
        fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
                 dbg_defines );
        fprintf( f, "    <AdditionalOptions>%s %%(AdditionalOptions)</AdditionalOptions>\n", additional_opts );
        fprintf( f, "  </PropertyGroup>\n" );
        fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
        fprintf( f, "    <NMakePreprocessorDefinitions>%s;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
                 rel_defines );
        fprintf( f, "    <AdditionalOptions>%s %%(AdditionalOptions)</AdditionalOptions>\n", additional_opts );
        fprintf( f, "  </PropertyGroup>\n" );
    }

    // All files as ClInclude regardless of extension. Listing .c files as ClCompile
    // would create a competing TU context; VS picks last-loaded-wins per file, so
    // headers would resolve under this empty context instead of the real target's
    // context (wrong defines, wrong API visible). ClInclude has no TU semantics.
    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
        fprintf( f, "    <ClInclude Include=\"%s%s\" />\n", s_root_prefix, g_files[ i ].path );
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s_nav.vcxproj.filters", s_out_dir, sln_name );
    f = fopen( filters_path, "w" );
    if ( f )
    {
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
        fprintf( f, "  </ItemGroup>\n" );

        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            fprintf( f, "    <ClInclude Include=\"%s%s\">\n", s_root_prefix, g_files[ i ].path );
            if ( g_files[ i ].filter[ 0 ] != '\0' )
                fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
            fprintf( f, "    </ClInclude>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }
}

/*==============================================================================================
    build_gen_solution()

    Write the .sln descriptor for one entry of g_solutions[]. Pipeline:
      1. Compute nav_guid and emit the nav .vcxproj if nav_dir is set.
      2. For each target: emit Project entry + ProjectDependencies cross-references.
      3. Emit virtual SLN folder entries.
      4. Emit GlobalSection blocks: configuration mapping + NestedProjects.
==============================================================================================*/

static void
build_gen_solution( solution_info_t* sln )
{
    char sln_path[ PATH_MAX ];
    snprintf( sln_path, sizeof( sln_path ), "%s\\%s.sln", s_out_dir, sln->name );
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
        snprintf( key, sizeof( key ), "nav:%s", sln->name );
        guid_from_name( key, nav_guid );
    }

    // 1. Navigation project.
    if ( sln->nav_dir )
    {
        const char* default_target =
            ( sln->target_names && sln->target_names[ 0 ] ) ? sln->target_names[ 0 ] : "unknown";
        gen_proj_engine_navigation( sln->name, sln->nav_dir, default_target, nav_guid );
        fprintf( f, "Project(\"%s\") = \"%s_nav\", \"%s_nav.vcxproj\", \"%s\"\n", cpp_type_guid, sln->name,
                 sln->name, nav_guid );
        fprintf( f, "EndProject\n" );
    }

    // 2. Target projects.
    char folders[ 16 ][ PATH_MAX ];
    char folder_guids[ 16 ][ 64 ];
    int  folder_count = 0;

    for ( const char** tn = sln->target_names; *tn; ++tn )
    {
        target_info_t* target = NULL;
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( strcmp( g_targets[ i ].name, *tn ) == 0 )
            {
                target = &g_targets[ i ];
                break;
            }
        }

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
            bool found = false;
            for ( int j = 0; j < folder_count; ++j )
                if ( strcmp( folders[ j ], target->sln_folder ) == 0 )
                {
                    found = true;
                    break;
                }
            if ( !found && folder_count < 16 )
            {
                snprintf( folders[ folder_count ], PATH_MAX, "%s", target->sln_folder );
                char key[ 192 ];
                snprintf( key, sizeof( key ), "folder:%s:%s", sln->name, target->sln_folder );
                guid_from_name( key, folder_guids[ folder_count ] );
                folder_count++;
            }
        }
    }

    // 3. Virtual SLN folders.
    for ( int i = 0; i < folder_count; ++i )
    {
        fprintf( f, "Project(\"%s\") = \"%s\", \"%s\", \"%s\"\n", folder_type_guid, folders[ i ],
                 folders[ i ], folder_guids[ i ] );
        fprintf( f, "EndProject\n" );
    }

    // 4. Global sections.
    fprintf( f, "Global\n" );
    fprintf( f, "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n" );
    fprintf( f, "\t\tDebug|x64 = Debug|x64\n" );
    fprintf( f, "\t\tRelease|x64 = Release|x64\n" );
    fprintf( f, "\tEndGlobalSection\n" );

    fprintf( f, "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n" );
    if ( sln->nav_dir )
    {
        fprintf( f, "\t\t%s.Debug|x64.ActiveCfg = Debug|x64\n", nav_guid );
        fprintf( f, "\t\t%s.Debug|x64.Build.0 = Debug|x64\n", nav_guid );
        fprintf( f, "\t\t%s.Release|x64.ActiveCfg = Release|x64\n", nav_guid );
        fprintf( f, "\t\t%s.Release|x64.Build.0 = Release|x64\n", nav_guid );
    }
    for ( const char** tn = sln->target_names; *tn; ++tn )
    {
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( strcmp( g_targets[ i ].name, *tn ) == 0 )
            {
                char guid[ 64 ];
                guid_from_name( g_targets[ i ].name, guid );
                fprintf( f, "\t\t%s.Debug|x64.ActiveCfg = Debug|x64\n", guid );
                fprintf( f, "\t\t%s.Debug|x64.Build.0 = Debug|x64\n", guid );
                fprintf( f, "\t\t%s.Release|x64.ActiveCfg = Release|x64\n", guid );
                fprintf( f, "\t\t%s.Release|x64.Build.0 = Release|x64\n", guid );
                break;
            }
        }
    }
    fprintf( f, "\tEndGlobalSection\n" );

    fprintf( f, "\tGlobalSection(NestedProjects) = preSolution\n" );
    for ( const char** tn = sln->target_names; *tn; ++tn )
    {
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( strcmp( g_targets[ i ].name, *tn ) == 0 )
            {
                char proj_guid[ 64 ];
                guid_from_name( g_targets[ i ].name, proj_guid );
                for ( int j = 0; j < folder_count; ++j )
                {
                    if ( strcmp( folders[ j ], g_targets[ i ].sln_folder ) == 0 )
                    {
                        fprintf( f, "\t\t%s = %s\n", proj_guid, folder_guids[ j ] );
                        break;
                    }
                }
                break;
            }
        }
    }
    fprintf( f, "\tEndGlobalSection\n" );
    fprintf( f, "EndGlobal\n" );
    fclose( f );
}

/*==============================================================================================

    build_gen_projects()

    Top-level entry point invoked by `build_tool.exe -gen`. Regenerates every
    .vcxproj and every .sln from the current target/solution registry.

    Safe to re-run anytime; the generated XML is fully deterministic given the
    registry contents. VS user state survives regen as long as target names
    don't change (see guid_from_name).

==============================================================================================*/

void
build_gen_projects( void )
{
    for ( int i = 0; i < g_solution_count; ++i )
    {
        solution_info_t* sln = &g_solutions[ i ];
        s_is_monolithic = sln->is_monolithic;

        compute_path_parts( sln->out_dir );
        ensure_dir( sln->out_dir );

        printf( "Generating Solution '%s' in %s/...\n", sln->name, sln->out_dir );

        for ( const char** tn = sln->target_names; *tn; ++tn )
        {
            for ( int j = 0; j < g_target_count; ++j )
            {
                if ( strcmp( g_targets[ j ].name, *tn ) == 0 )
                {
                    build_gen_proj_target( &g_targets[ j ], j );
                    break;
                }
            }
        }

        build_gen_solution( sln );
    }

    printf( "\nProjects generated successfully.\n" );
}

/*============================================================================================*/
