/*==============================================================================================

    build_tool_12_gen_manifest.c -- Single resolved generation manifest.

    Parses g_targets[], g_solutions[], and g_engine_root once into gen_manifest_t.
    All generators (NMake, MSBuild, VSCode, compile_commands) walk this struct
    instead of re-filtering the raw globals, so filtering logic lives in one place
    and every generator sees identical resolved data.

    Build once in main() with gen_manifest_build(), then pass to each generator.

    Struct layout:

        gen_manifest_t
            build_tool_exe          -- NMake/cmd format (backslashes, quoted if absolute)
            build_tool_exe_fwd      -- VSCode/JSON format (forward slashes, quoted)
            engine_src_dir          -- forward slashes; empty if no engine root
            engine_gen_dir          -- forward slashes; empty if no engine root
            workspace_name          -- first local solution name, or "workspace"

            solutions[]             -- local (non-external) solutions, resolved targets
                .sln                -- pointer into g_solutions[]
                .targets[]          -- resolved target_info_t* pointers
                .target_count

            local_targets[]         -- all non-external targets (vscode picker, compile_commands)
            exe_targets[]           -- non-external TARGET_EXECUTABLE targets (launch.json)
            ext_ref_targets[]       -- external targets referenced in any local solution (workspace)

==============================================================================================*/

/*==============================================================================================
    gen_manifest_build()

    Fills a gen_manifest_t from current global state. Call after registry_load() and
    init_builtin_targets() have run so all targets and solutions are registered.
==============================================================================================*/

void
gen_manifest_build( gen_manifest_t* m )
{
    memset( m, 0, sizeof( *m ) );

    // Build tool exe path: absolute engine path when 'engine' is declared, else local.
    if ( g_engine_root[ 0 ] )
    {
        // NMake / cmd.exe: backslashes, quoted (spaces in engine path are safe).
        snprintf( m->build_tool_exe, sizeof( m->build_tool_exe ),
                  "\"%s\\bin\\build_tool.exe\"", g_engine_root );

        // VSCode / JSON: forward slashes, quoted.
        char fwd[ PATH_MAX ];
        snprintf( fwd, sizeof( fwd ), "%s/bin/build_tool.exe", g_engine_root );
        for ( char* p = fwd; *p; ++p ) if ( *p == '\\' ) *p = '/';
        snprintf( m->build_tool_exe_fwd, sizeof( m->build_tool_exe_fwd ), "\"%s\"", fwd );
    }
    else
    {
        snprintf( m->build_tool_exe,     sizeof( m->build_tool_exe ),
                  "bin\\build_tool.exe" );
        snprintf( m->build_tool_exe_fwd, sizeof( m->build_tool_exe_fwd ),
                  "\"${workspaceFolder}\\\\bin\\\\build_tool.exe\"" );
    }

    // Engine include paths (forward slashes for JSON/YAML consumers).
    if ( g_engine_root[ 0 ] )
    {
        snprintf( m->engine_src_dir, sizeof( m->engine_src_dir ),
                  "%s/source", g_engine_root );
        snprintf( m->engine_gen_dir, sizeof( m->engine_gen_dir ),
                  "%s/%s/generated", g_engine_root, BUILD_DIR );
        for ( char* p = m->engine_src_dir; *p; ++p ) if ( *p == '\\' ) *p = '/';
        for ( char* p = m->engine_gen_dir; *p; ++p ) if ( *p == '\\' ) *p = '/';
    }

    // Local solutions: fill gen_sln_entry_t per non-external solution.
    for ( int i = 0; i < g_solution_count; ++i )
    {
        solution_info_t* sln = &g_solutions[ i ];
        if ( sln->is_external ) continue;

        // First local solution name becomes the workspace name.
        if ( m->solution_count == 0 && sln->name )
            snprintf( m->workspace_name, sizeof( m->workspace_name ), "%s", sln->name );

        gen_sln_entry_t* entry = &m->solutions[ m->solution_count++ ];
        entry->sln = sln;
        for ( const char* const* tn = sln->target_names; *tn; ++tn )
        {
            target_info_t* t = find_target( *tn );
            if ( t && entry->target_count < MAX_SLN_TARGETS )
                entry->targets[ entry->target_count++ ] = t;
        }
    }

    if ( !m->workspace_name[ 0 ] )
        snprintf( m->workspace_name, sizeof( m->workspace_name ), "workspace" );

    // All local targets + local exe targets.
    for ( int i = 0; i < g_target_count; ++i )
    {
        target_info_t* t = &g_targets[ i ];
        if ( t->is_external ) continue;
        if ( m->local_target_count < MAX_TARGETS )
            m->local_targets[ m->local_target_count++ ] = t;
        if ( t->type == TARGET_EXECUTABLE && m->exe_target_count < MAX_TARGETS )
            m->exe_targets[ m->exe_target_count++ ] = t;
    }

    // External targets referenced in any local solution (deduplicated by pointer).
    for ( int si = 0; si < m->solution_count; ++si )
    {
        gen_sln_entry_t* entry = &m->solutions[ si ];
        for ( int ti = 0; ti < entry->target_count; ++ti )
        {
            target_info_t* t = entry->targets[ ti ];
            if ( !t->is_external || !t->root_dir ) continue;
            bool seen = false;
            for ( int k = 0; k < m->ext_ref_target_count; ++k )
                if ( m->ext_ref_targets[ k ] == t ) { seen = true; break; }
            if ( !seen && m->ext_ref_target_count < MAX_TARGETS )
                m->ext_ref_targets[ m->ext_ref_target_count++ ] = t;
        }
    }
}

/*============================================================================================*/
