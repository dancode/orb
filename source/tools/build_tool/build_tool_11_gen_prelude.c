/*==============================================================================================

    build_tool_11_gen_prelude.c -- Unity-prelude header generator.

    For each target that has unity compilation units, reads the unity entry .c
    file and copies all preprocessor setup lines (everything before the first
    constituent .c #include) into build/generated/<target>.prelude.h.

    Delivery is exclusively through compile_commands.json: each entry in that
    database has -include <name>.prelude.h injected by build_tool_11_gen_json.c.
    This gives every constituent .c file the correct compilation context without
    any reliance on .clangd, VS Code settings, or file associations -- all of
    which are UI-layer configuration that cannot model compilation context.

    Lines filtered from the preamble copy:
      #pragma comment(lib, ...)  -- linker directive, invalid outside a full TU

    Forward declarations of static functions in constituent files are appended
    after the preamble.  This lets cross-constituent calls resolve in the IDE
    without needing the full unity TU in view.  Public functions need no
    treatment here -- they are already declared in headers.

    Heuristic used (relies on BreakBeforeBraces: Custom in .clang-format):
      static <return-type>        <- starts collection
      function_name( params )     <- paren depth returns to 0
      {                           <- lone brace confirms function, not variable
    Variables are rejected when = appears before ( or ; closes before {.

==============================================================================================*/

/*==============================================================================================
    --- Feature Control ---

    Set s_gen_preludes = false to skip all prelude generation during -gen.
    The boundary of this feature is this file plus the -include injection in
    build_tool_11_gen_json.c.  Nothing else participates.
==============================================================================================*/

static bool s_gen_preludes = true;

/*==============================================================================================
    --- Preamble Extraction Helpers ---
==============================================================================================*/

/* Return true if line is a #include of a .c source file -- a unity constituent include. */
static bool
prelude_is_c_include( const char* line )
{
    const char* p = line;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( *p != '#' ) return false;
    p++;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( strncmp( p, "include", 7 ) != 0 ) return false;
    p += 7;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( *p != '"' ) return false;
    const char* q = p + 1;
    while ( *q && *q != '"' ) q++;
    if ( *q != '"' || q == p + 1 ) return false;
    return q >= p + 3 && q[ -1 ] == 'c' && q[ -2 ] == '.';
}

/* Return true if line is a #pragma comment(lib, ...) linker directive. */
static bool
prelude_is_pragma_comment( const char* line )
{
    const char* p = line;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( *p != '#' ) return false;
    p++;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( strncmp( p, "pragma", 6 ) != 0 ) return false;
    return strstr( p + 6, "comment" ) != NULL;
}

/* Return true if line opens a new conditional block: #if, #ifdef, #ifndef. */
static bool
prelude_opens_if( const char* line )
{
    const char* p = line;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( *p != '#' ) return false;
    p++;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( strncmp( p, "ifdef",  5 ) == 0 ) return true;
    if ( strncmp( p, "ifndef", 6 ) == 0 ) return true;
    return strncmp( p, "if", 2 ) == 0 && ( p[ 2 ] == ' ' || p[ 2 ] == '\t' ||
                                            p[ 2 ] == '\r' || p[ 2 ] == '\n' );
}

/* Return true if line closes a conditional block: #endif. */
static bool
prelude_closes_if( const char* line )
{
    const char* p = line;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( *p != '#' ) return false;
    p++;
    while ( *p == ' ' || *p == '\t' ) p++;
    return strncmp( p, "endif", 5 ) == 0;
}

/*==============================================================================================
    --- Forward Declaration Helpers ---
==============================================================================================*/

/* Scan src_path for static function definitions and emit a forward declaration
   for each one found.  Relies on BreakBeforeBraces: lone { on its own line
   marks the start of a function body.

   Rejection rules (variable, not function):
     - = appears before first ( on any accumulated line
     - ; terminates a line while paren_depth == 0 and before a lone { is seen  */
static void
prelude_fwd_decl_file( FILE* out_fp, const char* src_path )
{
    platform_mapped_file_t mf;
    if ( !platform_map_file( src_path, &mf ) ) return;

    char        line[ 1024 ];
    char        sig[ 4096 ];
    int         sig_len     = 0;
    bool        collecting  = false;
    bool        seen_paren  = false;
    int         paren_depth = 0;
    const char* p           = mf.data;
    const char* end         = mf.data ? mf.data + mf.size : NULL;

    while ( p && p < end )
    {
        const char* nl  = (const char*)memchr( p, '\n', (size_t)( end - p ) );
        size_t      len = nl ? (size_t)( nl - p ) : (size_t)( end - p );
        if ( len > 0 && p[ len - 1 ] == '\r' ) len--;
        if ( len >= sizeof( line ) ) len = sizeof( line ) - 1;
        memcpy( line, p, len );
        line[ len ] = '\0';
        p = nl ? nl + 1 : end;

        const char* q = line;
        while ( *q == ' ' || *q == '\t' ) q++;

        if ( !collecting )
        {
            if ( strncmp( q, "static ", 7 ) != 0 ) continue;
            collecting  = true;
            seen_paren  = false;
            paren_depth = 0;
            sig_len     = 0;
            sig[ 0 ]    = '\0';
        }

        /* = before first ( means variable initializer -- abandon */
        if ( !seen_paren )
        {
            const char* eq = strchr( q, '=' );
            const char* lp = strchr( q, '(' );
            if ( eq && ( !lp || eq < lp ) ) { collecting = false; continue; }
        }

        /* ; at paren_depth 0 (before any lone {) means variable decl -- abandon */
        if ( paren_depth == 0 )
        {
            bool has_semi = false;
            for ( const char* r = q; *r; r++ )
            {
                if ( *r == '(' ) break;
                if ( *r == ';' ) { has_semi = true; break; }
            }
            if ( has_semi ) { collecting = false; continue; }
        }

        /* Lone { confirms function definition -- emit forward declaration */
        if ( q[ 0 ] == '{' && q[ 1 ] == '\0' )
        {
            if ( seen_paren && paren_depth == 0 )
            {
                while ( sig_len > 0 &&
                        ( sig[ sig_len - 1 ] == '\n' || sig[ sig_len - 1 ] == ' ' ||
                          sig[ sig_len - 1 ] == '\t' ) )
                    sig[ --sig_len ] = '\0';
                fprintf( out_fp, "%s;\n", sig );
            }
            collecting = false;
            continue;
        }

        /* Accumulate line into sig (with newline so multi-line sigs stay readable) */
        {
            int n    = (int)len + 1;   /* +1 for the \n we append */
            int room = (int)sizeof( sig ) - sig_len - 1;
            if ( n > room ) n = room;
            if ( n > 0 )
            {
                memcpy( sig + sig_len, line, (size_t)( n - 1 ) );
                sig_len += n - 1;
                if ( room > 1 ) sig[ sig_len++ ] = '\n';
                sig[ sig_len ] = '\0';
            }
        }

        /* Track paren depth */
        for ( const char* r = line; *r; r++ )
        {
            if      ( *r == '(' ) { paren_depth++; seen_paren = true; }
            else if ( *r == ')' )   paren_depth--;
        }
    }

    platform_unmap_file( &mf );
}

/* Scan unity_path for every #include "*.c" constituent, resolve each path
   (root_dir-relative then source/-relative), and emit forward declarations
   for all static functions found in those files. */
static void
prelude_fwd_decl_constituents( FILE* out_fp, const char* root_dir, const char* unit )
{
    char unity_path[ PATH_MAX ];
    snprintf( unity_path, sizeof( unity_path ), "%s/%s", root_dir, unit );

    platform_mapped_file_t mf;
    if ( !platform_map_file( unity_path, &mf ) ) return;

    bool        emitted_header = false;
    char        line[ 1024 ];
    const char* p              = mf.data;
    const char* end            = mf.data ? mf.data + mf.size : NULL;

    while ( p && p < end )
    {
        const char* nl  = (const char*)memchr( p, '\n', (size_t)( end - p ) );
        size_t      len = nl ? (size_t)( nl - p ) : (size_t)( end - p );
        if ( len > 0 && p[ len - 1 ] == '\r' ) len--;
        if ( len >= sizeof( line ) ) len = sizeof( line ) - 1;
        memcpy( line, p, len );
        line[ len ] = '\0';
        p = nl ? nl + 1 : end;

        if ( !prelude_is_c_include( line ) ) continue;

        /* Extract the bare include path from #include "path.c" */
        const char* q = line;
        while ( *q == ' ' || *q == '\t' ) q++;
        q++;                                    /* skip # */
        while ( *q == ' ' || *q == '\t' ) q++;
        q += 7;                                 /* skip "include" */
        while ( *q == ' ' || *q == '\t' ) q++;
        q++;                                    /* skip opening " */
        const char* r = q;
        while ( *r && *r != '"' ) r++;
        if ( *r != '"' ) continue;

        size_t path_len = (size_t)( r - q );
        if ( path_len == 0 || path_len >= PATH_MAX - 1 ) continue;
        char inc_path[ PATH_MAX ];
        memcpy( inc_path, q, path_len );
        inc_path[ path_len ] = '\0';

        /* Two-step resolution: root_dir-relative then source/-relative */
        char resolved[ PATH_MAX ];
        snprintf( resolved, sizeof( resolved ), "%s/%s", root_dir, inc_path );
        if ( build_get_mtime( resolved ) == 0 )
        {
            snprintf( resolved, sizeof( resolved ), "source/%s", inc_path );
            if ( build_get_mtime( resolved ) == 0 ) continue;
        }

        if ( !emitted_header )
        {
            fprintf( out_fp, "\n/* forward declarations of static functions in constituent files */\n" );
            emitted_header = true;
        }

        prelude_fwd_decl_file( out_fp, resolved );
    }

    platform_unmap_file( &mf );
}

/*==============================================================================================
    prelude_write_preamble()

    Open the unity entry at unity_path and copy every line from the first
    preprocessor directive (#) up to, but not including, the first constituent
    .c #include.  The opening file-header block comment is skipped.

    Tracks #if nesting depth; emits closing #endif lines for any blocks that
    were opened in the preamble but not yet closed at the stop boundary.
==============================================================================================*/

static void
prelude_write_preamble( FILE* out_fp, const char* unity_path )
{
    platform_mapped_file_t mf;
    if ( !platform_map_file( unity_path, &mf ) )
    {
        printf( ORB_INDENT "[orb warn] prelude: could not open %s\n", unity_path );
        return;
    }

    char        line[ 1024 ];
    bool        past_header = false;
    int         if_depth    = 0;
    const char* p           = mf.data;
    const char* end         = mf.data ? mf.data + mf.size : NULL;

    while ( p && p < end )
    {
        const char* nl  = (const char*)memchr( p, '\n', (size_t)( end - p ) );
        size_t      len = nl ? (size_t)( nl - p ) : (size_t)( end - p );
        if ( len > 0 && p[ len - 1 ] == '\r' ) len--;
        if ( len >= sizeof( line ) ) len = sizeof( line ) - 1;
        memcpy( line, p, len );
        line[ len ] = '\0';
        p = nl ? nl + 1 : end;

        if ( !past_header )
        {
            const char* q = line;
            while ( *q == ' ' || *q == '\t' ) q++;
            if ( *q != '#' ) continue;
            past_header = true;
        }

        if ( prelude_is_c_include( line ) ) break;
        if ( prelude_is_pragma_comment( line ) ) continue;

        if      ( prelude_opens_if( line )  ) if_depth++;
        else if ( prelude_closes_if( line ) ) if_depth--;

        fprintf( out_fp, "%s\n", line );
    }

    for ( int d = 0; d < if_depth; d++ )
        fprintf( out_fp, "#endif\n" );

    platform_unmap_file( &mf );
}

/*==============================================================================================
    build_gen_preludes()

    For every registered target with at least one unity unit, writes
    build/generated/<name>.prelude.h.

    The prelude is delivered to constituent .c files via -include flags injected
    into compile_commands.json entries by build_tool_11_gen_json.c -- not through
    .clangd or any other UI-layer mechanism.
==============================================================================================*/

void
build_gen_preludes( void )
{
    if ( !s_gen_preludes )
    {
        printf( "Prelude generation disabled (s_gen_preludes = false)\n" );
        return;
    }

    char gen_dir[ PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s/%s", g_build_dir, g_gen_dir );
    ensure_dir( g_build_dir );
    ensure_dir( gen_dir );

    int generated = 0;

    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];
        if ( !t->units[ 0 ] || !t->root_dir ) continue;

        char unity_path[ PATH_MAX ];
        snprintf( unity_path, sizeof( unity_path ), "%s/%s", t->root_dir, t->units[ 0 ] );

        char prelude_path[ PATH_MAX ];
        snprintf( prelude_path, sizeof( prelude_path ), "%s/%s.prelude.h", gen_dir, t->name );

        FILE* fp = fopen( prelude_path, "w" );
        if ( !fp )
        {
            printf( ORB_INDENT "[orb error] prelude: could not write %s\n", prelude_path );
            continue;
        }

        fprintf( fp, "/* %s.prelude.h  AUTO-GENERATED by build_tool -gen -- do not edit */\n",
                 t->name );
        fprintf( fp, "/* force-included via compile_commands.json /FI flag */\n" );
        fprintf( fp, "#pragma once\n\n" );
        prelude_write_preamble( fp, unity_path );
        prelude_fwd_decl_constituents( fp, t->root_dir, t->units[ 0 ] );

        fclose( fp );
        generated++;
        printf( ORB_INDENT "prelude: %s\n", prelude_path );
    }

    printf( "Generated %d unity preludes -> %s/\n", generated, gen_dir );
}

/*============================================================================================*/
