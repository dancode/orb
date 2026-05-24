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

/*==============================================================================================
    --- Per-Solution Path State ---

    Set once at the top of each solution's generation pass in build_gen_projects().
    All emitter functions read these instead of hardcoding depth-1 relative paths,
    so moving a solution's output directory only requires changing out_dir in the
    solution descriptor -- no string edits anywhere else in this file.

    s_out_dir     -- where .sln and .vcxproj files land  (e.g. "build\\proj")
    s_root_prefix -- "../..\\" back to the project root   (e.g. "..\\..\\")
    s_cd_root     -- "cd ..\\.." for NMake commands       (e.g. "..\\..")
==============================================================================================*/

static const char* s_out_dir      = "build";
static char        s_root_prefix[ 32 ] = "..\\";
static char        s_cd_root     [ 32 ] = "..";
static bool        s_is_monolithic     = false;

static void
compute_path_parts( const char* out_dir )
{
    // Count directory components: "build\\proj" has one separator -> depth 2.
    int depth = 1;
    for ( const char* p = out_dir; *p; ++p )
        if ( *p == '\\' || *p == '/' ) depth++;

    s_out_dir = out_dir;

    s_root_prefix[ 0 ] = '\0';
    for ( int i = 0; i < depth; ++i )
        strcat( s_root_prefix, "..\\" );

    // cd_root is the same path without the trailing backslash, used in
    // NMake commands: "cd ..\\.." rather than "cd ..\\..\\"
    s_cd_root[ 0 ] = '\0';
    strcat( s_cd_root, ".." );
    for ( int i = 1; i < depth; ++i )
        strcat( s_cd_root, "\\.." );
}

// Metadata for one source file picked up by scan_directory_recursive().
// Drives both the <ItemGroup> entries in the .vcxproj and the matching
// <Filter> mappings in the .filters file.
typedef struct
{
    char path[ PATH_MAX ];   // Relative path from project root.
    char filter[ PATH_MAX ]; // Virtual folder path in VS (e.g. "engine\\core").
    bool is_header;             // True for .h files; influences ClInclude vs ClCompile choice.
} file_info_t;

// Per-scan buffers. Reset (g_file_count = 0, g_filter_count = 0) at the
// start of every project emission -- they are reusable scratch, not state.
static file_info_t g_files[ MAX_FILES ];
static int         g_file_count = 0;

static char g_filters[ MAX_FILTERS ][ PATH_MAX ];
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
 * of the name -- registry reorderings cost nothing, renames cost only their
 * own project's identity (which is correct: a rename is a new project).
 *
 * Two FNV-1a passes with different seeds and primes fill the 16 bytes.
 * FNV is not cryptographic, but we are not defending against collisions
 * from an adversary -- we just need 2^128 worth of spread across the few
 * dozen names this tool will ever see.
 */
static void
guid_from_name( const char* name, char* out )
{
    // Two independent FNV-1a hashes. FNV-1a iterates byte-by-byte over the
    // input, XOR-ing the byte into the accumulator and then multiplying by a
    // large prime. Using two different (seed, prime) pairs produces 128 bits
    // of independent output -- enough spread for our few-dozen project names.
    //
    // The exact constants are the standard 64-bit FNV-1a values (h1) and an
    // arbitrary second pair (h2) chosen for being prime and distinct. The
    // specific values don't matter as long as they stay stable across runs;
    // changing them would re-roll every GUID and detach VS user state.
    unsigned long long h1 = 0xcbf29ce484222325ULL;
    unsigned long long h2 = 0x9ae16a3b2f90404fULL;
    for ( const unsigned char* p = ( const unsigned char* )name; *p; ++p )
    {
        h1 = ( h1 ^ *p ) * 0x100000001b3ULL;
        h2 = ( h2 ^ *p ) * 0x880355f21e6d1965ULL;
    }

    // Format the two 64-bit accumulators into the canonical 8-4-4-4-12
    // GUID layout VS expects: {AAAAAAAA-BBBB-CCCC-DDDD-EEEEFFFFFFFF}.
    // Output is exactly 38 bytes + NUL; caller's buffer is 64.
    //
    // Bit layout -- we slice h1 and h2 to fill each field:
    //   h1[63:32] -> AAAAAAAA  (top half of h1)
    //   h1[31:16] -> BBBB
    //   h1[15:0]  -> CCCC      (bottom half of h1)
    //   h2[63:48] -> DDDD      (top of h2)
    //   h2[47:32] -> EEEE
    //   h2[31:0]  -> FFFFFFFF  (bottom half of h2)
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
 * exist on disk -- or, in our case, that mirror disk layout but live in a
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
 * This is necessary for VS to display the nested folder structure correctly:
 * if only the leaf folder is registered, Solution Explorer skips the parent
 * tiers and the hierarchy collapses.
 *
 * Trick: instead of building each prefix in a fresh buffer, we walk the path
 * in place and temporarily null-terminate at each backslash to "fake" the
 * prefix string, register it, then restore the backslash. Cheap and avoids
 * any extra copies.
 */
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
            // Pretend the path ends here, register the prefix as a filter,
            // then put the separator back so we keep walking the full path.
            *p = '\0';
            add_filter( tmp );
            *p = '\\';
        }
        p++;
    }
    // Don't forget the leaf -- the loop above only registered prefixes ending
    // in a separator, not the final segment.
    add_filter( tmp );
}

/**
 * get_filter_for_path()
 *
 * Maps a physical file path to a virtual VS filter path.
 * Example: path = "source/engine/core/core.c", root_dir = "source/engine"
 *          -> out_filter = "core"
 * Example: path = "source/engine/core/win/io.c", root_dir = "source/engine"
 *          -> out_filter = "core\\win"
 *
 * Strategy: strip the project root prefix, drop the final filename, and
 * normalize the surviving directory portion to use backslashes (which is
 * what VS uses inside its filter XML).
 *
 * Files that sit directly under root_dir produce an empty filter ("") --
 * those go in the project's root with no virtual folder.
 */
static void
get_filter_for_path( const char* path, const char* root_dir, char* out_filter )
{
    out_filter[ 0 ] = '\0';

    // Reject anything that does not start with root_dir; we have no meaningful
    // filter to compute for it.
    size_t root_len = strlen( root_dir );
    if ( strncmp( path, root_dir, root_len ) != 0 ) return;

    // Step past the root prefix and any single separator that follows it, so
    // `sub` points to the relative tail we want to project into a filter.
    // E.g. for path "source/engine/core/io.c" and root "source/engine",
    // sub now points at "core/io.c".
    const char* sub = path + root_len;
    if ( *sub == '/' || *sub == '\\' ) sub++;

    // Find the last separator -- everything before it is the directory portion
    // (which becomes the filter), everything after is the filename (discarded).
    // We check '/' first since our paths are mostly forward-slash, but fall
    // back to '\\' for paths that came from the Win32 _findfirst APIs.
    const char* last_slash = strrchr( sub, '/' );
    if ( !last_slash ) last_slash = strrchr( sub, '\\' );

    // No separator after the root means the file lives directly under
    // root_dir -> empty filter (already set above), nothing more to do.
    if ( !last_slash ) return;

    // Copy the directory portion into out_filter, then convert to backslashes
    // for the VS filter convention. strncpy doesn't null-terminate when it
    // hits its cap, so the explicit `out_filter[len] = '\0'` is required.
    size_t len = last_slash - sub;
    strncpy( out_filter, sub, len );
    out_filter[ len ] = '\0';
    for ( char* p = out_filter; *p; ++p )
        if ( *p == '/' ) *p = '\\';
}

/**
 * scan_directory_recursive()
 *
 * Walks a directory tree starting at `dir` and accumulates every .c and .h
 * file it finds into the shared g_files[] buffer, computing each file's VS
 * filter (virtual folder) relative to `root_dir`.
 *
 * `dir` advances as we recurse into subdirectories; `root_dir` stays fixed
 * so the filter calculation is always relative to the original scan root.
 */
static void
scan_directory_recursive( const char* dir, const char* root_dir )
{
    // _findfirst expects a wildcard pattern; "<dir>/*" matches everything in
    // this directory (files + subdirectories + the . / .. specials).
    char search_path[ PATH_MAX ];
    snprintf( search_path, sizeof( search_path ), "%s\\*", dir );

    struct _finddata_t find_data;
    intptr_t           handle = _findfirst( search_path, &find_data );
    if ( handle == -1 ) return;        // Empty / nonexistent directory.

    do
    {
        // _findfirst always includes the current ("." ) and parent ("..")
        // directory entries -- skip them or we'd recurse infinitely.
        if ( strcmp( find_data.name, "." ) == 0 || strcmp( find_data.name, ".." ) == 0 ) continue;

        // Reconstruct the full path so we can either recurse into it or store it.
        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "%s\\%s", dir, find_data.name );

        if ( find_data.attrib & _A_SUBDIR )
        {
            // Subdirectory -> recurse. root_dir stays the same.
            scan_directory_recursive( path, root_dir );
        }
        else
        {
            // File -> only care about .c and .h. strrchr finds the LAST dot,
            // which is the extension marker. Files with no dot (e.g.
            // "Makefile") get ext == NULL and are silently skipped.
            const char* ext = strrchr( find_data.name, '.' );
            if ( !ext ) continue;

            bool is_c = _stricmp( ext, ".c" ) == 0;
            bool is_h = _stricmp( ext, ".h" ) == 0;
            if ( !( is_c || is_h ) ) continue;
            if ( g_file_count >= MAX_FILES ) continue;   // Hard cap; bigger trees would silently drop.

            // Record the file plus its filter (virtual folder). Register every
            // ancestor filter too so the nesting renders cleanly in Solution
            // Explorer (see add_filters_recursive for why).
            file_info_t* f = &g_files[ g_file_count++ ];
            strcpy( f->path, path );
            f->is_header = is_h;
            get_filter_for_path( path, root_dir, f->filter );
            if ( f->filter[ 0 ] != '\0' ) add_filters_recursive( f->filter );
        }
    }
    while ( _findnext( handle, &find_data ) == 0 );

    _findclose( handle );
}

/**
 * build_intellisense_defines()
 *
 * Builds the semicolon-separated NMakePreprocessorDefinitions value from the
 * shared define tables in build_tool_targets.c. Both the per-target and the
 * nav project vcxproj callers use this so neither can silently diverge from
 * what cc_fill_compile_cmd passes to cl.exe.
 *
 * target: the target being projected, or NULL for the nav project (no _STATIC
 *         chain -- the nav project lists every file as ClInclude and never
 *         claims TU ownership, so the per-target vcxproj's defines win).
 */
static void
build_intellisense_defines( char* buf, size_t buf_size, config_t config, target_info_t* target )
{
    buf[ 0 ] = '\0';
    size_t used = 0;

#define ISDEF_APPEND( s )                                               \
    do {                                                                \
        size_t slen = strlen( s );                                      \
        if ( used + slen + 2 < buf_size ) {                            \
            if ( used ) buf[ used++ ] = ';';                            \
            memcpy( buf + used, s, slen );                              \
            used += slen;                                               \
            buf[ used ] = '\0';                                         \
        }                                                               \
    } while ( 0 )

    for ( int i = 0; g_defines_always[ i ]; ++i )
        ISDEF_APPEND( g_defines_always[ i ] );

    const char** cfg = ( config == CONFIG_DEBUG ) ? g_defines_debug : g_defines_release;
    for ( int i = 0; cfg[ i ]; ++i )
        ISDEF_APPEND( cfg[ i ] );

    if ( target )
    {
        // <TARGET>_STATIC: same derivation as cc_fill_compile_cmd.
        char upper[ 128 ];
        get_target_upper( target->name, upper, sizeof( upper ) );
        char define[ 160 ];
        snprintf( define, sizeof( define ), "%s_STATIC", upper );
        ISDEF_APPEND( define );

        // <DEP>_STATIC for each static dep (and DLL deps in monolithic mode) --
        // mirrors the dep loop in cc_fill_compile_cmd so the IntelliSense API
        // gateway selection matches the actual build.
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            target_info_t* dep = find_target( target->deps[ i ] );
            if ( !dep ) continue;
            if ( dep->type == TARGET_STATIC_LIB || ( dep->type == TARGET_DYNAMIC_LIB && s_is_monolithic ) )
            {
                char dep_upper[ 128 ];
                get_target_upper( dep->name, dep_upper, sizeof( dep_upper ) );
                snprintf( define, sizeof( define ), "%s_STATIC", dep_upper );
                ISDEF_APPEND( define );
            }
        }
        if ( s_is_monolithic ) ISDEF_APPEND( "BUILD_STATIC" );
    }

#undef ISDEF_APPEND
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
 *     actually feed IntelliSense -- preprocessor defines, include paths, and
 *     AdditionalOptions. IntelliSense reads these to construct the TU
 *     context for headers and source. Splitting them per-config lets _DEBUG
 *     vs NDEBUG, and any future config-specific defines, diverge without
 *     re-emitting the rest of the property block.
 *
 * /std:c11 and /Zc:preprocessor are passed via AdditionalOptions so the
 * IntelliSense parser matches what cl.exe actually does for the build.
 *
 * target: the target being projected. Drives the _STATIC define chain via
 *         build_intellisense_defines(). NULL for the nav project.
 */
static void
write_vcxproj_common_header( FILE* f, const char* guid, const char* out_name,
                             target_type_t type, target_info_t* target )
{
    const char* ext = ".exe";
    if ( type == TARGET_STATIC_LIB ) ext = ".lib";
    if ( type == TARGET_DYNAMIC_LIB ) ext = s_is_monolithic ? ".lib" : ".dll";

    // Appended to build/compile commands so VS drives the correct mode.
    // Clean is mode-agnostic: it wipes all artifacts regardless of how they were built.
    const char* mono_flag = s_is_monolithic ? " -monolithic" : "";

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
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n", s_root_prefix, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -config $(Configuration) -target %s%s</NMakeBuildCommandLine>\n", s_cd_root, out_name, mono_flag );
    fprintf( f, "    <NMakeOutput>%sbin\\%s%s</NMakeOutput>\n", s_root_prefix, out_name, ext );
    fprintf( f, "    <NMakeCleanCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -clean -target %s</NMakeCleanCommandLine>\n", s_cd_root, out_name );
    fprintf( f, "    <NMakeCompileFile>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -config $(Configuration) -target %s%s</NMakeCompileFile>\n", s_cd_root, out_name, mono_flag );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n", s_root_prefix );
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
    fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n", s_cd_root, out_name, mono_flag );
    fprintf( f, "    </NMakeCompile>\n" );
    fprintf( f, "  </ItemDefinitionGroup>\n" );

    // --- IntelliSense context (Debug|x64 and Release|x64) ---
    // Defines and AdditionalOptions are derived from the shared tables in
    // build_tool_targets.c -- the same source cc_fill_compile_cmd uses.
    {
        char  additional_opts[ 256 ] = { 0 };
        for ( int i = 0; g_intellisense_flags[ i ]; ++i )
        {
            if ( i ) strcat( additional_opts, " " );
            strcat( additional_opts, g_intellisense_flags[ i ] );
        }

        char dbg_defines[ 1024 ];
        char rel_defines[ 1024 ];
        build_intellisense_defines( dbg_defines, sizeof( dbg_defines ), CONFIG_DEBUG,   target );
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

    // --- Debugger working directory ---
    // Isolated in their own PropertyGroups so VS does not conflate these with
    // IntelliSense or build settings, which would grey out per-file compile.
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">\n" );
    fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_cd_root );
    fprintf( f, "  </PropertyGroup>\n" );
    fprintf( f, "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">\n" );
    fprintf( f, "    <LocalDebuggerWorkingDirectory>$(ProjectDir)%s</LocalDebuggerWorkingDirectory>\n", s_cd_root );
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
 * call sites -- the GUID derivation no longer uses it (see guid_from_name).
 */
static void
build_gen_proj_target( target_info_t* target, int index )
{
    char vcxproj_path[ PATH_MAX ];
    snprintf( vcxproj_path, sizeof( vcxproj_path ), "%s\\%s.vcxproj", s_out_dir, target->name );

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

    write_vcxproj_common_header( f, guid, target->name, target->type, target );

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
        // Scan for the last separator of either kind to handle mixed-slash paths.
        bool        is_unit  = false;
        const char* filename = g_files[ i ].path;
        for ( const char* p = g_files[ i ].path; *p; ++p )
            if ( *p == '/' || *p == '\\' ) filename = p + 1;

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
            const char* item_mono = s_is_monolithic ? " -monolithic" : "";
            fprintf( f, "    <ClCompile Include=\"%s%s\">\n", s_root_prefix, g_files[ i ].path );
            fprintf( f, "      <NMakeCompileFileCommandLine>cd %s &amp;&amp; bin\\build_tool.exe -no-deps -compile-only -config $(Configuration) -target %s%s</NMakeCompileFileCommandLine>\n", s_cd_root, target->name, item_mono );
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

    // Generate the .filters file to mirror the folder structure in Solution Explorer.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s.vcxproj.filters", s_out_dir, target->name );
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
            fprintf( f, "    <%s Include=\"%s%s\">\n", tag, s_root_prefix, g_files[ i ].path );
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

    // Unconditional: build commands and shared paths.
    // The nav project exists for IntelliSense / Solution Explorer navigation only.
    // Its Build/Clean are deliberate no-ops: the per-target .vcxproj projects already
    // own their own build and clean, and running a global build_tool.exe here in
    // parallel with them races on shared dirs (e.g. build_new\generated\*).
    ( void )default_target;
    fprintf( f, "  <PropertyGroup>\n" );
    fprintf( f, "    <OutDir>$(ProjectDir)%sbin\\</OutDir>\n", s_root_prefix );
    fprintf( f, "    <IntDir>$(ProjectDir)%s%s\\$(ProjectName)\\$(Configuration)\\</IntDir>\n", s_root_prefix, g_int_dir );
    fprintf( f, "    <NMakeBuildCommandLine>echo       [nav] navigation-only project, nothing to build.</NMakeBuildCommandLine>\n" );
    fprintf( f, "    <NMakeOutput>$(ProjectDir)%s%s\\$(ProjectName)\\$(Configuration)\\nav.stamp</NMakeOutput>\n", s_root_prefix, g_int_dir );
    fprintf( f, "    <NMakeCleanCommandLine>echo       [nav] navigation-only project, nothing to clean.</NMakeCleanCommandLine>\n" );
    fprintf( f, "    <NMakeCompileFile>echo       [nav] navigation-only project.</NMakeCompileFile>\n" );
    fprintf( f, "    <NMakeIncludeSearchPath>$(ProjectDir)%ssource;$(NMakeIncludeSearchPath)</NMakeIncludeSearchPath>\n", s_root_prefix );
    fprintf( f, "  </PropertyGroup>\n" );

    // Per-config IntelliSense context. NULL target: nav lists all files as
    // ClInclude so the per-target vcxproj's defines win for editing -- no
    // _STATIC chain needed here.
    {
        char additional_opts[ 256 ] = { 0 };
        for ( int i = 0; g_intellisense_flags[ i ]; ++i )
        {
            if ( i ) strcat( additional_opts, " " );
            strcat( additional_opts, g_intellisense_flags[ i ] );
        }

        char dbg_defines[ 1024 ];
        char rel_defines[ 1024 ];
        build_intellisense_defines( dbg_defines, sizeof( dbg_defines ), CONFIG_DEBUG,   NULL );
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

    // Every file is listed as ClInclude regardless of extension. This is
    // deliberate: the nav project exists for global search and navigation,
    // not for compilation. Listing .c files here as ClCompile would create
    // a competing TU context, and VS picks last-loaded-wins per file --
    // headers would resolve under this empty context instead of the real
    // target's context (wrong defines, wrong API visible). ClInclude has
    // no TU semantics, so the per-target .vcxproj wins cleanly.
    fprintf( f, "  <ItemGroup>\n" );
    for ( int i = 0; i < g_file_count; ++i )
    {
        fprintf( f, "    <ClInclude Include=\"%s%s\" />\n", s_root_prefix, g_files[ i ].path );
    }
    fprintf( f, "  </ItemGroup>\n" );

    fprintf( f, "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n" );
    fprintf( f, "</Project>\n" );
    fclose( f );

    // Generate the .filters file to mirror the folder structure in Solution Explorer.
    char filters_path[ PATH_MAX ];
    snprintf( filters_path, sizeof( filters_path ), "%s\\%s_nav.vcxproj.filters", s_out_dir, sln_name );
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
        // extension -- see comment in the nav .vcxproj writer above.
        fprintf( f, "  <ItemGroup>\n" );
        for ( int i = 0; i < g_file_count; ++i )
        {
            fprintf( f, "    <ClInclude Include=\"%s%s\">\n", s_root_prefix, g_files[ i ].path );
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
 *   5. Emit GlobalSection blocks: configuration mapping (Debug/Release x
 *      x64) + NestedProjects (puts each target under its folder).
 */
static void
build_gen_solution( solution_info_t* sln )
{
    char sln_path[ PATH_MAX ];
    snprintf( sln_path, sizeof( sln_path ), "%s\\%s.sln", s_out_dir, sln->name );
    FILE* f = fopen( sln_path, "w" );
    if ( !f ) return;

    fprintf( f, "\nMicrosoft Visual Studio Solution File, Format Version 12.00\n" );
    fprintf( f, "# Visual Studio Version 17\n" );

    // Microsoft-defined "kind" GUIDs that .sln entries use as a discriminator.
    // These are FIXED by Visual Studio -- do not regenerate them.
    //   folder_type_guid -> declares an entry as a virtual solution folder.
    //   cpp_type_guid    -> declares an entry as a C/C++ project (.vcxproj).
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
    char  folders[ 16 ][ PATH_MAX ];
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
            if ( target->deps[ 0 ] || target->tool_deps[ 0 ] || target->has_reflect || !target->is_build_tool )
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

                // Implicit dep: if build_tool is in this solution, every other target
                // depends on it -- its NMake command invokes bin\build_tool.exe, so a
                // parallel Rebuild All would race if build_tool hasn't linked yet.
                // Only injected when build_tool is actually a project in this solution
                // (e.g. orb_build.sln) so other solutions (orb_make.sln) are unaffected.
                if ( !target->is_build_tool )
                {
                    bool bt_in_sln = false;
                    for ( const char** tn2 = sln->target_names; *tn2; ++tn2 )
                        if ( strcmp( *tn2, "build_tool" ) == 0 ) { bt_in_sln = true; break; }

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

            // Collect and track SLN folders for nesting later.
            bool found = false;
            for ( int j = 0; j < folder_count; ++j )
            {
                if ( strcmp( folders[ j ], target->sln_folder ) == 0 ) { found = true; break; }
            }
            if ( !found && folder_count < 16 )
            {
                snprintf( folders[ folder_count ], PATH_MAX, "%s", target->sln_folder );
                // Folder GUID is per-(solution, folder) -- same folder name in a
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

/*==============================================================================================
    --- Build Generate Projects ---

    Top-level entry point invoked by `build_tool.exe -gen`. Regenerates every
    .vcxproj and every .sln from the current target/solution registry.

    Per-solution emission: each solution owns its output directory (sln->out_dir)
    and gets its own set of .vcxproj files generated there. This allows different
    solutions to have different IntelliSense defines (e.g. modular vs monolithic)
    without sharing project files. The file-scope path state (s_out_dir,
    s_root_prefix, s_cd_root) is set per-solution so all emitter functions
    produce correct relative paths without receiving extra parameters.

    Safe to re-run anytime; the generated XML is fully deterministic given
    the registry contents, so VS user state survives regen as long as the
    target name doesn't change (see guid_from_name).
==============================================================================================*/

void
build_gen_projects( void )
{
    for ( int i = 0; i < g_solution_count; ++i )
    {
        solution_info_t* sln = &g_solutions[ i ];

        // Set file-scope state for this solution. All emitter functions read
        // these directly so paths and build flags are correct for this pass.
        compute_path_parts( sln->out_dir );
        s_is_monolithic = sln->is_monolithic;
        ensure_dir( sln->out_dir );

        printf( "Generating Solution '%s' in %s/...\n", sln->name, sln->out_dir );

        // Generate a .vcxproj for each target in this solution.
        // Each solution owns its vcxproj files -- they are NOT shared across solutions
        // so that per-solution IntelliSense properties (defines, flags) can differ.
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
