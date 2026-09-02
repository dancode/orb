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
        - res_tool is an implicit dependency of every executable that links the res
          library and of every dynamic module, including those of a child project that
          only imports the engine's targets, so it must resolve the same way.
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
        // /guard:cf on both compiler and linker writes a CFG bitmap into the PE header,
        // which EDRs read as a positive trust signal (marks the binary as memory-safe).
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

    // res_tool: the resource-reference harvester (RID / RES_TREE tokens -> res tables).
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
    (cl.exe invocation) and 12_gen_nmake.c / 12_gen_msbuild.c (IntelliSense
    vcxproj emission) iterate these arrays so the consumers can never silently
    diverge.
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

/*==============================================================================================
    --- Resource Table Policy ---

    A generated resource table (<name>_res_table.c, see build_gen_res_table) belongs to an
    IMAGE: a DLL module always gets one, under g_<name>_res_table, which its descriptor
    points at through MOD_RES_TABLE; an executable gets one, under g_host_res_table, when
    the res library is anywhere in its link closure -- res's own descriptor carries that
    symbol, so an exe linking res cannot link without it. Static libraries never get a
    table of their own; their names are harvested into whichever image links them.
==============================================================================================*/

// True when `name` is reachable from t through link deps. visited[] is indexed like g_targets[].
static bool
deps_closure_has( const target_info_t* t, const char* name, bool visited[ MAX_TARGETS ] )
{
    for ( int i = 0; t->deps[ i ]; ++i )
    {
        if ( strcmp( t->deps[ i ], name ) == 0 )
            return true;
        const target_info_t* dep = find_target( t->deps[ i ] );
        if ( !dep )
            continue;
        int idx = ( int )( dep - g_targets );
        if ( visited[ idx ] )
            continue;
        visited[ idx ] = true;
        if ( deps_closure_has( dep, name, visited ) )
            return true;
    }
    return false;
}

static bool
target_wants_res_table( const target_info_t* t )
{
    if ( t->alias_for )
        return false;
    if ( t->type == TARGET_DYNAMIC_LIB )
        return true;
    if ( t->type != TARGET_EXECUTABLE )
        return false;

    bool visited[ MAX_TARGETS ] = { 0 };
    return deps_closure_has( t, "res", visited );
}

// Symbol base of the target's table: g_<sym>_res_table. Executables share one fixed name
// because the res library, not the exe, is what declares and registers it.
static const char*
res_table_symbol( const target_info_t* t )
{
    return ( t->type == TARGET_EXECUTABLE ) ? "host" : t->name;
}

/*============================================================================================*/
// clang-format on