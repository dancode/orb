/*==============================================================================================

    build_tool_05_log.c -- Output formatting and log routing.

    Stateless print helpers: everything in this file produces human-readable
    output without touching any build state. Separating these from the command-
    building logic in 06_compile and 07_link keeps those files focused on
    assembling correct commands; this file focuses on making them readable.

    Contents:
      - Log routing   : log_open / log_close -- route output to the
                        per-worker log during parallel builds, stdout otherwise.
      - MSVC output   : is_msvc_source_echo -- filter cl.exe TU banner lines.
      - Token printers: print_tokens, print_section, print_compile_output.
      - Command echo  : print_raw_cmd -- wrapped display of assembled command lines.

    compile_cmd_t sections are printed by cc_print in 06_compile.c.
    link_cmd_t sections are printed by lk_print in 07_link.c.
    Both live with their respective struct types to avoid a forward-declaration
    dependency here.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Log Routing ---

    During a parallel build each worker redirects its child-process output and its
    own section prints to a private per-target log file. The scheduler dumps that
    log atomically to stdout when the target finishes, preventing interleaving.

    log_open / log_close encapsulate that routing: callers get the right sink
    (worker log or stdout) without caring about the scheduler's internal state.
==============================================================================================*/

static FILE*
log_open( void )
{
    const char* log = sched_log_path();
    if ( log )
    {
        FILE* f = fopen( log, "a" );
        if ( f ) return f;
    }
    return stdout;
}

static void
log_close( FILE* f )
{
    if ( f != stdout ) fclose( f );
}

/*==============================================================================================
    --- MSVC Source Echo Filter ---

    Returns true for the bare TU filename cl.exe prints before any diagnostics
    (e.g. it prints "reflect_tool.c" on its own line when compiling that file).
    We filter these out of log dumps because the orb build log already shows
    the source list in the [orb compiling] section -- cl's echo is duplicate noise.

    A line qualifies as a source echo iff (after trimming whitespace) it:
      - is non-empty
      - contains no internal spaces or path separators (so "src/foo.c" -- a
        diagnostic path -- does not match)
      - ends with extension "c" or "cpp" (case-insensitive)
==============================================================================================*/

static bool
is_msvc_source_echo( const char* line )
{
    // 1. Skip leading whitespace.
    while ( *line == ' ' || *line == '\t' ) ++line;

    // 2. Compute effective length, trimming trailing CR/LF/space.
    int len = ( int )strlen( line );
    while ( len > 0 && ( line[ len - 1 ] == '\n'
                      || line[ len - 1 ] == '\r'
                      || line[ len - 1 ] == ' ' ) ) --len;
    if ( len == 0 )
        return false;

    // 3. Reject anything with an interior space or path separator -- cl's TU
    //    banner is a bare filename, not a path or diagnostic message.
    for ( int i = 0; i < len; ++i )
        if ( line[ i ] == ' ' || line[ i ] == '/' || line[ i ] == '\\' )
            return false;

    // 4. Find the last '.' for extension extraction. No dot = not a source file.
    int dot = -1;
    for ( int i = len - 1; i >= 0; --i ) { if ( line[ i ] == '.' ) { dot = i; break; } }
    if ( dot < 0 )
        return false;

    // 5. Compare the extension (case-insensitive, length-bounded to avoid
    //    matching trailing CR/LF bytes that weren't trimmed by a prior pass).
    const char* ext     = line + dot + 1;
    int         ext_len = len - ( dot + 1 );
    if ( ext_len == 1 && str_nicmp( ext, "c",   1 ) == 0 ) return true;
    if ( ext_len == 3 && str_nicmp( ext, "cpp", 3 ) == 0 ) return true;

    return false;
}

/*==============================================================================================
    --- Token Printers ---

    Print space-separated tokens from `raw`, stripping the leading `prefix`
    from each token when non-NULL (e.g. "/D" strips preprocessor flag chars).

    Example: raw = "/DOS_WINDOWS /DARCH_X64", prefix = "/D"
             prints: "OS_WINDOWS ARCH_X64\n"

    Returns true if at least one token was written (content was non-empty).
==============================================================================================*/

#define SECTION_FMT  "            %-10s"

static bool
print_tokens( FILE* out, const char* raw, const char* prefix )
{
    size_t      plen  = prefix ? strlen( prefix ) : 0;
    const char* p     = raw;
    bool        first = true;

    while ( *p )
    {
        while ( *p == ' ' ) ++p;
        if ( !*p ) break;

        const char* s   = p;
        while ( *p && *p != ' ' ) ++p;
        int         len = (int)( p - s );

        // Strip the prefix from this token if it matches and something remains.
        if ( plen && (size_t)len > plen && strncmp( s, prefix, plen ) == 0 )
        {
            s   += plen;
            len -= (int)plen;
        }

        if ( !first ) fputc( ' ', out );
        fwrite( s, 1, len, out );
        first = false;
    }
    fputc( '\n', out );
    return !first;
}

/* Print a labeled section line: "            <label>  <content>". */

static bool
print_section( FILE* out, const char* label, const char* content, const char* strip )
{
    fprintf( out, SECTION_FMT, label );
    return print_tokens( out, content, strip );
}

/*==============================================================================================
    Print the compile output section: /FoobjDir/ /FdobjDir/ -> "obj=... pdb=..."

    Specialised variant of print_tokens(): instead of stripping one fixed prefix,
    it inspects each token's prefix character to choose a human label ("obj=" or
    "pdb=") for each /Fo and /Fd flag respectively.
==============================================================================================*/

static bool
print_compile_output( FILE* out, const char* raw )
{
    fprintf( out, SECTION_FMT, "output:" );
    const char* p     = raw;
    bool        first = true;

    while ( *p )
    {
        while ( *p == ' ' ) ++p;
        if ( !*p ) break;
        const char* s   = p;
        while ( *p && *p != ' ' ) ++p;
        int         len = (int)( p - s );

        const char* label = NULL;
        if ( len > 3 && s[ 0 ] == '/' && s[ 1 ] == 'F' )
        {
            if ( s[ 2 ] == 'o' ) { label = "obj=";   s += 3; len -= 3; }
            if ( s[ 2 ] == 'd' ) { label = "  pdb="; s += 3; len -= 3; }
        }

        if ( !first && !label ) fputc( ' ', out );
        if ( label ) fputs( label, out );
        fwrite( s, 1, len, out );
        first = false;
    }
    fputc( '\n', out );
    return !first;
}

/*==============================================================================================
    --- Raw Command Echo ---

    Print the assembled command to `out` when the caller's CMD flag is set.
    Wraps at 100 columns after the first token so long lines stay readable.
    Each token is kept intact (no mid-token breaks).
==============================================================================================*/

static void
print_raw_cmd( FILE* out, const char* cmd )
{
    static const int k_wrap = 100;  // soft line width target
    static const int k_cont = 22;   // continuation indent -- matches ORB_INDENT(10) + "[orb cmd]   "(12)

    fprintf( out, ORB_INDENT "[orb cmd]   " );
    int col = 0;
    const char* p = cmd;

    while ( *p )
    {
        const char* w = p;
        while ( *p && *p != ' ' ) ++p;
        int wlen = (int)( p - w );

        if ( col > 0 && col + 1 + wlen > k_wrap )
        {
            fprintf( out, "\n%*s", k_cont, "" );
            col = 0;
        }
        else if ( col > 0 ) { fputc( ' ', out ); ++col; }

        fwrite( w, 1, wlen, out );
        col += wlen;

        while ( *p == ' ' ) ++p;
    }
    fprintf( out, "\n\n" );
}

// clang-format on
/*============================================================================================*/
