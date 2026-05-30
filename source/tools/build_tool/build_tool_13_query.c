/*==============================================================================================

    build_tool_13_query.c -- read-only query commands for build_tool.c.

    Commands that inspect the build graph without invoking the compiler.
    Included after 13_create.c so all data pools and registry symbols are visible.

    Commands:
        cmd_print_help   -- -help / -h: usage reference for all recognized arguments
        cmd_list         -- -list: print all registered targets
        cmd_graph        -- -graph: dependency tree and topological build order

    Dependency graph internals (deps_*) live here because they exist solely to
    serve cmd_graph; no other module uses them.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- cmd_print_help (-help / -h) ---

    Prints the full argument reference and exits. Keep in sync with the arg parser in main().
==============================================================================================*/

static int
cmd_print_help( void )
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
    printf( ORB_INDENT "  %-28s%s\n", "-create <name>",         "Scaffold a new module. Requires -dir; optional -type static|dynamic." );
    printf( "\n" );

    printf( ORB_INDENT "create options:\n" );
    printf( ORB_INDENT "  %-28s%s\n", "-dir <source/path>",     "Output directory (e.g. source/engine/physics). Required with -create." );
    printf( ORB_INDENT "  %-28s%s\n", "-type static|dynamic",   "Module kind (default: static). dynamic = hot-reload DLL." );
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
    printf( ORB_INDENT "  %-28s%s\n", "-vs-version <year>",     "-gen: override VS version (e.g. 2022, 2026). Default: auto-detect." );
    printf( ORB_INDENT "  %-28s%s\n", "-no-fwd-compat",         "-gen: omit stdcpp20 IntelliSense mode; use strict C11." );
    printf( ORB_INDENT "  %-28s%s\n", "-no-rsp",                "Pass command lines directly; skip .rsp response files." );
    printf( ORB_INDENT "  %-28s%s\n", "-no-include-track",      "Skip /showIncludes; header changes won't trigger rebuild." );
    printf( "\n" );
    return 0;
}

/*==============================================================================================
    --- Dependency Graph Internals ---

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
    --- cmd_list (-list) ---
==============================================================================================*/

static int
cmd_list( void )
{
    static const char* type_tag[] = { "lib", "dll", "exe" };

    // Measure the longest name for column alignment.
    int max_name = 4;
    for ( int i = 0; i < g_target_count; ++i )
    {
        int n = ( int )strlen( g_targets[ i ].name );
        if ( n > max_name ) max_name = n;
    }

    int local_count    = 0;
    int external_count = 0;
    for ( int i = 0; i < g_target_count; ++i )
    {
        if ( g_targets[ i ].is_external ) external_count++;
        else                              local_count++;
    }

    printf( ORB_BANNER "[orb targets] %d local, %d external\n\n",
            local_count, external_count );

    // Local targets first, then external; each group in registration order.
    for ( int pass = 0; pass < 2; ++pass )
    {
        bool printing_external = ( pass == 1 );
        bool printed_header    = false;
        for ( int i = 0; i < g_target_count; ++i )
        {
            const target_info_t* t = &g_targets[ i ];
            if ( t->is_external != printing_external ) continue;

            if ( !printed_header )
            {
                printf( ORB_INDENT "%s\n", printing_external ? "(external)" : "(local)" );
                printed_header = true;
            }

            const char* kind   = ( t->is_build_tool || t->is_reflect_tool || t->is_tool ) ? " [tool]" : "";
            const char* folder = ( t->virtual_folder && t->virtual_folder[ 0 ] ) ? t->virtual_folder : "";
            printf( ORB_INDENT "  [%s]  %-*s  %s%s\n",
                    type_tag[ t->type ], max_name, t->name, folder, kind );
        }
        if ( printed_header ) printf( "\n" );
    }
    return 0;
}

/*==============================================================================================
    --- cmd_graph (-graph) ---

    With -target: prints the dependency tree rooted at that target and the flat
    topological build order for its closure.
    Without -target: prints the flat build order for all local targets collectively.
==============================================================================================*/

static int
cmd_graph( const char* target_name )
{
    static const char* type_tag[] = { "lib", "dll", "exe" };

    target_info_t* root = NULL;
    if ( target_name )
    {
        root = find_target_icase( target_name );
        if ( !root )
        {
            printf( ORB_INDENT "[orb error] unknown target '%s'\n", target_name );
            return 1;
        }
    }

    static deps_topo_t topo;
    memset( &topo, 0, sizeof( topo ) );

    if ( root )
    {
        deps_visit( &topo, root );
    }
    else
    {
        for ( int i = 0; i < g_target_count; ++i )
            if ( !g_targets[ i ].is_external )
                deps_visit( &topo, &g_targets[ i ] );
    }

    if ( topo.has_cycle )
    {
        printf( ORB_INDENT "[orb error] %s\n", topo.cycle_msg );
        return 1;
    }

    const char* label = root ? root->name : "ALL";
    printf( ORB_BANNER "[orb deps]  %s  (%d in closure)\n", label, topo.count );

    // Tree view: single-target mode only.
    if ( root )
    {
        int shown[ MAX_TARGETS + 4 ] = { 0 };
        int ridx = deps_target_idx( root );
        if ( ridx >= 0 ) shown[ ridx ]++;
        printf( "\n" );
        printf( ORB_INDENT "tree:\n" );
        printf( "%s  %-22s  [%s]\n", ORB_INDENT, root->name, type_tag[ root->type ] );
        deps_tree_children( root, shown, "  " );
    }

    // Flat topological build order (leaves first).
    int num_w = ( topo.count >= 10 ) ? 2 : 1;
    printf( "\n" );
    printf( ORB_INDENT "build order:\n" );
    for ( int i = 0; i < topo.count; ++i )
    {
        target_info_t* t = topo.order[ i ];
        printf( "%s  %*d  [%s]  %s\n",
                ORB_INDENT, num_w, i + 1, type_tag[ t->type ], t->name );
    }
    printf( "\n" );
    return 0;
}

/*============================================================================================*/
// clang-format on
