/*==============================================================================================

    build_tool_02_data.c -- Build inputs (targets + solutions) + helper functions.
    
    The in-memory representation of the build graph as parsed from orb.targets.    

    At startup, main() calls (in this order):

        registry_load( "orb.targets" )

        --  appended by build_tool_03_registry.c; all project targets and solutions
            live there. If orb.targets contains 'engine <path>', this sets
            g_engine_root before returning.

        init_builtin_targets()

        --  registers build_tool and reflect_tool into g_targets[].
            If g_engine_root is set, paths and is_external are derived from it;
            otherwise CWD-relative paths are used (engine-root build).

    g_engine_root:

        Empty string at startup. Set by registry_load() when it encounters the
        'engine <path>' directive in orb.targets. Used by init_builtin_targets()
        and the compile/gen modules to auto-add engine header search paths.

    Why build_tool and reflect_tool are hard-coded here and not in orb.targets:

        - build_tool.exe needs to be able to bootstrap itself (-bootstrap flag) even
          if orb.targets is missing or malformed.
        - reflect_tool is an immediate dependency of the bootstrap path (core etc.
          need it), so it must always be resolvable.
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

    Registers the two targets that must always be present:
        build_tool   -- the build orchestrator (this executable).
        reflect_tool -- the reflection code-generator.

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
    if ( is_external )
    {
        snprintf( bt_root, sizeof( bt_root ), "%s/source/tools/build_tool",  g_engine_root );
        snprintf( rt_root, sizeof( rt_root ), "%s/source/tools/reflect_tool", g_engine_root );
    }
    else
    {
        snprintf( bt_root, sizeof( bt_root ), "source/tools/build_tool" );
        snprintf( rt_root, sizeof( rt_root ), "source/tools/reflect_tool" );
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
    "IMGUI_DEBUG_OVERLAY",   /* compile the imgui debug overlay into Debug builds only */
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

/*============================================================================================*/
// clang-format on