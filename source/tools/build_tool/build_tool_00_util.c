/*==============================================================================================

    build_tool_00_util.c -- pre-main utility functions for build_tool.c.

    Contains output, validation, and query helpers that are too large to sit inline
    in the main entry point but depend on the full unity chain being compiled first
    (they reference symbols from 02_data, 07_compile, etc.).

    Included at the end of build_tool.c's unity chain, immediately before main().

    Sections:
        print_help           -- -help/-h command: usage text for all recognized arguments
        deps_*               -- dependency graph: topo sort, tree printer (-graph command)
        validate_targets     -- sanity-check the target/solution tables before any build
        print_startup_banner -- standardized header printed at the start of every build

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- print_help (-help / -h) ---

    Prints the full argument reference and exits. Keep in sync with the arg parser in main().
==============================================================================================*/

static void
print_help( void )
{
    printf( ORB_BANNER "[orb help]\n" );
    printf( "\n" );
    printf( ORB_INDENT "usage: build_tool.exe [command] [options]\n" );
    printf( "\n" );

    printf( ORB_INDENT "commands:\n" );
    printf( ORB_INDENT "  %-28s%s\n", "-help, -h",              "Print this message and exit." );
    printf( ORB_INDENT "  %-28s%s\n", "-list",                  "Print all registered targets and exit." );
    printf( ORB_INDENT "  %-28s%s\n", "-graph",                 "Print flat topological build order. Add -target for tree view." );
    printf( ORB_INDENT "  %-28s%s\n", "-clean",                 "Wipe build outputs. Add -target to clean one target." );
    printf( ORB_INDENT "  %-28s%s\n", "-bootstrap",             "Recompile build_tool.exe itself." );
    printf( ORB_INDENT "  %-28s%s\n", "-gen",                   "Regenerate all project files (NMake + MSBuild + VSCode)" );
    printf( ORB_INDENT "  %-28s%s\n", "-gen_nm",                "Regenerate NMake .sln/.vcxproj, compile_commands.json, .vscode/tasks.json." );
    printf( ORB_INDENT "  %-28s%s\n", "-gen_ms",                "Regenerate MSBuild .sln/.vcxproj only (full EDG IntelliSense)." );
    printf( "\n" );

    printf( ORB_INDENT "build options:\n" );
    printf( ORB_INDENT "  %-28s%s\n", "-target <name>",         "Restrict build/clean/graph to one target's closure." );
    printf( ORB_INDENT "  %-28s%s\n", "-config <Debug|Release>","Build configuration (default: Debug)." );
    printf( ORB_INDENT "  %-28s%s\n", "-release",               "Shortcut for -config Release." );
    printf( ORB_INDENT "  %-28s%s\n", "-shipping",              "Release + /GL + /LTCG (whole-program optimization). Implies -release." );
    printf( ORB_INDENT "  %-28s%s\n", "-monolithic, -mono",     "Build DLL modules as static libs; defines BUILD_STATIC." );
    printf( ORB_INDENT "  %-28s%s\n", "-clang",                 "Use clang-cl instead of cl.exe." );
    printf( ORB_INDENT "  %-28s%s\n", "-force",                 "Skip the up-to-date check; always compile + link." );
    printf( ORB_INDENT "  %-28s%s\n", "-no-deps",               "Build only the named target; skip dep recursion. (VS -managed)" );
    printf( ORB_INDENT "  %-28s%s\n", "-compile-only",          "Compile all unity units for -target; no link. (VS Ctrl+F7)" );
    printf( ORB_INDENT "  %-28s%s\n", "-file <path>",           "Compile one file with target's full flag set; no link." );
    printf( ORB_INDENT "  %-28s%s\n", "-j N",                   "Worker thread count (default: auto-detect from CPU count)." );
    printf( "\n" );

    printf( ORB_INDENT "output:\n" );
    printf( ORB_INDENT "  %-28s%s\n", "-q",                     "Quiet: suppress most output." );
    printf( ORB_INDENT "  %-28s%s\n", "-v",                     "Verbose: enable all output sections." );
    printf( ORB_INDENT "  %-28s%s\n", "--out <hex>",            "Fine-grained output mask (see out_flags_t in build_tool.h)." );
    printf( "\n" );

    printf( ORB_INDENT "developer:\n" );
    printf( ORB_INDENT "  %-28s%s\n", "-no-fwd-compat",         "-gen: omit stdcpp20 IntelliSense mode; use strict C11." );
    printf( ORB_INDENT "  %-28s%s\n", "-no-rsp",                "Pass command lines directly; skip .rsp response files." );
    printf( ORB_INDENT "  %-28s%s\n", "-no-include-track",      "Skip /showIncludes; header changes won't trigger rebuild." );
    printf( "\n" );
}

/*==============================================================================================
    --- Dependency Graph Dump (-graph) ---

    Resolves the transitive closure of a target (or all local targets), detects cycles,
    then prints a dependency tree (single-target mode) and a flat topological build order.
    The tree uses +-- / `-- ASCII connectors; [dup] marks nodes already expanded above.
==============================================================================================*/

#define DEPS_MAX_TOPO   128
#define DEPS_PREFIX_MAX 256

typedef struct
{
    target_info_t*  order[ DEPS_MAX_TOPO ];       // Topological order: leaves first.
    int             count;
    int             visited[ MAX_TARGETS + 4 ];   // 0=unvisited  1=on_stack  2=done
    bool            has_cycle;
    char            cycle_msg[ 128 ];
} deps_topo_t;

// Returns t's index in g_targets[], or -1 if not found.
static int
deps_target_idx( const target_info_t* t )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( &g_targets[ i ] == t ) return i;
    return -1;
}

// Post-order DFS: appends t to topo->order[] after all its dependencies.
static bool
deps_visit( deps_topo_t* topo, target_info_t* t )
{
    int idx = deps_target_idx( t );
    if ( idx < 0 ) return true;
    if ( topo->visited[ idx ] == 2 ) return true;
    if ( topo->visited[ idx ] == 1 )
    {
        topo->has_cycle = true;
        snprintf( topo->cycle_msg, sizeof( topo->cycle_msg ),
                  "cycle detected at '%s'", t->name );
        return false;
    }

    topo->visited[ idx ] = 1;  // on stack

    for ( int i = 0; t->deps[ i ]; ++i )
    {
        target_info_t* d = find_target( t->deps[ i ] );
        if ( d && !deps_visit( topo, d ) ) return false;
    }
    for ( int i = 0; t->tool_deps[ i ]; ++i )
    {
        target_info_t* d = find_target( t->tool_deps[ i ] );
        if ( d && !deps_visit( topo, d ) ) return false;
    }
    if ( t->has_reflect )
    {
        target_info_t* rt = find_reflect_tool();
        if ( rt && !deps_visit( topo, rt ) ) return false;
    }

    topo->visited[ idx ] = 2;  // done
    if ( topo->count < DEPS_MAX_TOPO )
        topo->order[ topo->count++ ] = t;
    return true;
}

// Gathers all direct deps of t (link + tool + implicit reflect) into out_deps[]/out_kind[].
static int
deps_collect( const target_info_t* t, target_info_t* out_deps[], const char* out_kind[], int max )
{
    int n = 0;
    for ( int i = 0; t->deps[ i ] && n < max; ++i )
    {
        target_info_t* d = find_target( t->deps[ i ] );
        if ( d ) { out_deps[ n ] = d; out_kind[ n++ ] = "link"; }
    }
    for ( int i = 0; t->tool_deps[ i ] && n < max; ++i )
    {
        target_info_t* d = find_target( t->tool_deps[ i ] );
        if ( d ) { out_deps[ n ] = d; out_kind[ n++ ] = "tool"; }
    }
    if ( t->has_reflect && n < max )
    {
        target_info_t* rt = find_reflect_tool();
        if ( rt )
        {
            bool dup = false;
            for ( int d = 0; d < n; ++d ) if ( out_deps[ d ] == rt ) { dup = true; break; }
            if ( !dup ) { out_deps[ n ] = rt; out_kind[ n++ ] = "tool"; }
        }
    }
    return n;
}

// Forward declaration: deps_tree_node -> deps_tree_children -> deps_tree_node (mutual recursion).
static void deps_tree_node( target_info_t* t, const char* kind, int shown[], const char* p, bool last );

static void
deps_tree_children( target_info_t* t, int shown[], const char* p )
{
    target_info_t* d[ TARGET_MAX_SLOTS + 2 ];
    const char*    k[ TARGET_MAX_SLOTS + 2 ];
    int n = deps_collect( t, d, k, TARGET_MAX_SLOTS + 2 );
    for ( int i = 0; i < n; ++i )
        deps_tree_node( d[ i ], k[ i ], shown, p, ( i == n - 1 ) );
}

// Prints one tree line then recurses into children if not already expanded.
// kind: "link" / "tool" (NULL for root node printed inline by caller).
static void
deps_tree_node( target_info_t* t, const char* kind, int shown[], const char* p, bool last )
{
    static const char* type_tag[] = { "lib", "dll", "exe" };
    int  idx     = deps_target_idx( t );
    bool already = ( idx >= 0 && shown[ idx ] != 0 );
    if ( idx >= 0 ) shown[ idx ]++;

    printf( "%s%s%s%-22s  [%s]%s%s%s\n",
            ORB_INDENT, p, last ? "`-- " : "+-- ",
            t->name, type_tag[ t->type ],
            kind ? "  " : "", kind ? kind : "",
            already ? "  [dup]" : "" );

    if ( !already )
    {
        char child_p[ DEPS_PREFIX_MAX ];
        snprintf( child_p, sizeof( child_p ), "%s%s", p, last ? "    " : "|   " );
        deps_tree_children( t, shown, child_p );
    }
}

/*==============================================================================================
    --- validate_targets ---

    Sanity-check the target and solution tables before any build work begins.
    Catches slot-array overflows and unresolved solution-to-target name references.
==============================================================================================*/

static bool
validate_targets( void )
{
    bool ok = true;

    // Duplicate target names.
    for ( int i = 0; i < g_target_count; ++i )
        for ( int j = i + 1; j < g_target_count; ++j )
            if ( strcmp( g_targets[ i ].name, g_targets[ j ].name ) == 0 )
            {
                printf( ORB_INDENT "[orb error] duplicate target name '%s'\n", g_targets[ i ].name );
                ok = false;
            }

    // Duplicate solution names.
    for ( int i = 0; i < g_solution_count; ++i )
        for ( int j = i + 1; j < g_solution_count; ++j )
            if ( g_solutions[ i ].name && g_solutions[ j ].name &&
                 strcmp( g_solutions[ i ].name, g_solutions[ j ].name ) == 0 )
            {
                printf( ORB_INDENT "[orb error] duplicate solution name '%s'\n", g_solutions[ i ].name );
                ok = false;
            }

    // Valide each target
    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];

        // Non-external targets must have a root_dir (guards unit file checks below).
        if ( !t->is_external && !t->root_dir )
        {
            printf( ORB_INDENT "[orb error] target '%s': missing root directory\n", t->name );
            ok = false;
            continue;
        }

        // Unit files exist on disk.
        for ( int j = 0; j < TARGET_MAX_SLOTS && t->units[ j ]; ++j )
        {
            char path[ PATH_MAX ];
            snprintf( path, sizeof( path ), "%s/%s", t->root_dir, t->units[ j ] );
            if ( !platform_file_exists( path ) )
            {
                printf( ORB_INDENT "[orb error] target '%s': unit file not found '%s'\n",
                        t->name, t->units[ j ] );
                ok = false;
            }
        }

        // Slot overflow.
        if ( t->units    [ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->deps     [ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->tool_deps[ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->mono_deps[ TARGET_MAX_SLOTS - 1 ] != NULL )
        {
            printf( ORB_INDENT "[orb error] target '%s' has too many units or dependencies "
                               "(raise TARGET_MAX_SLOTS)\n", t->name );
            ok = false;
        }

        // Unresolved and self-referencing dep / tool_dep / mono_dep names.
        for ( int j = 0; j < TARGET_MAX_SLOTS && t->deps[ j ]; ++j )
        {
            if ( strcmp( t->deps[ j ], t->name ) == 0 )
                printf( ORB_INDENT "[orb error] target '%s': dep references itself\n", t->name ),
                ok = false;
            else if ( !find_target( t->deps[ j ] ) )
                printf( ORB_INDENT "[orb error] target '%s': unknown dep '%s'\n",
                        t->name, t->deps[ j ] ),
                ok = false;
        }

        // All deps exist + prevent tool targets from depending on oneself.
        for ( int j = 0; j < TARGET_MAX_SLOTS && t->tool_deps[ j ]; ++j )
        {
            if ( strcmp( t->tool_deps[ j ], t->name ) == 0 )
                printf( ORB_INDENT "[orb error] target '%s': tool_dep references itself\n", t->name ),
                ok = false;
            else if ( !find_target( t->tool_deps[ j ] ) )
                printf( ORB_INDENT "[orb error] target '%s': unknown tool_dep '%s'\n",
                        t->name, t->tool_deps[ j ] ),
                ok = false;
        }

        // All mono_deps exist + prevent monolithic (static) targets from depending on oneself.
        for ( int j = 0; j < TARGET_MAX_SLOTS && t->mono_deps[ j ]; ++j )
        {
            if ( strcmp( t->mono_deps[ j ], t->name ) == 0 )
                printf( ORB_INDENT "[orb error] target '%s': mono_dep references itself\n", t->name ),
                ok = false;
            else if ( !find_target( t->mono_deps[ j ] ) )
                printf( ORB_INDENT "[orb error] target '%s': unknown mono_dep '%s'\n",
                        t->name, t->mono_deps[ j ] ),
                ok = false;
        }
    }

    // Unresolved solution-to-target references and missing out_dir.
    for ( int i = 0; i < g_solution_count; ++i )
    {
        const solution_info_t* sln = &g_solutions[ i ];

        if ( !sln->is_external && !sln->out_dir )
        {
            printf( ORB_INDENT "[orb error] solution '%s': missing 'out' directory\n", sln->name );
            ok = false;
        }

        for ( const char* const* tn = sln->target_names; *tn; ++tn )
        {
            bool found = false;
            for ( int j = 0; j < g_target_count; ++j )
                if ( strcmp( g_targets[ j ].name, *tn ) == 0 ) { found = true; break; }
            if ( !found )
            {
                printf( ORB_INDENT "[orb error] solution '%s' references unknown target '%s'\n",
                        sln->name, *tn );
                ok = false;
            }
        }
    }
    return ok;
}

/*==============================================================================================
    --- print_startup_banner ---

    Prints a standardized header at the start of every build with the active
    configuration. Calls get_target_upper() from 07_compile.c.
==============================================================================================*/

static void
print_startup_banner( const build_context_t* ctx )
{
    char target_upper[ 64 ] = "ALL";
    if ( ctx->target_name )
        get_target_upper( ctx->target_name, target_upper, sizeof( target_upper ) );

    /* truncate file path to just the file name. */

    const char* file_name = ctx->file_path;
    if ( file_name )
        for ( const char* p = ctx->file_path; *p; ++p )
            if ( *p == '\\' || *p == '/' ) file_name = p + 1;

    const char* config_str   = ctx->config   == CONFIG_DEBUG  ? "debug"
                             : ctx->is_shipping                ? "release+shipping" : "release";
    const char* compiler_str = ctx->compiler == COMPILE_CLANG ? "clang" : "msvc";
    const char* mode_str     = ctx->is_monolithic ? "monolithic" : "modular";
    const char* label        = NULL;
    const char* special      = NULL;

    char subject[ PATH_MAX ];
    snprintf( subject, sizeof( subject ), "%s", target_upper );

    if ( ctx->compile_only )
    {
        label   = "[orb compile-only]";
        special = "no-link";
    }
    else if ( ctx->file_path )
    {
        label   = "[orb single-file]";
        special = "file";
        snprintf( subject, sizeof( subject ), "%s %s", target_upper, file_name );
    }
    else
    {
        label   = "[orb build]";
        special = ctx->skip_deps ? "no-deps" : NULL;
    }

    char props[ 128 ];
    if ( special )
        snprintf( props, sizeof( props ), "[ %s | %s | %s | %s ]", special, mode_str, config_str, compiler_str );
    else
        snprintf( props, sizeof( props ), "[ %s | %s | %s ]", mode_str, config_str, compiler_str );

    printf( ORB_BANNER "----------------------------------------------------------------\n" );
    printf( ORB_BANNER "%s %s %s\n", label, subject, props );
}

/*============================================================================================*/
// clang-format off