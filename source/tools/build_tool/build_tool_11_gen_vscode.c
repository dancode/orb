/*==============================================================================================

    build_tool_11_gen_vscode.c -- VS Code workspace configuration generator.

    Emits .vscode/tasks.json with build tasks wired to build_tool.exe.
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

// VS Code workspace-relative exe path and cwd, emitted as JSON string literals.
// ${workspaceFolder} is substituted by VS Code at task run time.
// \\\\  in C  ->  \\  in file  ->  \  in JSON string  ->  \  in path.

#define VSCODE_EXE  "\"${workspaceFolder}\\\\bin\\\\build_tool.exe\""
#define VSCODE_CWD  "\"${workspaceFolder}\""

/*==============================================================================================
    vsc_task()

    Emit one task block (without a leading or trailing comma -- the caller manages
    entry separation). with_matcher adds the $msCompile problemMatcher which parses
    cl.exe error output into VS Code inline diagnostics.
==============================================================================================*/

static void
vsc_task( FILE* fp, const char* label, const char* args, bool with_matcher, bool is_default )
{
    fprintf( fp, "    {\n" );
    fprintf( fp, "      \"label\": \"%s\",\n", label );
    fprintf( fp, "      \"type\": \"process\",\n" );
    fprintf( fp, "      \"command\": " VSCODE_EXE ",\n" );
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

    vsc_task( fp, "orb: build debug",
              "\"-config\", \"Debug\"",
              true, true );
    fprintf( fp, ",\n" );

    vsc_task( fp, "orb: build release",
              "\"-config\", \"Release\"",
              true, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, "orb: build target",
              "\"-config\", \"Debug\", \"-target\", \"${input:targetName}\"",
              true, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, "orb: clean",
              "\"-clean\"",
              false, false );
    fprintf( fp, ",\n" );

    vsc_task( fp, "orb: regen",
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
    for ( int i = 0; i < g_target_count; ++i )
    {
        fprintf( fp, "        \"%s\"%s\n",
                 g_targets[ i ].name,
                 ( i < g_target_count - 1 ) ? "," : "" );
    }
    fprintf( fp, "      ]\n" );
    fprintf( fp, "    }\n" );
    fprintf( fp, "  ]\n" );
    fprintf( fp, "}\n" );

    fclose( fp );
    printf( "Generated .vscode/tasks.json (%d targets in picker)\n", g_target_count );

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
        fprintf( fp, "  Add: [--target=x86_64-pc-windows-msvc, -std=c11, -Isource, -Ibuild/generated,\n" );
        fprintf( fp, "        -D_CRT_SECURE_NO_WARNINGS, -D_DEBUG, -Wno-unused-function]\n" );
        fclose( fp );
        printf( "Generated .clangd\n" );
    }
    else
    {
        printf( ORB_INDENT "[orb error] could not write .clangd\n" );
    }
}

/*============================================================================================*/
