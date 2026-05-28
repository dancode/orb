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
build_gen_vscode( const gen_manifest_t* m )
{
    ensure_dir( ".vscode" );

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

    vsc_task( fp, m->build_tool_exe_fwd, "orb: build debug",
              "\"-config\", \"Debug\"",
              true, true );
    fprintf( fp, ",\n" );

    vsc_task( fp, m->build_tool_exe_fwd, "orb: build release",
              "\"-config\", \"Release\"",
              true, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, m->build_tool_exe_fwd, "orb: build target",
              "\"-config\", \"Debug\", \"-target\", \"${input:targetName}\"",
              true, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, m->build_tool_exe_fwd, "orb: clean",
              "\"-clean\"",
              false, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, m->build_tool_exe_fwd, "orb: regen",
              "\"-gen\"",
              false, false );
    fprintf( fp, "\n" );

    fprintf( fp, "  ],\n" );

    // inputs: pickString populated from local targets.
    fprintf( fp, "  \"inputs\": [\n" );
    fprintf( fp, "    {\n" );
    fprintf( fp, "      \"id\": \"targetName\",\n" );
    fprintf( fp, "      \"type\": \"pickString\",\n" );
    fprintf( fp, "      \"description\": \"Select target\",\n" );
    fprintf( fp, "      \"options\": [\n" );
    for ( int i = 0; i < m->local_target_count; ++i )
        fprintf( fp, "        \"%s\"%s\n",
                 m->local_targets[ i ]->name,
                 ( i < m->local_target_count - 1 ) ? "," : "" );
    fprintf( fp, "      ]\n" );
    fprintf( fp, "    }\n" );
    fprintf( fp, "  ]\n" );
    fprintf( fp, "}\n" );

    fclose( fp );
    printf( "Generated .vscode/tasks.json (%d targets in picker)\n", m->local_target_count );

    // launch.json: one config per local exe target.
    fp = fopen( ".vscode/launch.json", "w" );
    if ( fp )
    {
        fprintf( fp, "{\n" );
        fprintf( fp, "  \"version\": \"0.2.0\",\n" );
        fprintf( fp, "  \"configurations\": [\n" );

        for ( int i = 0; i < m->exe_target_count; ++i )
        {
            target_info_t* t = m->exe_targets[ i ];
            fprintf( fp, "    {\n" );
            fprintf( fp, "      \"name\": \"%s\",\n", t->name );
            fprintf( fp, "      \"type\": \"cppvsdbg\",\n" );
            fprintf( fp, "      \"request\": \"launch\",\n" );
            fprintf( fp, "      \"program\": \"${workspaceFolder}/bin/%s.exe\",\n", t->name );
            fprintf( fp, "      \"args\": [],\n" );
            fprintf( fp, "      \"cwd\": \"${workspaceFolder}\",\n" );
            fprintf( fp, "      \"console\": \"integratedTerminal\",\n" );
            fprintf( fp, "      \"preLaunchTask\": \"orb: build debug\"\n" );
            fprintf( fp, "    }%s\n", ( i < m->exe_target_count - 1 ) ? "," : "" );
        }

        fprintf( fp, "  ]\n" );
        fprintf( fp, "}\n" );
        fclose( fp );
        printf( "Generated .vscode/launch.json (%d configurations)\n", m->exe_target_count );
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
        fprintf( fp, "Diagnostics:\n" );
        fprintf( fp, "  UnusedIncludes: None\n" );
        fprintf( fp, "CompileFlags:\n" );
        if ( m->engine_src_dir[ 0 ] )
        {
            fprintf( fp, "  Add: [--target=x86_64-pc-windows-msvc, -std=c11, -Isource, -Ibuild/generated,\n" );
            fprintf( fp, "        -I%s, -I%s,\n", m->engine_src_dir, m->engine_gen_dir );
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

    // .code-workspace: root folder plus one entry per external target referenced in any local
    // solution. Emitted for any project with local solutions (not just child projects).
    if ( m->solution_count > 0 )
    {
        char ws_path[ PATH_MAX ];
        snprintf( ws_path, sizeof( ws_path ), "%s.code-workspace", m->workspace_name );

        fp = fopen( ws_path, "w" );
        if ( fp )
        {
            fprintf( fp, "{\n" );
            fprintf( fp, "  \"folders\": [\n" );
            fprintf( fp, "    { \"name\": \"%s\", \"path\": \".\" }", m->workspace_name );
            for ( int i = 0; i < m->ext_ref_target_count; ++i )
            {
                char fwd[ PATH_MAX ];
                snprintf( fwd, sizeof( fwd ), "%s", m->ext_ref_targets[ i ]->root_dir );
                for ( char* p = fwd; *p; ++p ) if ( *p == '\\' ) *p = '/';
                fprintf( fp, ",\n    { \"name\": \"%s\", \"path\": \"%s\" }",
                         m->ext_ref_targets[ i ]->name, fwd );
            }
            fprintf( fp, "\n  ]\n" );
            fprintf( fp, "}\n" );
            fclose( fp );
            if ( m->ext_ref_target_count > 0 )
                printf( "Generated %s (%d external folders)\n", ws_path, m->ext_ref_target_count );
            else
                printf( "Generated %s\n", ws_path );
        }
        else
        {
            printf( ORB_INDENT "[orb error] could not write %s\n", ws_path );
        }
    }
}

/*============================================================================================*/
