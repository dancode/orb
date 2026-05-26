/*==============================================================================================

    build_tool_11_gen_compdb.c -- compile_commands.json generator.

    Emits a JSON Compilation Database at the project root (compile_commands.json).
    Tools that speak clangd -- VS Code, Vim/Neovim, Emacs, JetBrains CLion -- use
    this file for accurate IntelliSense, completion, and cross-reference navigation.

    One entry is emitted per unity compilation unit (each entry in target->units[]).
    Targets with multiple unity modules each contribute one entry per module.
    Targets with has_reflect=true also get an entry for the reflection-generated .c
    file; that file may not exist until the first build, but clangd picks it up once
    it does.

    The compile command reuses the same flag/define/include tables as 06_compile.c
    and 11_gen.c, so the database always matches actual build behavior. Debug config
    is always used (most useful for development IntelliSense). The output flags
    (/Fo /Fd) are omitted because cc_fill_compile_cmd() leaves cc->output empty
    -- those are set by the compile entry points, not by the shared fill function.

==============================================================================================*/

/*==============================================================================================
    compdb_json_str()

    Write a JSON-encoded string with surrounding quotes to fp.
    Escapes backslashes and double-quotes; all other characters pass through unchanged
    because compile flags and paths are 7-bit ASCII.
==============================================================================================*/

static void
compdb_json_str( FILE* fp, const char* s )
{
    fputc( '"', fp );
    for ( ; *s; ++s )
    {
        if ( *s == '\\' ) { fputs( "\\\\", fp ); continue; }
        if ( *s == '"'  ) { fputs( "\\\"", fp ); continue; }
        fputc( *s, fp );
    }
    fputc( '"', fp );
}

/*==============================================================================================
    compdb_fwd_slashes()

    Convert backslashes to forward slashes in a path buffer in-place.
    cl.exe accepts both; forward slashes avoid backslash double-escaping in JSON.
==============================================================================================*/

static void
compdb_fwd_slashes( char* s )
{
    for ( ; *s; ++s )
        if ( *s == '\\' ) *s = '/';
}

/*==============================================================================================
    compdb_emit_entry()

    Write one compile_commands.json entry to fp using the "command" string form.
    No trailing comma or newline -- the caller manages entry separation.

    command = exe + flags + includes + defines + source_path
    The /Fo /Fd output flags are intentionally absent (cc->output is empty here).
==============================================================================================*/

static void
compdb_emit_entry( FILE* fp, const char* root_abs, const compile_cmd_t* cc, const char* abs_src )
{
    char cmd[ 4096 ];
    snprintf( cmd, sizeof( cmd ), "%s %s %s %s %s",
              cc->exe, cc->flags, cc->includes, cc->defines, abs_src );
    compdb_fwd_slashes( cmd );

    fprintf( fp, "  {\n" );
    fprintf( fp, "    \"directory\": " ); compdb_json_str( fp, root_abs ); fprintf( fp, ",\n" );
    fprintf( fp, "    \"file\": "      ); compdb_json_str( fp, abs_src  ); fprintf( fp, ",\n" );
    fprintf( fp, "    \"command\": "   ); compdb_json_str( fp, cmd       ); fprintf( fp, "\n" );
    fprintf( fp, "  }" );
}

/*==============================================================================================
    build_gen_compile_commands()

    Write compile_commands.json to the project root. Called from main() as part
    of the -gen command after build_gen_projects().
==============================================================================================*/

void
build_gen_compile_commands( void )
{
    const char* out_path = "compile_commands.json";
    FILE* fp = fopen( out_path, "w" );
    if ( !fp )
    {
        printf( ORB_INDENT "[orb error] could not write %s\n", out_path );
        return;
    }

    // Absolute project root for the "directory" field in every entry.
    // GetFullPathNameA (via platform_fullpath) resolves "." without requiring it to exist.
    char root_abs[ PATH_MAX ];
    if ( !platform_fullpath( root_abs, ".", sizeof( root_abs ) ) )
        snprintf( root_abs, sizeof( root_abs ), "." );
    compdb_fwd_slashes( root_abs );

    // Include path for generated headers: "build/generated".
    char gen_dir[ PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s/%s", g_build_dir, g_gen_dir );

    // Use clang-cl for the database: clangd has native support for clang-cl.exe commands
    // whereas cl.exe goes through an imperfect translation layer that can break hub
    // indexing and prevent include-context propagation to constituent .c files.
    // clang-cl.exe accepts all MSVC-style flags (/I, /D, /std:c11) and adds
    // --target=x86_64-pc-windows-msvc so _MSC_VER and MSVC extensions are defined.
    // clangd reads the command but does not execute it, so clang-cl.exe need not be
    // installed -- only the flags matter for IntelliSense.
    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;
    ctx.compiler = COMPILE_CLANG;

    int  entry_count = 0;
    bool first       = true;

    fprintf( fp, "[\n" );

    for ( int i = 0; i < g_target_count; ++i )
    {
        target_info_t* target = &g_targets[ i ];
        if ( !target->units[ 0 ] ) continue;

        // Build command skeleton: exe, flags, includes, defines.
        // obj_dir is unused inside cc_fill_compile_cmd (output is caller-set); "obj" is a placeholder.
        compile_cmd_t cc = { 0 };
        cc_fill_compile_cmd( &ctx, target, "obj", gen_dir, &cc );

        // One entry per unity unit. Constituent .c files that are #included by the
        // hub are not listed -- clangd finds the hub that includes them and transfers
        // its compilation context (includes, defines) automatically.
        for ( int j = 0; target->units[ j ]; ++j )
        {
            char rel[ PATH_MAX ];
            snprintf( rel, sizeof( rel ), "%s/%s", target->root_dir, target->units[ j ] );

            char abs_src[ PATH_MAX ];
            if ( !platform_fullpath( abs_src, rel, sizeof( abs_src ) ) )
                snprintf( abs_src, sizeof( abs_src ), "%s", rel );
            compdb_fwd_slashes( abs_src );

            if ( !first ) fprintf( fp, ",\n" );
            first = false;
            compdb_emit_entry( fp, root_abs, &cc, abs_src );
            entry_count++;
        }

        // Entry for the reflection-generated .c. May not exist until the first build;
        // clangd silently skips missing files and indexes them once they appear.
        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;

            char rel[ PATH_MAX ];
            snprintf( rel, sizeof( rel ), "%s/%s/%s.generated.c",
                      g_build_dir, g_gen_dir, rname );

            char abs_src[ PATH_MAX ];
            if ( !platform_fullpath( abs_src, rel, sizeof( abs_src ) ) )
                snprintf( abs_src, sizeof( abs_src ), "%s", rel );
            compdb_fwd_slashes( abs_src );

            if ( !first ) fprintf( fp, ",\n" );
            first = false;
            compdb_emit_entry( fp, root_abs, &cc, abs_src );
            entry_count++;
        }
    }

    fprintf( fp, "\n]\n" );
    fclose( fp );

    printf( "Generated compile_commands.json (%d entries)\n", entry_count );
}

/*============================================================================================*/
