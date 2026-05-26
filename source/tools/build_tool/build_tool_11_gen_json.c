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
    compdb_scan_dir()

    Recursively walk dir and emit one compile_commands.json entry for every .c
    file found, using the pre-built compile_cmd_t for the owning target.
    This covers both unity entry points and all constituent .c files so clangd
    has exact flags (includes, defines) for every file it opens.
==============================================================================================*/

static void
compdb_scan_dir( FILE* fp, const char* root_abs, const compile_cmd_t* cc,
                 const char* dir, bool* first, int* count )
{
    char search_path[ PATH_MAX ];
    snprintf( search_path, sizeof( search_path ), "%s" PATH_SEP "*", dir );

    platform_find_data_t fd;
    platform_find_t h = platform_find_first( search_path, &fd );
    if ( h == PLATFORM_FIND_INVALID )
        return;

    do
    {
        if ( strcmp( fd.name, "." ) == 0 || strcmp( fd.name, ".." ) == 0 )
            continue;

        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "%s" PATH_SEP "%s", dir, fd.name );

        if ( fd.is_dir )
        {
            compdb_scan_dir( fp, root_abs, cc, path, first, count );
            continue;
        }

        const char* ext = strrchr( fd.name, '.' );
        if ( !ext || platform_stricmp( ext, ".c" ) != 0 )
            continue;

        char abs_src[ PATH_MAX ];
        if ( !platform_fullpath( abs_src, path, sizeof( abs_src ) ) )
            snprintf( abs_src, sizeof( abs_src ), "%s", path );
        compdb_fwd_slashes( abs_src );

        if ( !*first ) fprintf( fp, ",\n" );
        *first = false;
        compdb_emit_entry( fp, root_abs, cc, abs_src );
        ( *count )++;
    }
    while ( platform_find_next( h, &fd ) );

    platform_find_close( h );
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

    // Debug context: _DEBUG defined, /Od, most relevant for development IntelliSense.
    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;
    ctx.compiler = COMPILE_MSVC;

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

        // Scan root_dir recursively -- every .c file gets an entry with this target's
        // exact flags so clangd has full context for unity entry points and all
        // constituent files that are #included into them.
        compdb_scan_dir( fp, root_abs, &cc, target->root_dir, &first, &entry_count );

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
