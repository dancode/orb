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

    Future: forward-declare static helper functions found in constituent .c
    files so cross-file static calls inside the same unity TU also resolve
    in the IDE.  (Not yet implemented.)

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
    FILE* in = fopen( unity_path, "r" );
    if ( !in )
    {
        printf( ORB_INDENT "[orb warn] prelude: could not open %s\n", unity_path );
        return;
    }

    char line[ 1024 ];
    bool past_header = false;
    int  if_depth    = 0;

    while ( fgets( line, sizeof( line ), in ) )
    {
        if ( !past_header )
        {
            const char* p = line;
            while ( *p == ' ' || *p == '\t' ) p++;
            if ( *p != '#' ) continue;
            past_header = true;
        }

        if ( prelude_is_c_include( line ) ) break;
        if ( prelude_is_pragma_comment( line ) ) continue;

        if      ( prelude_opens_if( line )  ) if_depth++;
        else if ( prelude_closes_if( line ) ) if_depth--;

        fputs( line, out_fp );
    }

    for ( int d = 0; d < if_depth; d++ )
        fprintf( out_fp, "#endif\n" );

    fclose( in );
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
        fprintf( fp, "/* force-included via compile_commands.json -include flag */\n" );
        fprintf( fp, "#pragma once\n\n" );
        prelude_write_preamble( fp, unity_path );

        fclose( fp );
        generated++;
        printf( ORB_INDENT "prelude: %s\n", prelude_path );
    }

    printf( "Generated %d unity preludes -> %s/\n", generated, gen_dir );
}

/*============================================================================================*/
