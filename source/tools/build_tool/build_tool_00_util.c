/*==============================================================================================

    build_tool_00_util.c -- pre-main utility functions for build_tool.c.

    Included at the end of build_tool.c's unity chain so both functions can
    reference symbols from the full chain (02_data, 07_compile, etc.).

    Sections:
        validate_dep_array   -- shared checks for one NULL-terminated dep slot array
        validate_targets     -- sanity-check the target/solution tables before any build
        print_startup_banner -- standardized header printed at the start of every build

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- validate_dep_array ---

    Checks one of a target's NULL-terminated dep slot arrays: no name may reference the
    owning target, and every name must resolve to a declared target. 'kind' is the field
    name as it appears in error messages ("dep", "tool_dep", "mono_dep").

    reject_host_only additionally bars a dynamic target from listing a host_only dep.
    Host-only globals live exclusively in the host exe; a DLL that links one gets a
    private uninitialized copy and will fail at runtime. Only the link deps carry this
    rule -- tool deps are never linked, and mono deps only link in a monolithic build
    where there is no separate DLL image.
==============================================================================================*/

static bool
validate_dep_array( const target_info_t* t, const char* const* deps, const char* kind,
                    bool reject_host_only )
{
    bool ok = true;

    for ( int j = 0; j < TARGET_MAX_SLOTS && deps[ j ]; ++j )
    {
        const target_info_t* dep = find_target( deps[ j ] );

        if ( strcmp( deps[ j ], t->name ) == 0 )
        {
            printf( ORB_INDENT "[orb error] target '%s': %s references itself\n", t->name, kind );
            ok = false;
        }
        else if ( !dep )
        {
            printf( ORB_INDENT "[orb error] target '%s': unknown %s '%s'\n",
                    t->name, kind, deps[ j ] );
            ok = false;
        }
        else if ( reject_host_only && t->type == TARGET_DYNAMIC_LIB && dep->is_host_only )
        {
            printf( ORB_INDENT "[orb error] target '%s' (dynamic): %s '%s' is host_only"
                               " -- access it through the module API\n",
                    t->name, kind, deps[ j ] );
            ok = false;
        }
    }
    return ok;
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

    // Validate each target
    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];

        // Alias launcher targets carry no sources of their own -- validate the reference
        // and skip the source/type checks below. Build/clean/gen resolve through alias_for.
        if ( t->alias_for )
        {
            const target_info_t* aliased = find_target( t->alias_for );
            if ( !aliased )
            {
                printf( ORB_INDENT "[orb error] target '%s': alias references unknown target '%s'\n",
                        t->name, t->alias_for );
                ok = false;
            }
            else if ( aliased->alias_for )
            {
                printf( ORB_INDENT "[orb error] target '%s': alias chains are not allowed"
                                   " ('%s' is itself an alias)\n", t->name, t->alias_for );
                ok = false;
            }
            if ( !t->run_cmd )
                printf( ORB_INDENT "[orb warn] target '%s': alias without a 'run' line does nothing\n",
                        t->name );
            continue;
        }

        // Non-external targets must have a root_dir (guards unit file checks below).
        if ( !t->is_external && !t->root_dir )
        {
            printf( ORB_INDENT "[orb error] target '%s': missing 'root' directory\n", t->name );
            ok = false;
            continue;
        }

        // 'type' must be explicitly declared; there is no safe default.
        if ( !t->is_external && !t->has_type )
        {
            printf( ORB_INDENT "[orb error] target '%s': missing 'type' (use static/dynamic/exe)\n", t->name );
            ok = false;
        }

        // Non-external targets must have at least one unity entry file.
        if ( !t->is_external && !t->units[ 0 ] )
        {
            printf( ORB_INDENT "[orb error] target '%s': no 'unit' entries\n", t->name );
            ok = false;
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

        // Slot overflow (defense-in-depth). registry_load() now hard-fails on a full
        // slot array at parse time, so this only ever fires for programmatically
        // constructed targets (e.g. the built-ins) that bypass the parser.
        if ( t->units    [ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->deps     [ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->tool_deps[ TARGET_MAX_SLOTS - 1 ] != NULL ||
             t->mono_deps[ TARGET_MAX_SLOTS - 1 ] != NULL )
        {
            printf( ORB_INDENT "[orb error] target '%s' has too many units or dependencies "
                               "(raise TARGET_MAX_SLOTS)\n", t->name );
            ok = false;
        }

        // Every dep slot array: names resolve, nothing references its own target.
        if ( !validate_dep_array( t, t->deps,      "dep",      true  ) ) ok = false;
        if ( !validate_dep_array( t, t->tool_deps, "tool_dep", false ) ) ok = false;
        if ( !validate_dep_array( t, t->mono_deps, "mono_dep", false ) ) ok = false;
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

        // A solution with no targets generates an empty workspace -- almost always a mistake.
        if ( !sln->is_external && !sln->target_names[ 0 ] )
            printf( ORB_INDENT "[orb warn] solution '%s': no targets added (use 'add')\n", sln->name );

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
    configuration.
==============================================================================================*/

static void
print_startup_banner( const build_context_t* ctx )
{
    char target_upper[ 64 ] = "ALL";
    if ( ctx->target_name )
        str_upper( ctx->target_name, target_upper, sizeof( target_upper ) );

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
// clang-format on