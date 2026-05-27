/*==============================================================================================

    build_tool_11_gen_prelude.c -- Unity-prelude header generator.

    For each target that has unity compilation units, reads the unity entry .c
    file and copies all preprocessor setup lines (everything before the first
    constituent .c #include) into build/prelude/<target>.prelude.h.

    Delivery is exclusively through compile_commands.json: each entry in that
    database has /FI <name>.prelude.h injected by build_tool_11_gen_json.c.
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
    The boundary of this feature is this file plus the /FI injection in
    build_tool_11_gen_json.c.  Nothing else participates.
==============================================================================================*/

static bool s_gen_preludes = true;

/*==============================================================================================
    --- Line Cursor ---

    Iterates over a memory-mapped file line by line.  Strips CRLF; null-terminates
    into the caller-supplied buf.  One definition replaces six copies of the same
    memchr/memcpy/advance pattern that previously appeared in every function.
==============================================================================================*/

typedef struct
{
    const char* p;
    const char* end;
} prelude_cursor_t;

static bool
prelude_advance( prelude_cursor_t* cur, char* buf, int cap, int* out_len )
{
    if ( cur->p >= cur->end ) return false;
    const char* nl  = (const char*)memchr( cur->p, '\n', (size_t)( cur->end - cur->p ) );
    int         len = (int)( nl ? nl - cur->p : cur->end - cur->p );
    if ( len > 0 && cur->p[ len - 1 ] == '\r' ) len--;
    if ( len >= cap ) len = cap - 1;
    memcpy( buf, cur->p, (size_t)len );
    buf[ len ] = '\0';
    cur->p     = nl ? nl + 1 : cur->end;
    if ( out_len ) *out_len = len;
    return true;
}

/*==============================================================================================
    --- Line Classifier ---

    Classifies a line into one of the directive kinds that matter for prelude
    generation.  A single call replaces the four sequential predicate calls that
    previously re-scanned from the start of each line.
==============================================================================================*/

typedef enum
{
    PLINE_OTHER,           /* blank line, code, or // comment */
    PLINE_C_INCLUDE,       /* #include "...c"  -- unity constituent */
    PLINE_H_INCLUDE,       /* #include "...h" or #include <...> */
    PLINE_PRAGMA_COMMENT,  /* #pragma comment(lib, ...) -- linker directive */
    PLINE_OPEN_IF,         /* #if / #ifdef / #ifndef */
    PLINE_CLOSE_IF,        /* #endif */
    PLINE_DIRECTIVE,       /* any other # directive (#define, #undef, etc.) */
} pline_t;

static pline_t
prelude_classify( const char* line )
{
    const char* p = line;
    while ( *p == ' ' || *p == '\t' ) p++;
    if ( *p != '#' ) return PLINE_OTHER;
    p++;
    while ( *p == ' ' || *p == '\t' ) p++;

    if ( strncmp( p, "include", 7 ) == 0 )
    {
        const char* q = p + 7;
        while ( *q == ' ' || *q == '\t' ) q++;
        if ( *q != '"' ) return PLINE_H_INCLUDE;     /* angle-bracket include */
        const char* s = q + 1;
        while ( *s && *s != '"' ) s++;
        if ( *s == '"' && s >= q + 3 && s[ -1 ] == 'c' && s[ -2 ] == '.' )
            return PLINE_C_INCLUDE;
        return PLINE_H_INCLUDE;
    }
    if ( strncmp( p, "pragma", 6 ) == 0 && strstr( p + 6, "comment" ) )
        return PLINE_PRAGMA_COMMENT;
    if ( strncmp( p, "ifdef",  5 ) == 0 ) return PLINE_OPEN_IF;
    if ( strncmp( p, "ifndef", 6 ) == 0 ) return PLINE_OPEN_IF;
    if ( strncmp( p, "if", 2 ) == 0 &&
         ( p[ 2 ] == ' ' || p[ 2 ] == '\t' || p[ 2 ] == '\r' || p[ 2 ] == '\n' ) )
        return PLINE_OPEN_IF;
    if ( strncmp( p, "endif", 5 ) == 0 ) return PLINE_CLOSE_IF;
    return PLINE_DIRECTIVE;
}

/*==============================================================================================
    --- Path Helpers ---
==============================================================================================*/

/* Extract the bare path from a #include "path.c" line into out[out_size].
   Caller must verify PLINE_C_INCLUDE first.  Returns true on success. */
static bool
prelude_extract_c_include_path( const char* line, char* out, size_t out_size )
{
    const char* q = line;
    while ( *q == ' ' || *q == '\t' ) q++;
    q++;                                    /* skip # */
    while ( *q == ' ' || *q == '\t' ) q++;
    q += 7;                                 /* skip "include" */
    while ( *q == ' ' || *q == '\t' ) q++;
    q++;                                    /* skip opening " */
    const char* r = q;
    while ( *r && *r != '"' ) r++;
    if ( *r != '"' ) return false;

    size_t path_len = (size_t)( r - q );
    if ( path_len == 0 || path_len >= out_size ) return false;
    memcpy( out, q, path_len );
    out[ path_len ] = '\0';
    return true;
}

/* Resolve inc_path: try root_dir-relative first, then source/-relative.
   Returns true and fills resolved[PATH_MAX] on success. */
static bool
prelude_resolve_include( const char* root_dir, const char* inc_path,
                         char* resolved, size_t resolved_size )
{
    snprintf( resolved, resolved_size, "%s/%s", root_dir, inc_path );
    if ( build_get_mtime( resolved ) != 0 ) return true;
    snprintf( resolved, resolved_size, "source/%s", inc_path );
    return build_get_mtime( resolved ) != 0;
}

/*==============================================================================================
    --- Unity Entry Scan ---

    Single pass over the unity entry: writes preamble lines to fp AND collects
    the resolved paths of all constituent .c includes into the caller's array.
    Previously three separate functions each opened the same unity file:
    prelude_write_preamble, prelude_write_constituent_headers, and
    prelude_fwd_decl_constituents.  This replaces all three with one map.

    Returns the number of constituent paths written into constituents[].
==============================================================================================*/

#define PRELUDE_MAX_CONSTITUENTS 64

static int
prelude_scan_unity( FILE* fp, const char* unity_path, const char* root_dir,
                    char constituents[][ PATH_MAX ], int max_c )
{
    platform_mapped_file_t mf;
    if ( !platform_map_file( unity_path, &mf ) )
    {
        printf( ORB_INDENT "[orb warn] prelude: could not open %s\n", unity_path );
        return 0;
    }

    prelude_cursor_t cur = { mf.data, mf.data ? mf.data + mf.size : NULL };

    char             line[ 1024 ];
    int              len;
    bool             past_header   = false;
    bool             preamble_done = false;
    int              if_depth      = 0;
    int              n_c           = 0;

    while ( prelude_advance( &cur, line, sizeof( line ), &len ) )
    {
        pline_t kind = prelude_classify( line );

        /* Skip everything before the first preprocessor directive (#). */
        if ( !past_header )
        {
            if ( kind == PLINE_OTHER ) continue;
            past_header = true;
        }

        if ( kind == PLINE_C_INCLUDE )
        {
            /* First .c include ends the preamble; close any still-open #if blocks. */
            if ( !preamble_done )
            {
                preamble_done = true;
                for ( int d = 0; d < if_depth; d++ )
                    fprintf( fp, "#endif\n" );
            }
            /* Collect resolved path. */
            if ( n_c < max_c )
            {
                char inc_path[ PATH_MAX ];
                if ( prelude_extract_c_include_path( line, inc_path, sizeof( inc_path ) ) )
                {
                    if ( prelude_resolve_include( root_dir, inc_path, constituents[ n_c ], PATH_MAX ) )
                        n_c++;
                }
            }
            else
            {
                printf( ORB_INDENT "[orb error] prelude: constituent limit hit in %s"
                        " (cap=%d) -- bump PRELUDE_MAX_CONSTITUENTS\n",
                        unity_path, max_c );
            }
            continue;
        }

        /* After preamble, only .c includes matter. */
        if ( preamble_done ) continue;

        /* Emit preamble lines; skip linker pragmas. */
        if ( kind == PLINE_PRAGMA_COMMENT ) continue;
        if ( kind == PLINE_OPEN_IF  ) if_depth++;
        if ( kind == PLINE_CLOSE_IF ) if_depth--;
        fwrite( line, 1, (size_t)len, fp );
        fputc( '\n', fp );
    }

    /* Handle unity entries with no constituents (if_depth may still be open). */
    if ( !preamble_done )
    {
        for ( int d = 0; d < if_depth; d++ )
            fprintf( fp, "#endif\n" );
    }

    platform_unmap_file( &mf );
    return n_c;
}

/*==============================================================================================
    --- Worklist ---

    Both walkers below (sub-header emitter, forward-declaration emitter) use the
    same iterative worklist pattern instead of recursion:

      - Static array of PRELUDE_WORKLIST_CAP slots of PATH_MAX each.
      - The worklist acts as a LIFO stack: push the seed path, then pop-process
        in a loop, pushing any .c sub-includes found while scanning each file.
      - File mappings are opened and closed within each iteration so no two
        mappings are live at the same time.

    PRELUDE_WORKLIST_CAP is sized to 32: observed high-water mark across the
    full source tree was 8, so 32 gives 4x headroom without waste.
==============================================================================================*/

#define PRELUDE_WORKLIST_CAP 32

/*==============================================================================================
    --- Sub-header Emitter ---

    Walks first_path and every .c sub-include it references, emitting every
    non-.c #include line into out_fp.  Duplicate include lines are suppressed
    via a linear-scan seen-set; the set is reset each call so deduplication is
    per-constituent, matching the scope of a single prelude pass.
==============================================================================================*/

#define PRELUDE_SEEN_CAP 256

static void
prelude_write_sub_headers( FILE* out_fp, const char* root_dir,
                           char constituents[][ PATH_MAX ], int n )
{
    static char wl  [ PRELUDE_WORKLIST_CAP ][ PATH_MAX ];
    static char seen[ PRELUDE_SEEN_CAP     ][ 1024     ];
    int top    = 0;
    int n_seen = 0;

    /* Seed all constituents so the seen-set spans the entire target. */
    for ( int j = 0; j < n && top < PRELUDE_WORKLIST_CAP; j++ )
        memcpy( wl[ top++ ], constituents[ j ], PATH_MAX );

    while ( top > 0 )
    {
        char cur_path[ PATH_MAX ];
        memcpy( cur_path, wl[ --top ], PATH_MAX );

        platform_mapped_file_t mf;
        if ( !platform_map_file( cur_path, &mf ) ) continue;

        prelude_cursor_t cur = { mf.data, mf.data ? mf.data + mf.size : NULL };
        char             line[ 1024 ];
        int              len;

        while ( prelude_advance( &cur, line, sizeof( line ), &len ) )
        {
            pline_t kind = prelude_classify( line );
            if ( kind == PLINE_C_INCLUDE )
            {
                /* Push sub-includes onto the worklist for later processing. */
                if ( top < PRELUDE_WORKLIST_CAP )
                {
                    char inc_path[ PATH_MAX ];
                    if ( prelude_extract_c_include_path( line, inc_path, sizeof( inc_path ) ) )
                    {
                        if ( prelude_resolve_include( root_dir, inc_path, wl[ top ], PATH_MAX ) )
                            top++;
                    }
                }
                else
                {
                    printf( ORB_INDENT "[orb error] prelude: sub-header worklist overflow in %s"
                            " (cap=%d) -- bump PRELUDE_WORKLIST_CAP\n",
                            cur_path, PRELUDE_WORKLIST_CAP );
                }
            }
            else if ( kind == PLINE_H_INCLUDE )
            {
                /* Suppress duplicate includes via linear-scan seen-set. */
                bool already_seen = false;
                for ( int s = 0; s < n_seen; s++ )
                {
                    if ( strcmp( seen[ s ], line ) == 0 ) { already_seen = true; break; }
                }
                if ( already_seen ) continue;
                if ( n_seen < PRELUDE_SEEN_CAP )
                    memcpy( seen[ n_seen++ ], line, (size_t)( len + 1 ) );

                fwrite( line, 1, (size_t)len, out_fp );
                fputc( '\n', out_fp );
            }
            /* All other kinds (defines, pragmas, code) are local to the sub-unity -- skip. */
        }

        platform_unmap_file( &mf );
    }
}

/*==============================================================================================
    --- Forward Declaration Emitter ---

    Walks first_path and every .c sub-include it references.  For each file,
    runs the static-function heuristic and emits a forward declaration for every
    static function found.

    Heuristic: relies on BreakBeforeBraces so lone { on its own line marks the
    opening of a function body.  Block comments and line comments are skipped so
    "static" in comment text cannot trigger collection.

    The state machine (sig, collecting, etc.) is reset at the top of each file
    so partial matches from one file cannot bleed into the next.

    Returns the worklist high-water mark.
==============================================================================================*/

static void
prelude_write_fwd_decls( FILE* out_fp, const char* root_dir,
                         const char* first_path )
{
    static char wl[ PRELUDE_WORKLIST_CAP ][ PATH_MAX ];

    int top = 0;

    /* Seed: the constituent .c file passed by the caller. */
    memcpy( wl[ top++ ], first_path, PATH_MAX );

    /* Forward-declaration state machine -- declared here, reset per file below. */
    char sig[ 4096 ];
    int  sig_len;
    bool collecting;
    bool seen_paren;
    int  paren_depth;
    bool in_block_comment;

    while ( top > 0 )
    {
        char cur_path[ PATH_MAX ];
        memcpy( cur_path, wl[ --top ], PATH_MAX );

        platform_mapped_file_t mf;
        if ( !platform_map_file( cur_path, &mf ) ) continue;

        prelude_cursor_t cur = { mf.data, mf.data ? mf.data + mf.size : NULL };
        char             line[ 1024 ];
        int              len;

        /* Reset state machine for this file. */
        sig_len          = 0;
        sig[ 0 ]         = '\0';
        collecting       = false;
        seen_paren       = false;
        paren_depth      = 0;
        in_block_comment = false;

        while ( prelude_advance( &cur, line, sizeof( line ), &len ) )
        {
            const char* q = line;
            while ( *q == ' ' || *q == '\t' ) q++;

            /* Block comment tracking: skip lines inside block comments entirely.
               "static" in a comment must not trigger collection or corrupt a live sig. */
            if ( in_block_comment )
            {
                if ( strstr( line, "*/" ) ) in_block_comment = false;
                collecting = false;
                continue;
            }
            {
                const char* open = strstr( q, "/*" );
                if ( open && !strstr( open + 2, "*/" ) )
                {
                    in_block_comment = true;
                    collecting       = false;
                    continue;
                }
            }

            /* Skip line comments. */
            if ( q[ 0 ] == '/' && q[ 1 ] == '/' ) { collecting = false; continue; }

            /* .c sub-includes feed the worklist; reset fwd-decl state. */
            if ( prelude_classify( line ) == PLINE_C_INCLUDE )
            {
                collecting = false;
                if ( top < PRELUDE_WORKLIST_CAP )
                {
                    char inc_path[ PATH_MAX ];
                    if ( prelude_extract_c_include_path( line, inc_path, sizeof( inc_path ) ) )
                    {
                        if ( prelude_resolve_include( root_dir, inc_path, wl[ top ], PATH_MAX ) )
                            top++;
                    }
                }
                else
                {
                    printf( ORB_INDENT "[orb error] prelude: fwd-decl worklist overflow in %s"
                            " (cap=%d) -- bump PRELUDE_WORKLIST_CAP\n",
                            cur_path, PRELUDE_WORKLIST_CAP );
                }
                continue;
            }

            /* --- Forward-declaration state machine --- */

            if ( !collecting )
            {
                /* Accept "static " at the start, or after leading qualifiers like
                   ORB_NOINLINE / __forceinline / __declspec(...) that precede it. */
                const char* s = strstr( q, "static " );
                if ( !s ) continue;
                if ( s != q && s[ -1 ] != ' ' && s[ -1 ] != '\t' ) continue;
                collecting  = true;
                seen_paren  = false;
                paren_depth = 0;
                sig_len     = 0;
                sig[ 0 ]    = '\0';
            }

            /* = before first ( means variable initializer -- abandon. */
            if ( !seen_paren )
            {
                const char* eq = strchr( q, '=' );
                const char* lp = strchr( q, '(' );
                if ( eq && ( !lp || eq < lp ) ) { collecting = false; continue; }
            }

            /* ; at paren_depth 0 before any lone { means variable declaration -- abandon. */
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

            /* Lone { confirms function definition -- emit forward declaration. */
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

            /* Accumulate line into sig (newline-separated for readable multi-line sigs). */
            {
                int n    = len + 1;
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

            /* Track paren depth for multi-line parameter lists. */
            for ( const char* r = line; *r; r++ )
            {
                if      ( *r == '(' ) { paren_depth++; seen_paren = true; }
                else if ( *r == ')' )   paren_depth--;
            }
        }

        platform_unmap_file( &mf );
    }
}

/*==============================================================================================
    build_gen_preludes()

    For every registered target with at least one unity unit, writes
    build/prelude/<name>.prelude.h.

    Per-target work: one unity scan (preamble + constituent paths), then one
    sub-header pass and one fwd-decl pass per constituent.  Each constituent file
    is mapped once per pass; sub-paths are deferred so no nested mappings occur.

    The constituent path array and both walker worklists are static, sized from
    observed high-water marks.  No heap allocation takes place during generation.
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
    snprintf( gen_dir, sizeof( gen_dir ), "%s/%s", g_build_dir, g_prelude_dir );
    ensure_dir( g_build_dir );
    ensure_dir( gen_dir );

    /* Constituent path array: reused across all targets. */
    static char constituents[ PRELUDE_MAX_CONSTITUENTS ][ PATH_MAX ];

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

        fwrite( "/* ", 1, 3, fp );
        fputs( t->name, fp );
        fputs( ".prelude.h  AUTO-GENERATED by build_tool -gen -- do not edit */\n", fp );
        fputs( "/* force-included via compile_commands.json /FI flag */\n", fp );
        fputs( "#pragma once\n\n", fp );

        /* One unity scan: write preamble + collect constituent resolved paths. */
        int n = prelude_scan_unity( fp, unity_path, t->root_dir,
                                    constituents, PRELUDE_MAX_CONSTITUENTS );

        /* Sub-unity headers: one pass across all constituents for deduplication. */
        prelude_write_sub_headers( fp, t->root_dir, constituents, n );

        /* Forward declarations: one pass per constituent. */
        if ( n > 0 )
        {
            fputs( "\n/* forward declarations of static functions in constituent files */\n",
                   fp );
            for ( int j = 0; j < n; j++ )
                prelude_write_fwd_decls( fp, t->root_dir, constituents[ j ] );
        }

        fclose( fp );
        if ( g_out_flags & ORB_OUT_GEN_PRELUDES )
            printf( ORB_INDENT "prelude: %s\n", prelude_path );
        generated++;
    }

    printf( "Generated preludes: %d written -> %s/\n", generated, gen_dir );
}

/*============================================================================================*/
