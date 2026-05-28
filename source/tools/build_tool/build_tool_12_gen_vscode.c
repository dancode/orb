/*==============================================================================================

    build_tool_12_gen_vscode.c -- VS Code workspace configuration generator.

    Emits .vscode/tasks.json, launch.json, and settings.json.
    Called as part of -gen alongside build_gen_projects() and build_gen_compile_commands().

    Generated tasks:
        orb: build debug      -- full debug build, Ctrl+Shift+B default
        orb: build release    -- full release build
        orb: build target     -- debug build of one target chosen from a dropdown
        orb: clean            -- wipe all build artifacts
        orb: regen            -- regenerate .sln/.vcxproj and compile_commands.json

    The pickString input for "orb: build target" is populated directly from g_targets[]
    so the dropdown always reflects the actual registered target set.

    type:process is used for all tasks -- VS Code spawns build_tool.exe directly without
    going through the shell, so paths with spaces need no extra quoting.

==============================================================================================*/

// VS Code cwd, emitted as a JSON string literal.
// ${workspaceFolder} is substituted by VS Code at task run time.
#define VSCODE_CWD  "\"${workspaceFolder}\""

/*==============================================================================================
    vsc_task()

    Emit one task block (without a leading or trailing comma -- the caller manages
    entry separation). exe_json is the quoted JSON string for the command path.
    with_matcher adds the $msCompile problemMatcher which parses cl.exe error output
    into VS Code inline diagnostics.
==============================================================================================*/

static void
vsc_task( FILE* fp, const char* exe_json, const char* label, const char* args,
          bool with_matcher, bool is_default )
{
    fprintf( fp, "    {\n" );
    fprintf( fp, "      \"label\": \"%s\",\n", label );
    fprintf( fp, "      \"type\": \"process\",\n" );
    fprintf( fp, "      \"command\": %s,\n", exe_json );
    fprintf( fp, "      \"args\": [%s],\n", args );
    if ( is_default )
        fprintf( fp, "      \"group\": { \"kind\": \"build\", \"isDefault\": true },\n" );
    else
        fprintf( fp, "      \"group\": \"build\",\n" );
    fprintf( fp, "      \"options\": { \"cwd\": " VSCODE_CWD " }" );
    if ( with_matcher )
        fprintf( fp, ",\n      \"problemMatcher\": \"$msCompile\"\n" );
    else
        fprintf( fp, "\n" );
    fprintf( fp, "    }" );
}

/*==============================================================================================
    build_gen_vscode()

    Write .vscode/tasks.json. Called from main() as part of the -gen command.
==============================================================================================*/

void
build_gen_vscode( void )
{
    ensure_dir( ".vscode" );

    // Determine the build_tool.exe path for task commands.
    // Child projects use the engine's absolute exe path (forward slashes for JSON).
    // Engine-root builds use a workspace-relative path.
    char vscode_exe[ PATH_MAX + 4 ];
    if ( g_engine_root[ 0 ] )
    {
        char fwd[ PATH_MAX ];
        snprintf( fwd, sizeof( fwd ), "%s/bin/build_tool.exe", g_engine_root );
        for ( char* p = fwd; *p; ++p ) if ( *p == '\\' ) *p = '/';
        snprintf( vscode_exe, sizeof( vscode_exe ), "\"%s\"", fwd );
    }
    else
    {
        snprintf( vscode_exe, sizeof( vscode_exe ),
                  "\"${workspaceFolder}\\\\bin\\\\build_tool.exe\"" );
    }

    const char* out_path = ".vscode/tasks.json";
    FILE* fp = fopen( out_path, "w" );
    if ( !fp )
    {
        printf( ORB_INDENT "[orb error] could not write %s\n", out_path );
        return;
    }

    fprintf( fp, "{\n" );
    fprintf( fp, "  \"version\": \"2.0.0\",\n" );
    fprintf( fp, "  \"tasks\": [\n" );

    vsc_task( fp, vscode_exe, "orb: build debug",
              "\"-config\", \"Debug\"",
              true, true );
    fprintf( fp, ",\n" );

    vsc_task( fp, vscode_exe, "orb: build release",
              "\"-config\", \"Release\"",
              true, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, vscode_exe, "orb: build target",
              "\"-config\", \"Debug\", \"-target\", \"${input:targetName}\"",
              true, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, vscode_exe, "orb: clean",
              "\"-clean\"",
              false, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, vscode_exe, "orb: regen",
              "\"-gen\"",
              false, false );
    fprintf( fp, "\n" );

    fprintf( fp, "  ],\n" );

    // inputs: pickString populated from all registered targets.
    fprintf( fp, "  \"inputs\": [\n" );
    fprintf( fp, "    {\n" );
    fprintf( fp, "      \"id\": \"targetName\",\n" );
    fprintf( fp, "      \"type\": \"pickString\",\n" );
    fprintf( fp, "      \"description\": \"Select target\",\n" );
    fprintf( fp, "      \"options\": [\n" );
    {
        /* Collect local target names first so comma placement is correct. */
        int local_count = 0;
        for ( int i = 0; i < g_target_count; ++i )
            if ( !g_targets[ i ].is_external ) local_count++;
        int emitted = 0;
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( g_targets[ i ].is_external ) continue;
            ++emitted;
            fprintf( fp, "        \"%s\"%s\n",
                     g_targets[ i ].name,
                     ( emitted < local_count ) ? "," : "" );
        }
    }
    fprintf( fp, "      ]\n" );
    fprintf( fp, "    }\n" );
    fprintf( fp, "  ]\n" );
    fprintf( fp, "}\n" );

    fclose( fp );
    {
        int local_count = 0;
        for ( int i = 0; i < g_target_count; ++i )
            if ( !g_targets[ i ].is_external ) local_count++;
        printf( "Generated .vscode/tasks.json (%d targets in picker)\n", local_count );
    }

    // launch.json: one config per local exe target.
    fp = fopen( ".vscode/launch.json", "w" );
    if ( fp )
    {
        fprintf( fp, "{\n" );
        fprintf( fp, "  \"version\": \"0.2.0\",\n" );
        fprintf( fp, "  \"configurations\": [\n" );

        int exe_count = 0;
        for ( int i = 0; i < g_target_count; ++i )
            if ( !g_targets[ i ].is_external && g_targets[ i ].type == TARGET_EXECUTABLE )
                exe_count++;

        int emitted = 0;
        for ( int i = 0; i < g_target_count; ++i )
        {
            target_info_t* t = &g_targets[ i ];
            if ( t->is_external || t->type != TARGET_EXECUTABLE ) continue;
            ++emitted;
            fprintf( fp, "    {\n" );
            fprintf( fp, "      \"name\": \"%s\",\n", t->name );
            fprintf( fp, "      \"type\": \"cppvsdbg\",\n" );
            fprintf( fp, "      \"request\": \"launch\",\n" );
            fprintf( fp, "      \"program\": \"${workspaceFolder}/bin/%s.exe\",\n", t->name );
            fprintf( fp, "      \"args\": [],\n" );
            fprintf( fp, "      \"cwd\": \"${workspaceFolder}\",\n" );
            fprintf( fp, "      \"console\": \"integratedTerminal\",\n" );
            fprintf( fp, "      \"preLaunchTask\": \"orb: build debug\"\n" );
            fprintf( fp, "    }%s\n", ( emitted < exe_count ) ? "," : "" );
        }

        fprintf( fp, "  ]\n" );
        fprintf( fp, "}\n" );
        fclose( fp );
        printf( "Generated .vscode/launch.json (%d configurations)\n", exe_count );
    }
    else
    {
        printf( ORB_INDENT "[orb error] could not write .vscode/launch.json\n" );
    }

    // Disable the MS C/C++ IntelliSense engine so it doesn't conflict with clangd.
    fp = fopen( ".vscode/settings.json", "w" );
    if ( fp )
    {
        fprintf( fp, "{\n" );
        fprintf( fp, "  \"C_Cpp.intelliSenseEngine\": \"disabled\"\n" );
        fprintf( fp, "}\n" );
        fclose( fp );
        printf( "Generated .vscode/settings.json\n" );
    }
    else
    {
        printf( ORB_INDENT "[orb error] could not write .vscode/settings.json\n" );
    }

    // clangd config:
    //   UnusedIncludes: None    -- suppresses false positives from MSVC internal headers
    //   CompileFlags.Add        -- fallback flags for files not in compile_commands.json
    //   -Wno-unused-function    -- static inline helpers in headers appear unused when the
    //                             including TU doesn't call every overload; always false positive
    fp = fopen( ".clangd", "w" );
    if ( fp )
    {
        // Engine include paths with forward slashes for clangd YAML.
        char engine_src_fwd[ PATH_MAX ] = { 0 };
        char engine_gen_fwd[ PATH_MAX ] = { 0 };
        if ( g_engine_root[ 0 ] )
        {
            snprintf( engine_src_fwd, sizeof( engine_src_fwd ),
                      "%s/source", g_engine_root );
            snprintf( engine_gen_fwd, sizeof( engine_gen_fwd ),
                      "%s/%s/generated", g_engine_root, BUILD_DIR );
            for ( char* p = engine_src_fwd; *p; ++p ) if ( *p == '\\' ) *p = '/';
            for ( char* p = engine_gen_fwd; *p; ++p ) if ( *p == '\\' ) *p = '/';
        }

        fprintf( fp, "Diagnostics:\n" );
        fprintf( fp, "  UnusedIncludes: None\n" );
        fprintf( fp, "CompileFlags:\n" );
        if ( g_engine_root[ 0 ] )
        {
            fprintf( fp, "  Add: [--target=x86_64-pc-windows-msvc, -std=c11, -Isource, -Ibuild/generated,\n" );
            fprintf( fp, "        -I%s, -I%s,\n", engine_src_fwd, engine_gen_fwd );
            fprintf( fp, "        -D_CRT_SECURE_NO_WARNINGS, -D_DEBUG, -Wno-unused-function]\n" );
        }
        else
        {
            fprintf( fp, "  Add: [--target=x86_64-pc-windows-msvc, -std=c11, -Isource, -Ibuild/generated,\n" );
            fprintf( fp, "        -D_CRT_SECURE_NO_WARNINGS, -D_DEBUG, -Wno-unused-function]\n" );
        }
        fclose( fp );
        printf( "Generated .clangd\n" );
    }
    else
    {
        printf( ORB_INDENT "[orb error] could not write .clangd\n" );
    }

    // .code-workspace: multi-root workspace so external target source directories are
    // browsable alongside the child project. Only emitted when 'engine' is declared.
    // One folder entry per external target referenced in any local solution (deduplicated),
    // so 'add sys' gives a sys folder and 'add orb' gives each engine target individually.
    if ( g_engine_root[ 0 ] )
    {
        const char* ws_name = "workspace";
        for ( int i = 0; i < g_solution_count; ++i )
        {
            if ( !g_solutions[ i ].is_external && g_solutions[ i ].name )
            {
                ws_name = g_solutions[ i ].name;
                break;
            }
        }

        // Collect external target root_dirs from all local solutions (deduplicated).
        const target_info_t* ext_targets[ MAX_TARGETS ];
        int ext_count = 0;
        for ( int si = 0; si < g_solution_count; ++si )
        {
            if ( g_solutions[ si ].is_external ) continue;
            for ( const char* const* tn = g_solutions[ si ].target_names; *tn; ++tn )
            {
                target_info_t* t = find_target( *tn );
                if ( !t || !t->is_external || !t->root_dir ) continue;
                bool seen = false;
                for ( int k = 0; k < ext_count; ++k )
                    if ( ext_targets[ k ] == t ) { seen = true; break; }
                if ( !seen && ext_count < MAX_TARGETS )
                    ext_targets[ ext_count++ ] = t;
            }
        }

        char ws_path[ PATH_MAX ];
        snprintf( ws_path, sizeof( ws_path ), "%s.code-workspace", ws_name );

        fp = fopen( ws_path, "w" );
        if ( fp )
        {
            fprintf( fp, "{\n" );
            fprintf( fp, "  \"folders\": [\n" );
            fprintf( fp, "    { \"name\": \"%s\", \"path\": \".\" }", ws_name );
            for ( int i = 0; i < ext_count; ++i )
            {
                char fwd[ PATH_MAX ];
                snprintf( fwd, sizeof( fwd ), "%s", ext_targets[ i ]->root_dir );
                for ( char* p = fwd; *p; ++p ) if ( *p == '\\' ) *p = '/';
                fprintf( fp, ",\n    { \"name\": \"%s\", \"path\": \"%s\" }",
                         ext_targets[ i ]->name, fwd );
            }
            fprintf( fp, "\n  ]\n" );
            fprintf( fp, "}\n" );
            fclose( fp );
            printf( "Generated %s (%d external folders)\n", ws_path, ext_count );
        }
        else
        {
            printf( ORB_INDENT "[orb error] could not write %s\n", ws_path );
        }
    }
}

/*============================================================================================*/
