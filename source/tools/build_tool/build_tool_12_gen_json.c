/*==============================================================================================

    build_tool_12_gen_json.c -- compile_commands.json generator.

    Emits a JSON Compilation Database at the project root (compile_commands.json).
    Tools that speak clangd -- VS Code, Vim/Neovim, Emacs, JetBrains CLion -- use
    this file for accurate IntelliSense, completion, and cross-reference navigation.

    IMPORTANT: the commands in this file are NEVER executed.  Clangd reads only
    the flags (includes, defines, target triple) to model the compilation.  The
    executable does not need to exist; we use clang-cl format because clangd has
    native support for it and avoids an imperfect cl.exe translation layer.

    Entries emitted per target:
      - One per unity compilation unit (target->units[]).
      - One per constituent .c file found via #include "*.c" in each unity unit.
        This gives clangd a direct per-file context without relying on context
        propagation, which can fail for files included via -I-resolved paths
        (e.g. #include "engine/sys/sys_api.c" from source/engine/sys/sys.c).
      - One for the reflection-generated .c (if target->has_reflect).

    The compile command reuses the same flag/define/include tables as 06_compile.c
    and 11_gen.c, so the database always matches actual build behavior.  Debug
    config is always used (most useful for development IntelliSense).  The output
    flags (/Fo /Fd) are omitted -- cc->output is empty in this context.

    Constituent file path resolution (json_emit_constituents):
      For each #include "path.c" found in a unity file, tries:
        1. root_dir/path   -- include relative to the unity file's directory
        2. source/path     -- include resolved through the -Isource search path
      Skips silently if neither resolves to an existing file.

==============================================================================================*/

/*==============================================================================================
    json_str()

    Write a JSON-encoded string with surrounding quotes to fp.
    Escapes backslashes and double-quotes; all other characters pass through unchanged
    because compile flags and paths are 7-bit ASCII.
==============================================================================================*/

static void
json_str( FILE* fp, const char* s )
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
    json_fwd_slashes()

    Convert backslashes to forward slashes in a path buffer in-place.
    cl.exe accepts both; forward slashes avoid backslash double-escaping in JSON.
==============================================================================================*/

static void
json_fwd_slashes( char* s )
{
    for ( ; *s; ++s )
        if ( *s == '\\' ) *s = '/';
}

/*==============================================================================================
    json_emit_entry()

    Write one compile_commands.json entry to fp using the "command" string form.
    No trailing comma or newline -- the caller manages entry separation.

    command = exe + flags + includes + defines + source_path
    The /Fo /Fd output flags are intentionally absent (cc->output is empty here).
==============================================================================================*/

static void
json_emit_entry( FILE* fp, const char* root_abs, const compile_cmd_t* cc, const char* abs_src )
{
    char cmd[ 4096 ];
    snprintf( cmd, sizeof( cmd ), "%s %s %s %s %s",
              cc->exe, cc->flags, cc->includes, cc->defines, abs_src );
    json_fwd_slashes( cmd );

    fprintf( fp, "  {\n" );
    fprintf( fp, "    \"directory\": " ); json_str( fp, root_abs ); fprintf( fp, ",\n" );
    fprintf( fp, "    \"file\": "      ); json_str( fp, abs_src  ); fprintf( fp, ",\n" );
    fprintf( fp, "    \"command\": "   ); json_str( fp, cmd       ); fprintf( fp, "\n" );
    fprintf( fp, "  }" );
}

/*==============================================================================================
    json_emit_constituents_from()

    Scan scan_path for every #include "*.c" directive, emit a compile_commands.json
    entry for each constituent found, then recurse into that constituent to pick up
    any further nested .c includes (sub-unity files like core_sid.c -> sid/sid.c).

    root_dir stays fixed at the target root throughout recursion; all include paths
    are resolved against it first, then against source/ as a fallback.

    depth guards against cycles; unity nesting in this codebase is at most 2 levels
    deep, so a limit of 8 is generous.

    json_emit_constituents() is a thin wrapper that builds the initial scan_path
    from root_dir + unit and calls this function at depth 0.
==============================================================================================*/

/*==============================================================================================
    json_prelude_macro()

    Derive the prelude-guard macro name from an _api.c constituent's absolute path.
    The generated macro lets the unity file exclude that include when the constituent
    is compiled as the main file, preventing the double-definition that corrupts
    clangd's type recovery and produces spurious squiggles in the _api.h header.

    net_api.c  ->  NET_API_C_PRELUDE
    sys_api.c  ->  SYS_API_C_PRELUDE
==============================================================================================*/

static void
json_prelude_macro( const char* abs_src, char* out, size_t out_size )
{
    const char* base = strrchr( abs_src, '/' );
    base = base ? base + 1 : abs_src;

    size_t i = 0;
    for ( const char* p = base; *p && i < out_size - 10; p++, i++ )
    {
        if      ( *p == '.' )              out[ i ] = '_';
        else if ( *p >= 'a' && *p <= 'z' ) out[ i ] = (char)( *p - 'a' + 'A' );
        else                               out[ i ] = *p;
    }
    if ( i + 9 <= out_size ) { memcpy( out + i, "_PRELUDE", 8 ); i += 8; }
    out[ i ] = '\0';
}

static void
json_emit_constituents_from( FILE* fp, bool* first,
                                const char* root_abs, const compile_cmd_t* cc,
                                const char* root_dir, const char* scan_path,
                                int* entry_count, int depth )
{
    if ( depth > 8 ) return;

    FILE* in = fopen( scan_path, "r" );
    if ( !in ) return;

    char line[ 1024 ];
    const char* p;
    while ( ( p = fgets( line, sizeof( line ), in ) ) != NULL )
    {
        /* detect #include "*.c" with arbitrary whitespace after # */
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( *p != '#' ) continue;
        p++;
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( strncmp( p, "include", 7 ) != 0 ) continue;
        p += 7;
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( *p != '"' ) continue;                  /* skip angle-bracket system includes */
        const char* q = p + 1;
        while ( *q && *q != '"' ) q++;
        if ( *q != '"' || q == p + 1 ) continue;
        if ( !( q >= p + 3 && q[ -1 ] == 'c' && q[ -2 ] == '.' ) ) continue;

        /* extract the bare include path */
        size_t path_len = (size_t)( q - ( p + 1 ) );
        if ( path_len == 0 || path_len >= PATH_MAX - 1 ) continue;
        char inc_path[ PATH_MAX ];
        memcpy( inc_path, p + 1, path_len );
        inc_path[ path_len ] = '\0';

        /* step 1: resolve relative to root_dir (target root, fixed across recursion) */
        char rel[ PATH_MAX ];
        snprintf( rel, sizeof( rel ), "%s/%s", root_dir, inc_path );
        if ( platform_get_mtime( rel ) == 0 )
        {
            /* step 2: resolve relative to the source root (via -Isource) */
            snprintf( rel, sizeof( rel ), "source/%s", inc_path );
            if ( platform_get_mtime( rel ) == 0 )
                continue;   /* unresolvable -- skip */
        }

        char abs_src[ PATH_MAX ];
        if ( !platform_fullpath( abs_src, rel, sizeof( abs_src ) ) )
            snprintf( abs_src, sizeof( abs_src ), "%s", rel );
        json_fwd_slashes( abs_src );

        /* _api.c constituents define the g_<name>_api_struct that MOD_GATEWAY_STATIC
           declares in the matching _api.h.  The unity prelude (/FI net.c) already
           includes net_api.c, so compiling it again as the main file produces a
           double-definition that corrupts clangd's type recovery and squiggles
           the _api.h.  Inject a per-file guard define so the unity file can wrap
           its #include with #ifndef NET_API_C_PRELUDE ... #endif. */
        const char* base = strrchr( abs_src, '/' );
        base = base ? base + 1 : abs_src;
        size_t base_len = strlen( base );
        bool is_api_c   = ( base_len > 6 && strcmp( base + base_len - 6, "_api.c" ) == 0 );

        if ( !*first ) fprintf( fp, ",\n" );
        *first = false;
        if ( is_api_c )
        {
            compile_cmd_t cc_api = *cc;
            char macro[ 64 ];
            json_prelude_macro( abs_src, macro, sizeof( macro ) );
            size_t used = strlen( cc_api.defines );
            snprintf( cc_api.defines + used, sizeof( cc_api.defines ) - used,
                      " /D%s", macro );
            json_emit_entry( fp, root_abs, &cc_api, abs_src );
        }
        else
        {
            json_emit_entry( fp, root_abs, cc, abs_src );
        }
        ( *entry_count )++;

        /* Recurse: if this constituent is itself a sub-unity, emit its children too. */
        json_emit_constituents_from( fp, first, root_abs, cc, root_dir, rel,
                                     entry_count, depth + 1 );
    }

    fclose( in );
}

static void
json_emit_constituents( FILE* fp, bool* first,
                        const char* root_abs, const compile_cmd_t* cc,
                        const char* root_dir, const char* unit,
                        int* entry_count )
{
    char unity_path[ PATH_MAX ];
    snprintf( unity_path, sizeof( unity_path ), "%s/%s", root_dir, unit );
    json_emit_constituents_from( fp, first, root_abs, cc, root_dir, unity_path,
                                 entry_count, 0 );
}

/*==============================================================================================
    build_gen_compile_commands()

    Write compile_commands.json to the project root. Called from main() as part
    of the -gen command after build_gen_projects().
==============================================================================================*/

void
build_gen_compile_commands( const gen_manifest_t* m )
{    
    const char* out_path = "compile_commands.json";
    FILE* fp = fopen( out_path, "w" );
    if ( !fp )
    {
        printf( ORB_INDENT "[orb error] could not write %s\n", out_path );
        return;
    }

    /* Absolute project root for the "directory" field in every entry.
       All relative paths in compile commands (like -Isource) are resolved from here. */
    char root_abs[ PATH_MAX ];
    if ( !platform_fullpath( root_abs, ".", sizeof( root_abs ) ) )
        snprintf( root_abs, sizeof( root_abs ), "." );
    json_fwd_slashes( root_abs );

    /* Include path for reflection-generated headers: "build/generated". */
    char gen_dir[ PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s/%s", g_build_dir, g_gen_dir );

    /* Use clang-cl for the database: clangd has native support for clang-cl.exe commands
       whereas cl.exe goes through an imperfect translation layer that can break indexing.
       clang-cl.exe accepts all MSVC-style flags and adds --target=x86_64-pc-windows-msvc
       so _MSC_VER and MSVC extensions are defined.  The commands are never executed --
       clangd reads the flags only. */
       
    build_context_t ctx = { 0 };
    ctx.config   = CONFIG_DEBUG;
    ctx.compiler = COMPILE_CLANG;

    int  entry_count = 0;
    bool first       = true;

    fprintf( fp, "[\n" );

    for ( int i = 0; i < m->local_target_count; ++i )
    {
        target_info_t* target = m->local_targets[ i ];
        if ( !target->units[ 0 ] ) continue;

        /* Build command skeleton: exe, flags, includes, defines.
           obj_dir is unused inside cc_fill_compile_cmd; "obj" is a placeholder. */
        compile_cmd_t cc_entry = { 0 };
        cc_fill_compile_cmd( &ctx, target, "obj", gen_dir, &cc_entry );

        /* Constituent command force-includes the unity entry via /FI.
           This gives clangd the full preamble and all sibling constituent definitions
           before compiling the constituent as the main file -- F12 and go-to-
           implementation resolve correctly because every static is genuinely in scope.
           Do NOT use -include: in --driver-mode=cl clangd drops the path token silently. */
        compile_cmd_t cc_constituent = cc_entry;
        {
            char unity_path[ PATH_MAX ];
            snprintf( unity_path, sizeof( unity_path ), "%s/%s",
                      target->root_dir, target->units[ 0 ] );
            json_fwd_slashes( unity_path );

            size_t used = strlen( cc_constituent.includes );
            snprintf( cc_constituent.includes + used, sizeof( cc_constituent.includes ) - used,
                      " /FI %s", unity_path );
        }
        {
            size_t used = strlen( cc_constituent.flags );
            snprintf( cc_constituent.flags + used, sizeof( cc_constituent.flags ) - used,
                      " -Wno-unused-function -Wno-undefined-internal" );
        }

        /* --- Unity entry: one entry per compilation unit --- */
        for ( int j = 0; target->units[ j ]; ++j )
        {
            char rel[ PATH_MAX ];
            snprintf( rel, sizeof( rel ), "%s/%s", target->root_dir, target->units[ j ] );

            char abs_src[ PATH_MAX ];
            if ( !platform_fullpath( abs_src, rel, sizeof( abs_src ) ) )
                snprintf( abs_src, sizeof( abs_src ), "%s", rel );
            json_fwd_slashes( abs_src );

            if ( !first ) fprintf( fp, ",\n" );
            first = false;
            json_emit_entry( fp, root_abs, &cc_entry, abs_src );
            entry_count++;
        }

        /* --- Constituent entries: one per #included .c file in each unit ---
           Explicit entries give clangd direct per-file context without relying on
           context propagation, which fails for includes resolved via -I paths. */
        for ( int j = 0; target->units[ j ]; ++j )
            json_emit_constituents( fp, &first, root_abs, &cc_constituent,
                                    target->root_dir, target->units[ j ],
                                    &entry_count );

        /* --- Reflection-generated .c ---
           May not exist until the first build; clangd silently skips missing files
           and indexes them once they appear. */
        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;

            char rel[ PATH_MAX ];
            snprintf( rel, sizeof( rel ), "%s/%s/%s.generated.c",
                      g_build_dir, g_gen_dir, rname );

            char abs_src[ PATH_MAX ];
            if ( !platform_fullpath( abs_src, rel, sizeof( abs_src ) ) )
                snprintf( abs_src, sizeof( abs_src ), "%s", rel );
            json_fwd_slashes( abs_src );

            if ( !first ) fprintf( fp, ",\n" );
            first = false;
            json_emit_entry( fp, root_abs, &cc_constituent, abs_src );
            entry_count++;
        }
    }

    fprintf( fp, "\n]\n" );
    fclose( fp );

    printf( "Generated compile_commands.json (%d entries)\n", entry_count );
}

/*============================================================================================*/
