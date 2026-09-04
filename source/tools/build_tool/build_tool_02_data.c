/*==============================================================================================

    build_tool_02_data.c -- Build inputs (targets + solutions) + helper functions.
    
    The in-memory representation of the build graph as parsed from orb.targets.    

    At startup, main() calls (in this order):

        registry_load( "orb.targets" )

        --  appended by build_tool_03_registry.c; all project targets and solutions
            live there. If orb.targets contains 'engine <path>', this sets
            g_engine_root before returning.

        init_builtin_targets()

        --  registers build_tool, reflect_tool and res_tool into g_targets[].
            If g_engine_root is set, paths and is_external are derived from it;
            otherwise CWD-relative paths are used (engine-root build).

    g_engine_root:

        Empty string at startup. Set by registry_load() when it encounters the
        'engine <path>' directive in orb.targets. Used by init_builtin_targets()
        and the compile/gen modules to auto-add engine header search paths.

    Why build_tool, reflect_tool and res_tool are hard-coded here and not in orb.targets:

        - build_tool.exe needs to be able to bootstrap itself (-bootstrap flag) even
          if orb.targets is missing or malformed.
        - reflect_tool is an immediate dependency of the bootstrap path (core etc.
          need it), so it must always be resolvable.
        - res_tool is an implicit dependency of every executable and every dynamic
          module, including those of a child project that only imports the engine's
          targets, so it must resolve the same way.
        - Every other target can be added or edited in orb.targets without touching
          or recompiling build_tool.c.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Engine Root ---

    Absolute path to the engine installation. Empty when build_tool is running at the
    engine root itself. Set by registry_load() when it encounters 'engine <path>'.
    Read by init_builtin_targets(), cc_fill_compile_cmd(), and the gen modules.
==============================================================================================*/

char            g_engine_root[ PATH_MAX ] = { 0 };

/*==============================================================================================
    --- Dynamic Target and Solution Pools ---
==============================================================================================*/

target_info_t   g_targets[ MAX_TARGETS ];
int             g_target_count = 0;

solution_info_t g_solutions[ MAX_SOLUTIONS ];
int             g_solution_count = 0;

/*==============================================================================================
    --- String Pool ---

    Bump-allocator for strings parsed from orb.targets. Lives for the process
    lifetime -- no free() needed. Built-in target strings are C literals and do
    not consume pool space.
==============================================================================================*/

#define STR_POOL_SIZE ( 32 * 1024 )

static char g_str_pool[ STR_POOL_SIZE ];
static int  g_str_pool_used = 0;

static const char*
pool_str( const char* s )
{
    if ( !s || !s[ 0 ] )
        return NULL;

    int len = (int)strlen( s ) + 1;
    if ( g_str_pool_used + len > STR_POOL_SIZE )
    {
        printf( ORB_INDENT "[orb error] string pool exhausted; raise STR_POOL_SIZE\n" );
        exit( 1 );
    }
    char* out = g_str_pool + g_str_pool_used;
    memcpy( out, s, (size_t)len );
    g_str_pool_used += len;
    return out;
}

/*==============================================================================================
    --- Built-in Target Registration ---

    Registers the three targets that must always be present:
        build_tool   -- the build orchestrator (this executable).
        reflect_tool -- the reflection code-generator.
        res_tool     -- the resource-reference harvester.

    Called once in main() after registry_load(), so g_engine_root is already set.
==============================================================================================*/

static void
init_builtin_targets( void )
{
    // If 'engine <path>' was declared in orb.targets, g_engine_root is set and built-ins
    // belong to the engine installation, not this project. Paths come from the engine root;
    // is_external excludes them from "build all", gen, and clean.
    bool is_external = ( g_engine_root[ 0 ] != '\0' );

    char bt_root[ PATH_MAX ];
    char rt_root[ PATH_MAX ];
    char rs_root[ PATH_MAX ];
    if ( is_external )
    {
        snprintf( bt_root, sizeof( bt_root ), "%s/source/tools/build_tool",  g_engine_root );
        snprintf( rt_root, sizeof( rt_root ), "%s/source/tools/reflect_tool", g_engine_root );
        snprintf( rs_root, sizeof( rs_root ), "%s/source/tools/res_tool",     g_engine_root );
    }
    else
    {
        snprintf( bt_root, sizeof( bt_root ), "source/tools/build_tool" );
        snprintf( rt_root, sizeof( rt_root ), "source/tools/reflect_tool" );
        snprintf( rs_root, sizeof( rs_root ), "source/tools/res_tool" );
    }

    // build_tool: the build orchestrator itself.
    {
        target_info_t* t = &g_targets[ g_target_count++ ];
        memset( t, 0, sizeof( *t ) );
        t->name          = "build_tool";
        t->type          = TARGET_EXECUTABLE;
        t->has_type      = true;
        t->root_dir      = pool_str( bt_root );
        t->virtual_folder    = "08_TOOL";
        t->units[ 0 ]    = "build_tool.c";
        t->is_build_tool = true;
        t->is_tool       = true;
        t->is_external   = is_external;

        /*  Both settings exist to keep an unsigned, freshly-built .exe out of EDR heuristics.

            /guard:cf on compiler and linker alike writes a CFG bitmap into the PE header,
            which EDRs read as a positive trust signal (marks the binary as memory-safe).

            has_win_resources links build_tool.rc (publisher metadata) and embeds
            build_tool.manifest (DPI awareness, supported OS list, asInvoker), both of which
            live in root_dir beside the sources. bootstrap_build_tool.bat compiles and embeds
            the same two files by hand for the exe it produces before any build_tool exists. */

        t->has_win_resources = true;

        t->extra_compile_flags[ 0 ].compiler = COMPILE_MSVC;
        snprintf( t->extra_compile_flags[ 0 ].flag,
                  sizeof( t->extra_compile_flags[ 0 ].flag ), "/guard:cf" );
        t->extra_compile_flag_count = 1;

        t->extra_link_flags[ 0 ].compiler = COMPILE_MSVC;
        snprintf( t->extra_link_flags[ 0 ].flag,
                  sizeof( t->extra_link_flags[ 0 ].flag ), "/guard:cf" );
        t->extra_link_flag_count = 1;
    }

    // reflect_tool: the reflection code generator.
    {
        target_info_t* t = &g_targets[ g_target_count++ ];
        memset( t, 0, sizeof( *t ) );
        t->name            = "reflect_tool";
        t->type            = TARGET_EXECUTABLE;
        t->has_type        = true;
        t->root_dir        = pool_str( rt_root );
        t->virtual_folder      = "08_TOOL";
        t->units[ 0 ]      = "reflect_tool.c";
        t->is_tool         = true;
        t->is_reflect_tool = true;
        t->is_external     = is_external;
    }

    // res_tool: the resource-reference harvester (RID / RES_TREE tokens -> res manifests).
    {
        target_info_t* t = &g_targets[ g_target_count++ ];
        memset( t, 0, sizeof( *t ) );
        t->name            = "res_tool";
        t->type            = TARGET_EXECUTABLE;
        t->has_type        = true;
        t->root_dir        = pool_str( rs_root );
        t->virtual_folder  = "08_TOOL";
        t->units[ 0 ]      = "res_tool.c";
        t->is_tool         = true;
        t->is_res_tool     = true;
        t->is_external     = is_external;
    }
}

/*==============================================================================================
    --- Warning Suppression Table ---

    Applied globally after the base flag set in build_target_compile().
    Each entry fires only when the active config AND compiler match.
==============================================================================================*/

warn_suppress_t g_warn_suppressions[] = {

    // Unused parameters: suppressed in both compilers. Use UNUSED() at each call site
    // when a parameter is intentionally unused -- don't rely on this global suppression.

    { "/wd4100",                    CONFIG_COUNT,   COMPILE_MSVC  },  // C4100: unreferenced formal parameter
    { "-Wno-unused-parameter",      CONFIG_COUNT,   COMPILE_CLANG },  // clang equivalent of C4100

    { "-Wno-unused-variable",          CONFIG_RELEASE, COMPILE_CLANG },  // debug variables hanging around
    { "-Wno-unused-but-set-variable",  CONFIG_RELEASE, COMPILE_CLANG },  // debug variables hanging around

    // Variables used only in debug assertions compile away in Release, leaving them
    // unreferenced. Suppressed only for Release so Debug still catches real dead vars.

    { "/wd4101",                    CONFIG_RELEASE, COMPILE_MSVC  },  // C4101: unreferenced local variable
    { "/wd4189",                    CONFIG_RELEASE, COMPILE_MSVC  },  // C4189: initialized but not referenced

    // clang-cl: suppress spurious "linker input unused" when the toolchain passes extra
    // arguments that clang doesn't consume (e.g. response-file edge cases).

    { "-Wno-unused-command-line-argument", CONFIG_COUNT, COMPILE_CLANG },

    // C6262: stack frame exceeds threshold. Default is 16 KB; raised to 64 KB here.
    // We have 1 MB of stack and stay shallow, so large locals are not a concern.
    // To suppress entirely instead: { "/wd6262", CONFIG_COUNT, COMPILE_MSVC }

    // C6262: stack frame exceeds 16 KB after inlining.

    { "/wd6262",                           CONFIG_COUNT, COMPILE_MSVC  }, 
 // { "/analyze:stacksize 65536",          CONFIG_COUNT, COMPILE_MSVC  },
};

int g_warn_suppression_count = sizeof( g_warn_suppressions ) / sizeof( g_warn_suppressions[ 0 ] );

/*==============================================================================================
    --- Compile Define Tables ---

    Single source of truth for preprocessor defines. Both 07_compile.c
    (cl.exe invocation) and 12_gen_vs.c (build_intellisense_defines, feeding
    IntelliSense in every vcxproj format) iterate these arrays so the consumers
    can never silently diverge.
==============================================================================================*/

const char* g_defines_always[] = {
    "_CRT_SECURE_NO_WARNINGS",
    NULL,
};

const char* g_defines_debug[] = {
    "_DEBUG",
    "GUI_DEBUG_OVERLAY",       /* compile the gui debug overlay into Debug builds only      */
    "GUI_PIPELINE_DASHBOARD",  /* compile the gui pipeline dashboard into Debug builds only */
    NULL,
};

const char* g_defines_release[] = {
    "NDEBUG",
    NULL,
};

// Subset of compile flags the IntelliSense parser needs to match cl.exe's
// language and conformance behavior.
const char* g_intellisense_flags[] = {
    "/TC",
    "/std:c11",
    "/Zc:preprocessor",
    NULL,
};

/*==============================================================================================
    --- Target Lookup Helpers ---

    The find_*_tool() pair of a flag and a target: each returns the first target carrying its
    is_*_tool flag, or NULL when none does. Every consumer -- the builder, the scheduler, both
    project generators, -doctor -- resolves a tool through these rather than scanning the flag
    or naming the tool in a string literal, so a project that registers its own reflect tool,
    res tool or cooker is picked up everywhere at once and no two sites can disagree about
    which target IS the tool.
==============================================================================================*/

static target_info_t*
find_target( const char* name )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( strcmp( g_targets[ i ].name, name ) == 0 )
            return &g_targets[ i ];
    return NULL;
}

static target_info_t*
find_target_icase( const char* name )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( str_icmp( g_targets[ i ].name, name ) == 0 )
            return &g_targets[ i ];
    return NULL;
}

static target_info_t*
find_build_tool( void )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( g_targets[ i ].is_build_tool )
            return &g_targets[ i ];
    return NULL;
}

static target_info_t*
find_reflect_tool( void )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( g_targets[ i ].is_reflect_tool )
            return &g_targets[ i ];
    return NULL;
}

static target_info_t*
find_res_tool( void )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( g_targets[ i ].is_res_tool )
            return &g_targets[ i ];
    return NULL;
}

static target_info_t*
find_asset_tool( void )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( g_targets[ i ].is_asset_tool )
            return &g_targets[ i ];
    return NULL;
}

/*==============================================================================================
    --- Reflection File Base Name ---

    The base name reflect_tool gives a target's generated .c/.h. Defaults to the target name;
    an orb.targets entry may override it with a `reflect_name` line. Every site that builds a
    generated-file path or passes reflect_tool its -name argument reads it from here.
==============================================================================================*/

static const char*
target_reflect_name( const target_info_t* t )
{
    return t->reflect_name ? t->reflect_name : t->name;
}

/*==============================================================================================
    --- Resource Manifest Policy ---

    A resource manifest (<name>_res_manifest.txt, see build_gen_res_manifest) belongs to an
    IMAGE: every executable and every DLL module gets one, listing the names its own units
    and its statically linked libraries mark with RID() / RES_TREE(). Static libraries never
    get a manifest of their own; their names land in whichever image links them. The three
    builtin tools are excluded: res_tool cannot depend on itself, and build_tool and
    reflect_tool must build before it exists.
==============================================================================================*/

static bool
target_wants_res_manifest( const target_info_t* t )
{
    if ( t->alias_for )
        return false;
    if ( t->is_build_tool || t->is_reflect_tool || t->is_res_tool )
        return false;
    return t->type == TARGET_DYNAMIC_LIB || t->type == TARGET_EXECUTABLE;
}

/*==============================================================================================
    --- Content Tool Membership ---

    True for the targets that make up the content pipeline: res_tool, asset_tool, and the
    cookers asset_tool spawns as bin/ siblings (its tool_deps -- shader_tool, font_tool).
    Membership is derived from the flags and the tool_dep edges, so a project that swaps in
    its own cooker is covered without touching this file.

    The scheduler consults it when a job fails: a content tool produces nothing the compiler
    reads, so its failure is reported and the graph keeps going (see worker_main). reflect_tool
    is not a member -- its generated .c/.h are compiled, so it stays a hard dependency.
==============================================================================================*/

static bool
target_is_content_tool( const target_info_t* t )
{
    if ( t->is_res_tool || t->is_asset_tool )
        return true;

    // A cooker the asset tool spawns: reached through find_asset_tool() so membership is
    // computed against the same target build_cook_content() actually runs.
    const target_info_t* cooker = find_asset_tool();
    if ( cooker )
        for ( int d = 0; cooker->tool_deps[ d ]; ++d )
            if ( strcmp( cooker->tool_deps[ d ], t->name ) == 0 )
                return true;
    return false;
}

/*============================================================================================*/
// clang-format on