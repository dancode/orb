/*==============================================================================================

    build_tool_gen.c -- Visual Studio .sln / .vcxproj generator.

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

#define MAX_FILES   1024   // Max source files scanned per vcxproj.
#define MAX_FILTERS  512   // Max virtual filter folders per vcxproj.

// Metadata for one source file picked up by scan_directory_recursive().
// Drives both the <ItemGroup> entries in the .vcxproj and the matching
// <Filter> mappings in the .filters file.
typedef struct
{
    char path[ BT_PATH_MAX ];   // Relative path from project root.
    char filter[ BT_PATH_MAX ]; // Virtual folder path in VS (e.g. "engine\\core").
    bool is_header;             // True for .h files; influences ClInclude vs ClCompile choice.
} file_info_t;

// Per-scan buffers. Reset (g_file_count = 0, g_filter_count = 0) at the
// start of every project emission — they are reusable scratch, not state.
static file_info_t g_files[ MAX_FILES ];
static int         g_file_count = 0;

static char g_filters[ MAX_FILTERS ][ BT_PATH_MAX ];
static int  g_filter_count = 0;

/**
 * guid_from_name()
 *
 * Produces a deterministic 128-bit GUID from a string, formatted in the
 * standard 8-4-4-4-12 hex form Visual Studio expects.
 *
 * The previous scheme derived GUIDs from the target's index in g_targets[]
 * (e.g. {...12B000+i}). That made every GUID a function of registry order,
 * so inserting or reordering a target shifted every subsequent GUID. The
 * resulting churn invalidated user-visible state stored against those
 * GUIDs: .suo files, window layouts, breakpoints, ProjectDependencies
 * cross-references, source-control mappings, anything CMake/MSBuild cached.
 *
 * Hashing the project's name instead means the GUID is stable for the life
 * of the name — registry reorderings cost nothing, renames cost only their
 * own project's identity (which is correct: a rename is a new project).
 *
 * Two FNV-1a passes with different seeds and primes fill the 16 bytes.
 * FNV is not cryptographic, but we are not defending against collisions
 * from an adversary — we just need 2^128 worth of spread across the few
 * dozen names this tool will ever see.
 */
static void
guid_from_name( const char* name, char* out )
{
    unsigned long long h1 = 0xcbf29ce484222325ULL;
    unsigned long long h2 = 0x9ae16a3b2f90404fULL;
    for ( const unsigned char* p = ( const unsigned char* )name; *p; ++p )
    {
        h1 = ( h1 ^ *p ) * 0x100000001b3ULL;
        h2 = ( h2 ^ *p ) * 0x880355f21e6d1965ULL;
    }
    // 38-byte fixed output (32 hex + 4 dashes + 2 braces + NUL); caller passes
    // a 64-byte buffer. Use snprintf with a conservative cap for safety.
    snprintf( out, 64, "{%08X-%04X-%04X-%04X-%04X%08X}",
              ( unsigned int )( h1 >> 32 ),
              ( unsigned int )( ( h1 >> 16 ) & 0xFFFFu ),
              ( unsigned int )( h1 & 0xFFFFu ),
              ( unsigned int )( h2 >> 48 ),
              ( unsigned int )( ( h2 >> 32 ) & 0xFFFFu ),
              ( unsigned int )( h2 & 0xFFFFFFFFu ) );
}

/**
 * add_filter()
 *
 * Registers a virtual folder ("Filter" in VS terminology) for this scan.
 * Filters are how the Solution Explorer renders nested folders that don't
 * exist on disk — or, in our case, that mirror disk layout but live in a
 * separate XML namespace from the source files.
 *
 * Idempotent: a no-op if `filter` is empty or already present.
 */
static void
add_filter( const char* filter )
{
    if ( filter[ 0 ] == '\0' ) return;
    for ( int i = 0; i < g_filter_count; ++i )
    {
        if ( _stricmp( g_filters[ i ], filter ) == 0 ) return;
    }
    if ( g_filter_count < MAX_FILTERS )
    {
        strcpy( g_filters[ g_filter_count++ ], filter );
    }
}

/**
 * add_filters_recursive()
 * 
 * Ensures all parent folders of a filter path are also registered.
 * e.g. "engine\\core\\win" -> adds "engine", "engine\\core", and "engine\\core\\win".
 * This is necessary for VS to display the nested folder structure correctly.
 */
static void
add_filters_recursive( const char* filter )
{
    char  tmp[ BT_PATH_MAX ];
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
    add_filter( tmp );
}

/**
 * get_filter_for_path()
 * 
 * Maps a physical file path to a virtual VS filter path.
 * e.g. "source/engine/core/core.c" -> "engine\\core"
 */
static void
get_filter_for_path( const char* path, const char* root_dir, char* out_filter )
{
    out_filter[ 0 ] = '\0';
    size_t root_len = strlen( root_dir );
    if ( strncmp( path, root_dir, root_len ) == 0 )
    {
        const char* sub = path + root_len;
        if ( *sub == '/' || *sub == '\\' ) sub++;

        const char* last_slash = strrchr( sub, '/' );
        if ( !last_slash ) last_slash = strrchr( sub, '\\' );

        if ( last_slash )
        {
            size_t len = last_slash - sub;
            strncpy( out_filter, sub, len );
            out_filter[ len ] = '\0';
            // VS uses backslashes for filters.
            for ( char* p = out_filter; *p; ++p )
                if ( *p == '/' ) *p = '\\';
        }
    }
}

/**
 * scan_directory_recursive()
 * 
 * Traverses the source tree and collects all .c and .h files to be
 * included in the navigation project.
 */
static void
scan_directory_recursive( const char* dir, const char* root_dir )
{
    char search_path[ BT_PATH_MAX ];
    snprintf( search_path, sizeof( search_path ), "%s/*", dir );

    struct _finddata_t find_data;
    intptr_t           handle = _findfirst( search_path, &find_data );

    if ( handle == -1 ) return;

    do
    {
        // Skip hidden/special directories.
        if ( strcmp( find_data.name, "." ) == 0 || strcmp( find_data.name, ".." ) == 0 ) continue;

        char path[ BT_PATH_MAX ];
        snprintf( path, sizeof( path ), "%s/%s", dir, find_data.name );

        if ( find_data.attrib & _A_SUBDIR )
        {
            scan_directory_recursive( path, root_dir );
        }
        else
        {
            const char* ext = strrchr( find_data.name, '.' );
            if ( ext )
            {
                bool is_c = _stricmp( ext, ".c" ) == 0;
                bool is_h = _stricmp( ext, ".h" ) == 0;

                if ( ( is_c || is_h ) && g_file_count < MAX_FILES )
                {
                    file_info_t* f = &g_files[ g_file_count++ ];
                    strcpy( f->path, path );
                    f->is_header = is_h;
                    get_filter_for_path( path, root_dir, f->filter );
                    if ( f->filter[ 0 ] != '\0' ) add_filters_recursive( f->filter );
                }
            }
        }
    }
    while ( _findnext( handle, &find_data ) == 0 );

    _findclose( handle );
}

/**
 * write_vcxproj_common_header()
 *
 * Writes the boilerplate XML required for a Visual Studio Makefile project.
 * Two layers of <PropertyGroup>:
 *
 *  1. An unconditional group with OutDir/IntDir and the NMake build, clean,
 *     and run commands that drive build_tool.exe.
 *  2. Per-configuration groups (Debug|x64, Release|x64) for the fields that
 *     actually feed IntelliSense — preprocessor defines, include paths, and
 *     AdditionalOptions. IntelliSense reads these to construct the TU
 *     context for headers and source. Splitting them per-config lets _DEBUG
 *     vs NDEBUG, and any future config-specific defines, diverge without
 *     re-emitting the rest of the property block.
 *
 * /std:c11 and /Zc:preprocessor are passed via AdditionalOptions so the
 * IntelliSense parser matches what cl.exe actually does for the build. We
 * also project the target's <NAME>_STATIC define here so APIs guarded by
 * the static-link symbol resolve correctly while editing.
 *
 * `static_def`: NULL for the nav project (no _STATIC). Else the upper-cased
 * target name, e.g. "CORE" → emits CORE_STATIC.
 */
static void
write_vcxproj_common_header( FILE* f, const char* guid, const char* out_name,
                             target_type_t type, const char* static_def )
{
    const char* ext = ".exe";
    if ( type == TARGET_STATIC_LIB ) ext = ".lib";
    if ( type == TARGET_DYNAMIC_LIB ) ext = ".dll";

    fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    fprintf( f, "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );

    // Configurations (Debug/Release)
    fprintf( f, "  <ItemGroup Label=\"ProjectConfigurations\">\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Debug|x64\"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "    <ProjectConfiguration Include=\"Release|x64\"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>\n" );
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <PropertyGroup Label=\"Globals\">\n" );
    fprintf( f, "    <ProjectGuid>%s</ProjectGuid>\n", guid );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n" );
    fprintf( f, "  <PropertyGroup Label=\"Configuration\">\n" );
    fprintf( f, "    <ConfigurationType>Makefile</ConfigurationType>\n" );   // Makefile type
    fprintf( f, "    <PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n" );

    // --- Unconditional: build commands and paths ---
    //
    // The "Hook": tell VS to call our build_tool.exe with the specific
    // target. -no-deps lets MSBuild's scheduler (which honors the .sln's
    // ProjectDependencies) own build order; each project builds only
    // itself, so parallel solution builds never race shared dep outputs.
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)..\\bin\\</OutDir>\n" );
    fprintf( f, "    <IntDir>$(ProjectDir)%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n", g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -no-deps -config $(Configuration) -target %s</NMakeBuildCommandLine>\n", out_name );
    fprintf( f, "    <NMakeOutput>..\\bin\\%s%s</NMakeOutput>\n", out_name, ext );
    fprintf( f, "    <NMakeCleanCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -clean -target %s</NMakeCleanCommandLine>\n", out_name );
    fprintf( f, "    <NMakeCompileFile>cd .. &amp;&amp; bin\\build_tool.exe -no-deps -config $(Configuration) -target %s</NMakeCompileFile>\n", out_name );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)..\\source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    // --- Single-file compile (Ctrl+F7) ---
    //
    // NMakeCompileSelectedFiles in Microsoft.MakeFile.Targets creates a NMakeCompile item
    // at runtime and reads %(NMakeCompile.NMakeCompileFileCommandLine) as the command to run.
    // VS does NOT inject the selected file path into any accessible property or env var,
    // so the command runs -compile-only (all unity units, no link) for the whole target.
    // ItemDefinitionGroup applies this metadata to the dynamically created NMakeCompile item.
    fprintf( f, "  <ItemDefinitionGroup>\n" );
    fprintf( f, "    <NMakeCompile>\n" );
    fprintf( f, "      <NMakeCompileFileCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -no-deps -compile-only -config $(Configuration) -target %s</NMakeCompileFileCommandLine>\n",out_name );
    fprintf( f, "    </NMakeCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    // --- Debug|x64 IntelliSense context ---
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;ARCH_X64;_CRT_SECURE_NO_WARNINGS;%s%s_DEBUG;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
             static_def ? static_def : "",
             static_def ? "_STATIC;" : "" );
    fprintf( f, "    <AdditionalOptions>/std:c11 /Zc:preprocessor %%(AdditionalOptions)</AdditionalOptions>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    // --- Release|x64 IntelliSense context ---
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;ARCH_X64;_CRT_SECURE_NO_WARNINGS;%s%sNDEBUG;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n",
             static_def ? static_def : "",
             static_def ? "_STATIC;" : "" );
    fprintf( f, "    <AdditionalOptions>/std:c11 /Zc:preprocessor %%(AdditionalOptions)</AdditionalOptions>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
}

/**
 * build_gen_proj_target()
 *
 * Emit one .vcxproj + matching .vcxproj.filters for a specific engine
 * target. The vcxproj's <ClCompile> entry is the target's actual unity TU;
 * every other discovered file under target->root_dir is added as
 * <ClInclude> so it shows up in Solution Explorer and IntelliSense can
 * index it, without claiming compile ownership.
 *
 * `index` is preserved in the signature for backwards compat with earlier
 * call sites — the GUID derivation no longer uses it (see guid_from_name).
 */
static void
build_gen_proj_target( target_info_t* target, int index )
{
    char vcxproj_path[ BT_PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s/%s.vcxproj", g_build_dir, target->name );

    // Generate a deterministic GUID for this project from its name.
    char guid[ 64 ];
    guid_from_name( target->name, guid );
    ( void )index;

    FILE* f = fopen( vcxproj_path, "w" );
    if ( !f )
    {
        printf( "Error: could not write %s\n", vcxproj_path );
        return;
    }

    // Upper-case the target name for the _STATIC define IntelliSense uses
    // (matches what build_tool_cc.c emits for cl.exe at build time).
    char target_upper[ 128 ];
    get_target_upper( target->name, target_upper );
    write_vcxproj_common_header( f, guid, target->name, target->type, target_upper );

    // Scan the target's source tree recursively so unity sub-files appear in VS.
    g_file_count   = 0;
    g_filter_count = 0;
    scan_directory_recursive( target->root_dir, target->root_dir );

    // Single ItemGroup: units as ClCompile, everything else as ClInclude.
    // This lets VS show all files while only the true compilation units are built.
    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        // Check whether this file is a named compilation unit.
        bool        is_unit  = false;
        const char* filename = strrchr( g_files[ i ].path, '/' );
        if ( !filename ) filename = strrchr( g_files[ i ].path, '\\' );

        if ( filename ) filename++;   // skip past the final slash
        else filename = g_files[ i ].path;

        for ( int j = 0; target->units[ j ]; ++j )
        {
            if ( _stricmp( filename, target->units[ j ] ) == 0 )
            {
                is_unit = true;
                break;
            }
        }

        if ( is_unit )
        {
            // ClCompile items carry NMakeCompileFileCommandLine as a fallback for VS versions
            // that read per-item metadata instead of the ItemDefinitionGroup above.
            fprintf( f, "    <ClCompile Include=\"..\\%s\">\n", g_files[ i ].path );
            fprintf( f, "      <NMakeCompileFileCommandLine>cd .. &amp;&amp; bin\\build_tool.exe -no-deps -compile-only -config $(Configuration) -target %s</NMakeCompileFileCommandLine>\n",target->name );
            fprintf( f, "    </ClCompile>\n" );
        }
        else
        {
            fprintf( f, "    <ClInclude Include=\"..\\%s\" />\n", g_files[ i ].path );
        }
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // Generate the .filters file to mirror the folder structure in Solution Explorer.
    char filters_path[ BT_PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s/%s.vcxproj.filters", g_build_dir, target->name );
    f = fopen( filters_path, "w" );
    if ( f )
    {
        fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
        fprintf( f, "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
        
        // Write virtual filter definitions.
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_filter_count; ++i )
        {
            fprintf( f, "    <Filter Include=\"%s\">\n", g_filters[ i ] );
            fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n", (unsigned int)i );
            fprintf( f, "    </Filter>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
        
        // Map each file to its virtual filter.
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            // Match the tag used in the .vcxproj file.
            bool        is_unit  = false;
            const char* filename = strrchr( g_files[ i ].path, '/' );
            if ( !filename ) filename = strrchr( g_files[ i ].path, '\\' );
            if ( filename ) filename++;
            else filename = g_files[ i ].path;

            for ( int j = 0; target->units[ j ]; ++j )
            {
                if ( _stricmp( filename, target->units[ j ] ) == 0 )
                {
                    is_unit = true;
                    break;
                }
            }

            const char* tag = is_unit ? "ClCompile" : "ClInclude";
            fprintf( f, "    <%s Include=\"..\\%s\">\n", tag, g_files[ i ].path );
            if ( g_files[ i ].filter[ 0 ] != '\0' ) fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
            fprintf( f, "    </%s>\n", tag );
        }
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }
}

/**
 * build_gen_proj_engine_navigation()
 *
 * Emit the "Mega" navigation .vcxproj for a solution. Scans nav_dir
 * recursively and lists EVERY .c/.h as <ClInclude> (never ClCompile) so
 * the developer gets a unified Solution Explorer view for grep/jump/find,
 * but the nav project never competes with a target .vcxproj for any
 * file's IntelliSense TU context. See the long-form comment in the
 * function body for the "why never ClCompile" rationale.
 *
 * `nav_guid` is the per-solution stable GUID computed by the caller via
 * guid_from_name("nav:<sln_name>"), so two solutions opened together
 * don't claim the same project identity.
 */
static void
build_gen_proj_engine_navigation( const char* sln_name, const char* nav_dir, const char* default_target,
                                  const char* nav_guid )
{
    g_file_count   = 0;
    g_filter_count = 0;
    scan_directory_recursive( nav_dir, nav_dir );

    char vcxproj_path[ BT_PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s/%s_nav.vcxproj", g_build_dir, sln_name );
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

    // Unconditional: build commands and shared paths.
    // The nav project exists for IntelliSense / Solution Explorer navigation only.
    // Its Build/Clean are deliberate no-ops: the per-target .vcxproj projects already
    // own their own build and clean, and running a global build_tool.exe here in
    // parallel with them races on shared dirs (e.g. build_new\generated\*).
    ( void )default_target;
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)..\\bin\\</OutDir>\n" );
    fprintf( f, "    <IntDir>$(ProjectDir)%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n", g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>echo [nav] navigation-only project, nothing to build.</NMakeBuildCommandLine>\n" );
    fprintf( f, "    <NMakeOutput>$(ProjectDir)%s\\$(ProjectName)\\$(Configuration)\\nav.stamp</NMakeOutput>\n", g_int_dir );
    fprintf( f, "    <NMakeCleanCommandLine>echo [nav] navigation-only project, nothing to clean.</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakeCompileFile>echo [nav] navigation-only project.</NMakeCompileFile>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)..\\source;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    // Per-config IntelliSense context. No _STATIC define here on purpose:
    // every file in the nav project is listed as ClInclude (see below), so
    // VS does NOT use this project's TU context for any .c file's headers.
    // The per-target .vcxproj's IntelliSense context wins for editing, with
    // the correct <TARGET>_STATIC define for that translation unit.
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;ARCH_X64;_CRT_SECURE_NO_WARNINGS;_DEBUG;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
    fprintf( f, "    <AdditionalOptions>/std:c11 /Zc:preprocessor %%(AdditionalOptions)</AdditionalOptions>\n" );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <NMakePreprocessorDefinitions>OS_WINDOWS;COMPILER_MSVC;ARCH_X64;_CRT_SECURE_NO_WARNINGS;NDEBUG;$(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>\n" );
    fprintf( f, "    <AdditionalOptions>/std:c11 /Zc:preprocessor %%(AdditionalOptions)</AdditionalOptions>\n" );
    fprintf( f, "  </PropertyGroup>\n" );

    // Every file is listed as ClInclude regardless of extension. This is
    // deliberate: the nav project exists for global search and navigation,
    // not for compilation. Listing .c files here as ClCompile would create
    // a competing TU context, and VS picks last-loaded-wins per file —
    // headers would resolve under this empty context instead of the real
    // target's context (wrong defines, wrong API visible). ClInclude has
    // no TU semantics, so the per-target .vcxproj wins cleanly.
    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        fprintf( f, "    <ClInclude Include=\"..\\%s\" />\n", g_files[ i ].path );
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // Generate the .filters file to mirror the folder structure in Solution Explorer.
    char filters_path[ BT_PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s/%s_nav.vcxproj.filters", g_build_dir, sln_name );
    f = fopen( filters_path, "w" );
    if ( f )
    {
        fprintf( f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
        fprintf( f, "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n" );
        
        // Write virtual filter definitions.
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_filter_count; ++i )
        {
            fprintf( f, "    <Filter Include=\"%s\">\n", g_filters[ i ] );
            fprintf( f, "      <UniqueIdentifier>{%08X-0000-0000-0000-000000000000}</UniqueIdentifier>\n", (unsigned int)i );
            fprintf( f, "    </Filter>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
        
        // Map each file to its virtual filter. ClInclude regardless of
        // extension — see comment in the nav .vcxproj writer above.
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            fprintf( f, "    <ClInclude Include=\"..\\%s\">\n", g_files[ i ].path );
            if ( g_files[ i ].filter[ 0 ] != '\0' ) fprintf( f, "      <Filter>%s</Filter>\n", g_files[ i ].filter );
            fprintf( f, "    </ClInclude>\n" );
        }
        fprintf( f, "  </ItemGroup>\n" );
        fprintf( f, "</Project>\n" );
        fclose( f );
    }
}

/**
 * build_gen_solution()
 *
 * Write the .sln descriptor for one entry of g_solutions[]. The .sln is
 * the file the user actually opens in Visual Studio; it references the
 * per-target .vcxproj files (and optionally the nav project) generated
 * elsewhere in this file.
 *
 * Pipeline:
 *   1. Compute nav_guid up front (hash of "nav:<sln_name>") so it can be
 *      reused by every .sln section that references the nav project.
 *   2. If nav_dir is set, emit the nav .vcxproj + register it in the .sln.
 *   3. For each target listed in sln->target_names:
 *      - Emit a "Project(...)" entry with that target's stable GUID.
 *      - Emit ProjectDependencies cross-references so VS's parallel
 *        build scheduler can respect link order. This is the surface
 *        that, combined with -no-deps in each project, makes parallel
 *        VS solution builds safe.
 *      - Track this target's sln_folder so the NestedProjects section
 *        below can nest the project under the right virtual folder.
 *   4. Emit virtual SLN folders ("01_BASE", "02_ENGINE", ...) as separate
 *      Project entries.
 *   5. Emit GlobalSection blocks: configuration mapping (Debug/Release ×
 *      x64) + NestedProjects (puts each target under its folder).
 */
static void
build_gen_solution( solution_info_t* sln )
{
    char sln_path[ BT_PATH_MAX ];
    snprintf( sln_path, sizeof( sln_path ), "%s/%s.sln", g_build_dir, sln->name );
    FILE* f = fopen( sln_path, "w" );
    if ( !f ) return;

    fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
    fprintf( f, "# Visual Studio Version 17\n" );

    const char* folder_type_guid = "{2150E333-8FDC-42A3-9474-1A3956D46DE8}";
    const char* cpp_type_guid    = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

    // Each solution's nav project gets its own stable GUID derived from
    // the sln name, so two solutions opened side-by-side don't collide.
    char nav_guid[ 64 ] = { 0 };
    {
        char key[ 128 ];
        snprintf( key, sizeof( key ), "nav:%s", sln->name );
        guid_from_name( key, nav_guid );
    }

    // 1. Add Navigation Project if requested.
    if ( sln->nav_dir )
    {
        // We assume the first target in the list is the "primary" one for NMakeOutput.
        const char* default_target = (sln->target_names && sln->target_names[ 0 ]) ? sln->target_names[ 0 ] : "unknown";
        build_gen_proj_engine_navigation( sln->name, sln->nav_dir, default_target, nav_guid );
        fprintf( f, "Project(\"%s\") = \"%s_nav\", \"%s_nav.vcxproj\", \"%s\"\n",
                cpp_type_guid, sln->name, sln->name, nav_guid );
        fprintf( f, "EndProject\n" );
    }

    // 2. Add Target Projects.
    char  folders[ 16 ][ BT_PATH_MAX ];
    char  folder_guids[ 16 ][ 64 ];
    int   folder_count = 0;

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
            fprintf( f, "Project(\"%s\") = \"%s\", \"%s.vcxproj\", \"%s\"\n",
                    cpp_type_guid, target->name, target->name, guid );

            // --- Project Dependencies ---
            // This section tells Visual Studio's scheduler exactly which projects
            // must be finished before starting this one. This prevents race conditions
            // where multiple cl.exe instances try to write to the same PDB.
            if ( target->deps[ 0 ] || target->tool_deps[ 0 ] || target->has_reflect )
            {
                fprintf( f, "\tProjectSection(ProjectDependencies) = postProject\n" );

                // Add Link Dependencies (libs).
                for ( int i = 0; target->deps[ i ]; ++i )
                {
                    char dep_guid[ 64 ];
                    guid_from_name( target->deps[ i ], dep_guid );
                    fprintf( f, "\t\t%s = %s\n", dep_guid, dep_guid );
                }

                // Add Tool Dependencies (exes).
                for ( int i = 0; target->tool_deps[ i ]; ++i )
                {
                    char tool_guid[ 64 ];
                    guid_from_name( target->tool_deps[ i ], tool_guid );
                    fprintf( f, "\t\t%s = %s\n", tool_guid, tool_guid );
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

            // Collect and track SLN folders for nesting later.
            bool found = false;
            for ( int j = 0; j < folder_count; ++j )
            {
                if ( strcmp( folders[ j ], target->sln_folder ) == 0 ) { found = true; break; }
            }
            if ( !found && folder_count < 16 )
            {
                snprintf( folders[ folder_count ], BT_PATH_MAX, "%s", target->sln_folder );
                // Folder GUID is per-(solution, folder) — same folder name in a
                // different solution stays distinct, but is stable across regens.
                char key[ 192 ];
                snprintf( key, sizeof( key ), "folder:%s:%s", sln->name, target->sln_folder );
                guid_from_name( key, folder_guids[ folder_count ] );
                folder_count++;
            }
        }
    }

    // 3. Add Virtual SLN Folders.
    for ( int i = 0; i < folder_count; ++i )
    {
        fprintf( f, "Project(\"%s\") = \"%s\", \"%s\", \"%s\"\n", 
                folder_type_guid, folders[ i ], folders[ i ], folder_guids[ i ] );
        fprintf( f, "EndProject\n" );
    }

    // 4. Global Section (Configuration Mapping and Project Nesting).
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

    // Nest projects into the virtual SLN folders we created.
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

/**
 * build_gen_projects()
 *
 * Top-level entry point invoked by `build_tool.exe -gen`. Regenerates
 * every .vcxproj and every .sln from the current target/solution registry.
 *
 * Two-phase emission:
 *   1. One .vcxproj per target (shared across solutions that reference it).
 *   2. One .sln per solution descriptor in g_solutions[].
 *
 * Safe to re-run anytime; the generated XML is fully deterministic given
 * the registry contents, so VS user state survives regen as long as the
 * target name doesn't change (see guid_from_name).
 */
void
build_gen_projects( void )
{
    printf( "Generating Visual Studio projects in %s/...\n", g_build_dir );

    // Make sure the output directory exists.
    ensure_dir( g_build_dir );

    // 1. Generate every target's .vcxproj once. They're shared across
    //    solutions by name — a single base.vcxproj is referenced by
    //    every .sln that lists "base" in its target_names.
    for ( int i = 0; i < g_target_count; ++i )
    {
        build_gen_proj_target( &g_targets[ i ], i );
    }

    // 2. Generate each .sln in the registry. Each call also writes its
    //    own nav .vcxproj if nav_dir is set on the descriptor.
    for ( int i = 0; i < g_solution_count; ++i )
    {
        printf( "Generating Solution: %s.sln\n", g_solutions[ i ].name );
        build_gen_solution( &g_solutions[ i ] );
    }

    printf( "\nProjects generated successfully in %s/.\n", g_build_dir );
}

/*============================================================================================*/
