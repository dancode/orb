/*==============================================================================================

    build_tool_cc.c -- Compiler and linker command generation.

    Builds the cl.exe / link.exe / lib.exe command lines for a single target.
    Commands are assembled section-by-section into compile_cmd_t / link_cmd_t
    structs so each logical group (flags, defines, includes, sources, output)
    can be printed independently under g_out_flags control, then joined into
    one string for actual execution.

    Two public entry points, both called from build_target():
      build_target_compile()        -- full unity compile; include tracking via /showIncludes + _includes.txt.
      build_target_compile_single() -- single-file compile for VS Ctrl+F7; no include tracking.
      build_target_link()           -- lib.exe for static libs, link.exe otherwise.

    Define source of truth:
      Preprocessor defines are driven from the shared tables in build_tool_targets.c
      (g_defines_always, g_defines_debug, g_defines_release). build_tool_gen.c
      reads the same tables for IntelliSense vcxproj emission -- no manual lockstep.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Structured command types --- 

    Holds the raw fragment that goes into the final cl/link/lib command. 
    Assembled in order via cc_assemble() / lk_assemble(); 
    
    Printing of these values is filetered selectively via cc_print() / lk_print() 
    based on g_out_flags.
==============================================================================================*/

typedef struct
{
    char exe      [ 64          ];  // cl.exe or clang-cl.exe
    char flags    [ 512         ];  // /c /nologo /W4 /WX /Zi /Od /MDd ...
    char includes [ 512         ];  // /I source /I gen_dir
    char defines  [ 1024        ];  // /DOS_WINDOWS /DCOMPILE_MSVC /D_DEBUG ...
    char output   [ 512         ];  // /FoobjDir/ /FdobjDir/
    char sources  [ CMD_BUF_MAX ];  // absolute .c paths
                                
} compile_cmd_t;

typedef struct
{
    char exe      [ 32          ];  // lib.exe or link.exe
    char artifact [ BT_PATH_MAX ];  // final output path (summary display only)
    char flags    [ 256         ];  // /nologo /DLL ...
    char output   [ 512         ];  // /OUT:... /IMPLIB:...
    char pdb      [ 256         ];  // /DEBUG /PDB:... (empty for lib.exe)
    char inputs   [ 512         ];  // objDir/*.obj
    char libs     [ 1024        ];  // dep.lib ... user32.lib ...

} link_cmd_t;

/*==============================================================================================
    --- Overflow Sentinel checks -- 

    Verify the last byte of every field is still '\0' before the structs are
    assembled into a command string. cc_field() aborts on detected overflow, but
    these catch any silent corruption that bypassed it.
==============================================================================================*/

static void
cc_check_overflow( const compile_cmd_t* cc )
{
    if ( cc->exe     [ 64          - 1 ] || cc->flags   [ 512         - 1 ] ||
         cc->includes[ 512         - 1 ] || cc->defines [ 1024        - 1 ] ||
         cc->output  [ 512         - 1 ] || cc->sources [ CMD_BUF_MAX - 1 ] )
    {
        printf( ORB_INDENT "[orb error] compile_cmd_t sentinel overwritten -- field overflow\n" );
        exit( 1 );
    }
}

static void
lk_check_overflow( const link_cmd_t* lk )
{
    if ( lk->exe     [ 32          - 1 ] || lk->artifact[ BT_PATH_MAX - 1 ] ||
         lk->flags   [ 256         - 1 ] || lk->output  [ 512         - 1 ] ||
         lk->pdb     [ 256         - 1 ] || lk->inputs  [ 512         - 1 ] ||
         lk->libs    [ 1024        - 1 ] )
    {
        printf( ORB_INDENT "[orb error] link_cmd_t sentinel overwritten -- field overflow\n" );
        exit( 1 );
    }
}

/*==============================================================================================
    --- Internal helpers ---

    Append a formatted string to a fixed-size field. Halts the process on overflow.
==============================================================================================*/

static void
cc_field( char* dst, size_t dst_size, const char* fmt, ... )
{
    size_t used = strlen( dst );
    if ( used >= dst_size - 1 )
    {
        printf( ORB_INDENT "[orb error] cc_field overflow (capacity %zu)"
               "-- raise field size in compile_cmd_t\n", dst_size );
        exit( 1 );
    }
    size_t  remaining = dst_size - used;

    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( dst + used, remaining, fmt, args );
    va_end( args );

    if ( written < 0 || ( size_t )written >= remaining )
    {
        printf( ORB_INDENT "[orb error] cc_field truncated (needed %d, had %zu)"
               "-- raise field size in compile_cmd_t\n", written, remaining );
        exit( 1 );
    }
}

/*==============================================================================================
    Convenience wrapper around cc_field(): infers the destination capacity from
    the field's declared size, removing the need for callers to pass sizeof().
==============================================================================================*/

#define CC_APPEND( field, ... ) cc_field( ( field ), sizeof( field ), __VA_ARGS__ )

/*==============================================================================================
    get_target_upper() -- derive <TARGET>_STATIC from a target name.
    Must match the IntelliSense defines emitted by build_tool_gen.c (unity build).
==============================================================================================*/

static void
get_target_upper( const char* name, char* out, size_t out_size )
{
    strncpy( out, name, out_size - 1 );
    out[ out_size - 1 ] = '\0';
    for ( char* p = out; *p; ++p ) *p = ( char )toupper( *p );
}

/*==============================================================================================
    cleanup_stale_pdbs() -- remove PDB files from previous links.

    Each link emits a uniquely-named bin/<target>_<timestamp>.pdb so the linker
    never has to overwrite a PDB that a debugger may still hold open. Across
    many builds those uniquely-named files accumulate; this routine sweeps the
    unlocked ones away at the start of each link.
    
    remove() silently fails for any PDB still locked by an attached debugger,
    which is exactly what we want -- we keep going, the held file survives, and
    the new link goes to a fresh name with a newer timestamp.
==============================================================================================*/

static void
cleanup_stale_pdbs( const char* target_name )
{
    // Build a wildcard pattern like "bin/foo_*.pdb" that _findfirst will
    // expand against the filesystem. Note: backslashes also work, but we use
    // forward slashes for consistency with the rest of the codebase.
    char pattern[ BT_PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "bin\\%s_*.pdb", target_name );

    struct _finddata_t fd;
    intptr_t h = _findfirst( pattern, &fd );
    if ( h == -1 ) return;          // No matches -> nothing to do.

    // Walk every match. _findfirst returns the FIRST entry implicitly; we
    // do-while so we process that first hit before calling _findnext.
    // fd.name is just the basename ("foo_12345.pdb"), so we rebuild the
    // full path with the "bin/" prefix before calling remove().
    do
    {
        char path[ BT_PATH_MAX ];
        snprintf( path, sizeof( path ), "bin\\%s", fd.name );
        remove( path );
    }
    while ( _findnext( h, &fd ) == 0 );
    _findclose( h );
}

/*==============================================================================================
    Return the active output sink: per-thread log when inside a parallel worker, 
    (so section output lands with child-process output), stdout otherwise. 
    
    Caller must close with cc_close_log().
==============================================================================================*/

static FILE*
cc_open_log( void )
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
cc_close_log( FILE* f )
{
    if ( f != stdout ) fclose( f );
}

/*==============================================================================================
    Section Printers

    Print space-separated tokens from `raw`, stripping the leading `prefix`
    from each token when non-NULL (e.g. "/D" strips preprocessor flag chars).

    Example: raw = "/DOS_WINDOWS /DARCH_X64", prefix = "/D"
             prints: "OS_WINDOWS ARCH_X64\n"

    This is the workhorse that renders the human-readable section view of
    flags / defines / includes / etc. We can't just printf(raw) because the
    caller wants per-token prefix-stripping and a final newline.
    Returns true if at least one token was written (content was non-empty).
==============================================================================================*/

static bool
print_tokens( FILE* out, const char* raw, const char* prefix )
{
    size_t      plen  = prefix ? strlen( prefix ) : 0;
    const char* p     = raw;          // running cursor through `raw`
    bool        first = true;         // controls whether to emit a separating space

    while ( *p )
    {
        // Skip any run of spaces between tokens (and leading whitespace).
        while ( *p == ' ' ) ++p;
        if ( !*p ) break;             // ran off the end inside the gap -> done

        // Record token start, then advance to the next space (or end-of-string).
        // `s` ... `p` now bracket exactly one token.
        const char* s   = p;
        while ( *p && *p != ' ' ) ++p;
        int         len = (int)( p - s );

        // Strip the caller's prefix from this token IF: a prefix was provided,
        // the token is longer than the prefix (so something remains after the
        // strip), and the token's leading bytes match. The `len > plen` guard
        // is the reason "/D" alone (no symbol after it) is left untouched.
        if ( plen && (size_t)len > plen && strncmp( s, prefix, plen ) == 0 )
        {
            s   += plen;
            len -= (int)plen;
        }

        // Emit a single space between tokens, then the (possibly-stripped) token.
        // fwrite is used instead of fputs because `s` is NOT null-terminated --
        // it points into the middle of `raw`.
        if ( !first ) fputc( ' ', out );
        fwrite( s, 1, len, out );
        first = false;
    }
    fputc( '\n', out );
    return !first;  // first stayed true -> no tokens -> empty content
}

/*==============================================================================================
    Print a labeled section line: "  <label>  <content>".
    Returns true if content had tokens (non-empty). `strip` is forwarded to print_tokens.
==============================================================================================*/

#define SECTION_FMT  "            %-10s"

static bool
print_section( FILE* out, const char* label, const char* content, const char* strip )
{
    fprintf( out, SECTION_FMT, label );
    return print_tokens( out, content, strip );
}

/*==============================================================================================
    Print the compile output section: /FoobjDir/ /FdobjDir/ -> "obj=... pdb=..."
    
    Specialised version of print_tokens(): instead of just stripping one fixed
    prefix, it inspects each token's prefix character to decide which human
    label ("obj=" or "pdb=") to print in front of the stripped path.

    // Example: raw = "/Fobuild\\obj\\foo/ /Fdbuild\\obj\\foo/"
    //          prints: "obj=build\obj\foo/  pdb=build\obj\foo/\n"

==============================================================================================*/

static bool
print_compile_output( FILE* out, const char* raw )
{
    fprintf( out, SECTION_FMT, "output:" );
    const char* p     = raw;
    bool        first = true;

    while ( *p )
    {
        // Skip whitespace, find the token bounds -- same scan loop as print_tokens.
        while ( *p == ' ' ) ++p;
        if ( !*p ) break;
        const char* s   = p;
        while ( *p && *p != ' ' ) ++p;
        int         len = (int)( p - s );

        // Decide which prefix this token uses, if any. The MSVC flags are
        // "/Foxxx" (object output dir) and "/Fdxxx" (compile-PDB output dir).
        // `len > 3` ensures there is at least one character after "/Fo" or
        // "/Fd" to print -- empty flags would just produce noise.
        const char* label = NULL;
        if ( len > 3 && s[ 0 ] == '/' && s[ 1 ] == 'F' )
        {
            if ( s[ 2 ] == 'o' ) { label = "obj=";   s += 3; len -= 3; }
            if ( s[ 2 ] == 'd' ) { label = "  pdb="; s += 3; len -= 3; }
        }

        // Spacing rules:
        //   - For an UNLABELED token, emit a separating space if it's not first.
        //   - For a LABELED token, the label itself already starts with the
        //     visual separation we want ("  pdb=" has two leading spaces), so
        //     no extra space needed.
        if ( !first && !label ) fputc( ' ', out );
        if ( label ) fputs( label, out );
        fwrite( s, 1, len, out );
        first = false;
    }
    fputc( '\n', out );
    return !first;
}

/*==============================================================================================
    Print compile command sections to `out` according to g_out_flags.
==============================================================================================*/

static void
cc_print( FILE* out, const compile_cmd_t* cc, const target_info_t* target, const char* config )
{
    if ( g_out_flags & ORB_OUT_SUMMARY_COMPILE ) {
        fprintf( out, ORB_INDENT "[orb compiling] %s\n", target->name );
    }

    if ( g_out_flags & ORB_OUT_ANY_COMPILE ) {
        fprintf( out, "\n" );
    };

    bool any = false;
    if ( g_out_flags & ORB_OUT_COMPILE_SOURCES  ) any |= print_section( out, "sources:",  cc->sources,  NULL );
    if ( g_out_flags & ORB_OUT_COMPILE_FLAGS    ) any |= print_section( out, "flags:",    cc->flags,    NULL );
    if ( g_out_flags & ORB_OUT_COMPILE_DEFINES  ) any |= print_section( out, "defines:",  cc->defines,  "/D" );
    if ( g_out_flags & ORB_OUT_COMPILE_INCLUDES ) any |= print_section( out, "includes:", cc->includes, "/I" );
    if ( g_out_flags & ORB_OUT_COMPILE_OUTPUT   ) any |= print_compile_output( out, cc->output );
    if ( any ) fprintf( out, "\n" );
}

/*==============================================================================================
    Print link command sections to `out` according to g_out_flags.
==============================================================================================*/

static void
lk_print( FILE* out, const link_cmd_t* lk, const target_info_t* target )
{
    if ( g_out_flags & ORB_OUT_SUMMARY_LINK )
        fprintf( out, ORB_INDENT "[orb link] %s -> %s\n", target->name, lk->artifact );

    if ( g_out_flags & ORB_OUT_ANY_LINK ) {
        fprintf( out, "\n" );
    };

    bool any = false;
    if ( g_out_flags & ORB_OUT_LINK_INPUTS  ) any |= print_section( out, "inputs:",  lk->inputs,  NULL );
    if ( g_out_flags & ORB_OUT_LINK_LIBS    ) any |= print_section( out, "libs:",    lk->libs,    NULL );
    if ( g_out_flags & ORB_OUT_LINK_FLAGS   ) any |= print_section( out, "flags:",   lk->flags,   NULL );
    if ( g_out_flags & ORB_OUT_LINK_OUTPUT  ) any |= print_section( out, "output:",  lk->output,  "/OUT:" );
    if ( g_out_flags & ORB_OUT_LINK_PDB     ) any |= print_section( out, "pdb:",     lk->pdb,     "/PDB:" );
    if ( any ) fprintf( out, "\n" );
}

/*==============================================================================================

    -- Command Assembly -- 

    Join the struct fields into a single string for the actual cl/link/lib call.
    Response-file spill happens here when the assembled string exceeds the shell
    arg limit -- same mechanism as before, now operating on the joined result.

    order: <exe> <flags> <includes> <defines> <output> <sources>

    Spillover: rsp_file

    The sources field alone can be CMD_BUF_MAX bytes; joining everything through
    cmd_buf_t (also CMD_BUF_MAX) would silently truncate the sources before the
    spill check ever runs. 
    
    Instead, format args into a dedicated oversize buffer, measure the total, 
    then write the rsp file from that buffer while the data is still complete.

==============================================================================================*/

static bool
cc_assemble( const compile_cmd_t* cc, cmd_buf_t* cmd, const char* rsp_path )
{
    char args[ CMD_BUF_MAX ];

    int  written = snprintf( args, sizeof( args ), "%s %s %s %s %s",
                             cc->flags, cc->includes, cc->defines, cc->output, cc->sources );

    /* sanity check: ensure the fields were big enough to hold their content without
       overflow before we even get to the spill logic. If this fires, the fix is
       to increase the relevant field size in compile_cmd_t, not ARGS_BUF. */
    if ( written < 0 || ( size_t )written >= sizeof( args ) )
    {
        printf( ORB_INDENT "[orb error] cc_assemble args truncated (needed %d)\n", written );
        return false;
    }

    /* handle response file spillover if enabled, if not emit a warning but continue */
    size_t total = strlen( cc->exe ) + 1 + ( size_t )( written < 0 ? 0 : written );
    if ( total >= CMD_RSP_THRESHOLD )
    {
        if ( g_use_rsp )
        {
            // Write the full args to the response file before any truncation can occur.
            FILE* f = fopen( rsp_path, "w" );
            if ( f ) 
            { 
                fputs( args, f ); 
                fclose( f ); 
            }
            else 
            {
                printf( ORB_INDENT "[orb error] could not open response file %s\n", rsp_path );
                return false;
            }
            cmd->size      = 0;
            cmd->truncated = false;
            cmd_append( cmd, "%s @%s", cc->exe, rsp_path );
            return true;
        }
        
        // fatal error
        printf( ORB_INDENT "[orb error] command length %zu exceeds threshold;" 
                "enable -rsp to use a response file\n", total );
        return false;        
    }
    else
    {
        cmd_append( cmd, "%s %s", cc->exe, args );
    }
    return true;
}

/*==============================================================================================
    Same idea for the link/archive side. The PDB section is optional: lib.exe
    doesn't take a /DEBUG /PDB pair, so we omit that field when lk->pdb is
    empty rather than emitting a stray pair of spaces.
==============================================================================================*/

static void
lk_assemble( const link_cmd_t* lk, cmd_buf_t* cmd, const char* rsp_path )
{
    if ( lk->pdb[ 0 ] )
        cmd_append( cmd, "%s %s %s %s %s %s", lk->exe, lk->flags, lk->output, lk->pdb, lk->inputs, lk->libs );
    else
        cmd_append( cmd, "%s %s %s %s %s",    lk->exe, lk->flags, lk->output, lk->inputs, lk->libs );

    cmd_spill_to_response_file( cmd, rsp_path );
}

/*==============================================================================================

    -- Raw-command echo (ORB_OUT_*_CMD) ---

    Print the assembled command to `out` when the caller's CMD flag is set.
    Wraps at 100 columns after the first token so long lines stay readable.

    Each token is kept intact (no mid-token line breaks). If appending the next
    token would push past k_wrap, we line-break first and indent the new line
    by k_cont spaces so the continuation visually nests under the command.

==============================================================================================*/

static void
print_raw_cmd( FILE* out, const char* cmd )
{
    static const int k_wrap = 100;  // soft line width target
    static const int k_cont = 22;   // continuation indent width -- matches ORB_INDENT(10) + "[orb cmd]   "(12)

    fprintf( out, ORB_INDENT "[orb cmd]   " );
    int col = 0;                    // current column within the current line
    const char* p = cmd;

    while ( *p )
    {
        // Find the next whitespace-delimited token: w ... p brackets it.
        const char* w = p;
        while ( *p && *p != ' ' ) ++p;
        int wlen = (int)( p - w );

        // Two layout cases:
        //   (a) The token would push the line past k_wrap -> wrap to a new
        //       indented line. col == 0 after wrap, no leading space needed.
        //   (b) Token fits on the current line -> emit a single separator
        //       space (skipped on the very first token, when col == 0).
        if ( col > 0 && col + 1 + wlen > k_wrap )
        {
            fprintf( out, "\n%*s", k_cont, "" );
            col = 0;
        }
        else if ( col > 0 ) { fputc( ' ', out ); ++col; }

        fwrite( w, 1, wlen, out );
        col += wlen;

        // Eat any run of spaces after the token so the next iteration picks up
        // the start of the following token (or hits the end-of-string).
        while ( *p == ' ' ) ++p;
    }
    fprintf( out, "\n\n" );
}

/*==============================================================================================

    == MSVC output classification --

    Returns true for the bare source-file banner cl.exe prints once per compiled
    translation unit (e.g. it prints "reflect_tool.c" on its own line before emitting
    any diagnostics for that TU). We filter these out of our log dumps because
    the orb build log already shows the source list in the [orb compiling] section
    -- leaving cl's echo in just creates duplicate noise.

    A line qualifies as a source echo iff (after trimming whitespace) it is:
      - non-empty
      - contains no internal spaces or path separators (so "src/foo.c" doesn't
        match -- that's a diagnostic, not a banner)
      - has a '.' producing an extension equal to "c" or "cpp" (case-insensitive)

    Implementation walks the line in three passes for readability over speed:
    the volume here is tiny (a few hundred lines per build at worst).

==============================================================================================*/

static bool
is_msvc_source_echo( const char* line )
{
    /*  1.  Skip any leading whitespace so " foo.c" matches just like "foo.c". */

    while ( *line == ' ' || *line == '\t' ) ++line;

    /*  2.  Compute the effective length, ignoring any trailing CR / LF / space
            that survived the line-buffer split. We work with a length rather
            than mutating the buffer because the caller may want the line again. */

    int len = ( int )strlen( line );
    while ( len > 0 && ( line[ len - 1 ] == '\n'
                      || line[ len - 1 ] == '\r'
                      || line[ len - 1 ] == ' ' ) ) --len;
    if ( len == 0 ) 
        return false;

    /*  3.  Reject anything that contains an interior space or a path separator --
            cl's TU banner is a bare filename, never a path. This is what
            distinguishes "foo.c" (echo) from "F:\orb\src\foo.c(12): error C..."
            (diagnostic -- keep!). */

    for ( int i = 0; i < len; ++i )
        if ( line[ i ] == ' ' || line[ i ] == '/' || line[ i ] == '\\' ) 
            return false;

    /* 4.   Find the last '.' so we can look at the extension. No dot = no
            extension = not a source banner. */

    int dot = -1;
    for ( int i = len - 1; i >= 0; --i ) { if ( line[ i ] == '.' ) { dot = i; break; } }
    if ( dot < 0 ) 
        return false;

    /* 5.   Compare the extension (case-insensitive). We use _strnicmp + an
            explicit length check rather than _stricmp because `line` may still
            have trailing CR/LF/spaces past `len` (we trimmed by adjusting len
            only, not by null-terminating). E.g. "foo.c\n" gives ext = "c\n"
            and _stricmp("c\n", "c") would mismatch -- losing the filter. */

    const char* ext     = line + dot + 1;
    int         ext_len = len - ( dot + 1 );
    if ( ext_len == 1 && _strnicmp( ext, "c",   1 ) == 0 ) return true;
    if ( ext_len == 3 && _strnicmp( ext, "cpp", 3 ) == 0 ) return true;

    return false;
}

/*==============================================================================================

    -- Fill Compile Command --

    Shared setup for both compile entry points. Fills exe, flags, includes, defines,
    and output. Sources are left empty -- the caller appends them. /showIncludes is
    also left to the caller so each entry point can opt in independently.

==============================================================================================*/

static void
cc_fill_compile_cmd( build_context_t* ctx, target_info_t* target,
                     const char* obj_dir, const char* gen_dir, compile_cmd_t* cc )
{
    /* exe: which compiler is invoked */

    snprintf( cc->exe, sizeof( cc->exe ), "%s", ctx->compiler == COMPILE_CLANG ? "clang-cl.exe" : "cl.exe" );

    /* flags: standard + config-specific + warning suppressions.
       /showIncludes is added by the caller when include tracking is active. */

    CC_APPEND( cc->flags, "/c /nologo /W4 /WX /Zc:preprocessor /std:c11" );
    if ( ctx->config == CONFIG_DEBUG ) CC_APPEND( cc->flags, " /Zi /Od /MDd" );
    else                               CC_APPEND( cc->flags, " /O2 /MD" );

    for ( int i = 0; i < g_warn_suppression_count; ++i )
    {
        warn_suppress_t* s = &g_warn_suppressions[ i ];
        if ( ( s->config == ctx->config || s->config == CONFIG_COUNT ) &&
             ( s->compiler_mask & (unsigned int)ctx->compiler ) )
            CC_APPEND( cc->flags, " %s", s->flag );
    }

    /* includes: ensure generated headers are in include path */

    CC_APPEND( cc->includes, "/I source /I %s", gen_dir );

    /* defines: consumed from shared tables in build_tool_targets.c so gen.c
       IntelliSense output is guaranteed to match. Dynamic per-target defines
       (the _STATIC chain) stay procedural -- they depend on runtime target state. */

    for ( int i = 0; g_defines_always[ i ]; ++i )
        CC_APPEND( cc->defines, "%s/D%s", cc->defines[ 0 ] ? " " : "", g_defines_always[ i ] );

    {
        char upper[ 128 ];
        get_target_upper( target->name, upper, sizeof( upper ) );
        CC_APPEND( cc->defines, " /D%s_STATIC", upper );
    }
    for ( int i = 0; target->deps[ i ]; ++i )
    {
        target_info_t* dep = find_target( target->deps[ i ] );
        if ( !dep ) continue;
        bool dep_is_static = ( dep->type == TARGET_STATIC_LIB ) ||
                             ( dep->type == TARGET_DYNAMIC_LIB && ctx->is_monolithic );
        if ( dep_is_static )
        {
            char dep_upper[ 128 ];
            get_target_upper( dep->name, dep_upper, sizeof( dep_upper ) );
            CC_APPEND( cc->defines, " /D%s_STATIC", dep_upper );
        }
    }
    if ( ctx->is_monolithic ) CC_APPEND( cc->defines, " /DBUILD_STATIC" );
    {
        const char** cfg_defines = ( ctx->config == CONFIG_DEBUG ) ? g_defines_debug : g_defines_release;
        for ( int i = 0; cfg_defines[ i ]; ++i )
            CC_APPEND( cc->defines, " /D%s", cfg_defines[ i ] );
    }

    /* output dirs: /Fo = .obj destination, /Fd = compile-PDB destination.
       Trailing slash required -- without it cl treats the path as a filename prefix. */

    CC_APPEND( cc->output, "/Fo%s/ /Fd%s/", obj_dir, obj_dir );
}

/*==============================================================================================
    
    -- Run Compile Command -- 

    : Shared tail for both compile entry points: print sections, assemble, echo raw command
      if requested, then run. (writing an rsp file under obj_dir/<rsp_name> when needed), 
    
    : includes_path is forwarded to build_run_cmd_capture_includes; NULL means no includes file written.

==============================================================================================*/

static bool
cc_run_compile_cmd( compile_cmd_t* cc, target_info_t* target, const char* config,
                    const char* obj_dir, const char* rsp_name, const char* includes_path )
{
    cmd_buf_t cmd = { 0 };
    {
        /* -- Assemble the command -- */

        FILE* log_out = cc_open_log();
        cc_print( log_out, cc, target, config );

        char rsp_path[ BT_PATH_MAX ];
        snprintf( rsp_path, sizeof( rsp_path ), "%s\\%s", obj_dir, rsp_name );
        cc_check_overflow( cc );
        bool ok = cc_assemble( cc, &cmd, rsp_path );
        if ( g_out_flags & ORB_OUT_COMPILE_CMD ) print_raw_cmd( log_out, cmd.buf );
        cc_close_log( log_out );

        if ( !ok ) return false;
    }
    return build_run_cmd_capture_includes( cmd.buf, includes_path ) == 0;
}

/*==============================================================================================

    build_target_compile()

    Full unity compile: all target units + generated reflect file. Adds /showIncludes
    when include tracking is active so the captured output feeds _includes.txt for the next
    incremental check.

==============================================================================================*/

bool
build_target_compile( build_context_t* ctx, target_info_t* target,
                      const char* obj_dir, const char* gen_dir )
{
    compile_cmd_t cc     = { 0 };
    const char*   config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";

    cc_fill_compile_cmd( ctx, target, obj_dir, gen_dir, &cc );

    /* /showIncludes: cl.exe emits a line for each included header with the format:
       "Note: including file: <path>". We capture those lines into _includes.txt
       so we can track header includes incrementally. When a header changes, we
       know to recompile any target that includes it directly or indirectly.

       We only add this flag when include tracking is active because it produces a
       lot of extra output -- every included header, even from the CRT and
       third-party libs -- which would be noisy in the logs when we're not
       actually using it. */

    if ( g_include_track ) CC_APPEND( cc.flags, " /showIncludes" );

    /* sources: Create absolute paths so MSVC error messages are navigable from any CWD.
       _fullpath returns NULL only on truly broken paths; fall back to relative so
       the build still proceeds and cl surfaces a recognisable error instead of a
       silent skip. The `cc.sources[0] ? " " : ""` trick suppresses a leading space
       on the first entry. */
    {
        char rel[ BT_PATH_MAX ], abs_p[ BT_PATH_MAX ];
        for ( int i = 0; target->units[ i ]; ++i )
        {
            snprintf( rel, sizeof( rel ), "%s\\%s", target->root_dir, target->units[ i ] );
            if ( !_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            CC_APPEND( cc.sources, "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }

        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            snprintf( rel, sizeof( rel ), "%s\\%s.generated.c", gen_dir, rname );
            if ( !_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            CC_APPEND( cc.sources, "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }
    }

    /* includes_path: when include tracking is active, we write _includes.txt alongside
       the .obj files with the list of headers included by this target. The incremental
       check reads that file to determine which targets need recompilation when a header changes. */

    char  includes_path[ BT_PATH_MAX ];
    char* includes_out = NULL;
    if ( g_include_track )
    {
        snprintf( includes_path, sizeof( includes_path ), "%s\\_includes.txt", obj_dir );
        includes_out = includes_path;
    }

    return cc_run_compile_cmd( &cc, target, config, obj_dir, "cl.rsp", includes_out );
}

/*==============================================================================================

    build_target_compile_single()

    Compiles one source file with the target's full flag/define/include set.
    Used by the VS Ctrl+F7 path (NMakeCompileFile). No /showIncludes, no includes
    file -- single-file compiles are not tracked incrementally.

==============================================================================================*/

bool
build_target_compile_single( build_context_t* ctx, target_info_t* target,
                             const char* obj_dir, const char* gen_dir, const char* file_path )
{
    compile_cmd_t cc     = { 0 };
    const char*   config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";

    cc_fill_compile_cmd( ctx, target, obj_dir, gen_dir, &cc );

    /* source: the single file VS handed us */
    CC_APPEND( cc.sources, "%s", file_path );

    return cc_run_compile_cmd( &cc, target, config, obj_dir, "cl_file.rsp", NULL );
}

/*==============================================================================================

    build_target_link()

    Fill a link_cmd_t, print the active sections, assemble the command, and run it.
      TARGET_STATIC_LIB  -> lib.exe
      TARGET_DYNAMIC_LIB -> link.exe /DLL /IMPLIB
      TARGET_EXECUTABLE  -> link.exe

==============================================================================================*/

bool
build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir )
{
    link_cmd_t lk = { 0 };

    // In monolithic mode, dynamic modules are archived as static libs instead
    // of DLLs. This lets host executables link them directly with no
    // hot-reload overhead. We compute an "effective" type once so the rest of
    // the function can branch on lib-vs-exe behaviour without re-checking the
    // monolithic flag everywhere.
    target_type_t effective_type = target->type;
    if ( ctx->is_monolithic && effective_type == TARGET_DYNAMIC_LIB )
        effective_type = TARGET_STATIC_LIB;

    if ( effective_type == TARGET_STATIC_LIB )
    {
        // --- Static library (lib.exe) ----------------------------------
        // lib.exe is the archiver: it bundles obj files into a .lib. No
        // linking, no PDB, no dep resolution -- just a flat archive of
        // every *.obj in obj_dir.
        snprintf( lk.exe,      sizeof( lk.exe ),      "lib.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin\\%s.lib", target->name );
        snprintf( lk.flags,    sizeof( lk.flags ),     "/nologo" );
        snprintf( lk.output,   sizeof( lk.output ),    "/OUT:bin\\%s.lib", target->name );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s\\*.obj", obj_dir );
        // lk.pdb and lk.libs stay empty by zero-init -- lib.exe ignores both.
    }
    else
    {
        // --- Executable or DLL (link.exe) ------------------------------
        // Same tool, two output shapes selected via /DLL.
        const char* ext = ( effective_type == TARGET_DYNAMIC_LIB ) ? ".dll" : ".exe";

        snprintf( lk.exe,      sizeof( lk.exe ),      "link.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin\\%s%s", target->name, ext );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s\\*.obj", obj_dir );

        // Flags: /DLL flips link.exe from "build EXE" into "build DLL" mode.
        if ( effective_type == TARGET_DYNAMIC_LIB )
            snprintf( lk.flags, sizeof( lk.flags ), "/nologo /DLL" );
        else
            snprintf( lk.flags, sizeof( lk.flags ), "/nologo" );

        // Output: DLLs also produce an "import library" -- the .lib that
        // dependents link against to bind to the DLL's exports. /IMPLIB
        // tells link.exe to emit it alongside the .dll.
        if ( effective_type == TARGET_DYNAMIC_LIB )
            snprintf( lk.output, sizeof( lk.output ),
                      "/OUT:bin/%s.dll /IMPLIB:bin/%s.lib", target->name, target->name );
        else
            snprintf( lk.output, sizeof( lk.output ), "/OUT:bin\\%s.exe", target->name );

        // PDB: every link produces a uniquely-timestamped PDB so a debugger
        // holding the previous one open can never block the linker. Stale
        // (unlocked) leftovers from earlier builds are garbage-collected
        // by cleanup_stale_pdbs() before we emit the new name.
        cleanup_stale_pdbs( target->name );
        snprintf( lk.pdb, sizeof( lk.pdb ),
                  "/DEBUG /PDB:bin/%s_%lld.pdb", target->name, ( long long )time( NULL ) );

        // Libs to feed link.exe: every declared dep's .lib, plus the four
        // Windows system libs every target uses. The `[0] ? " " : ""` trick
        // suppresses a leading space when lk.libs is still empty -- same
        // pattern as the sources list above.
        for ( int i = 0; target->deps[ i ]; ++i )
            CC_APPEND( lk.libs, "%sbin/%s.lib",
                      lk.libs[ 0 ] ? " " : "", target->deps[ i ] );
        CC_APPEND( lk.libs, "%suser32.lib shell32.lib gdi32.lib advapi32.lib",
                  lk.libs[ 0 ] ? " " : "" );
    }

    // Print sections, assemble, optionally echo raw command, then run.
    FILE* log_out = cc_open_log();
    lk_print( log_out, &lk, target );

    char      rsp_path[ BT_PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s\\%s.rsp", obj_dir,
              target->type == TARGET_STATIC_LIB ? "lib" : "link" );
    lk_check_overflow( &lk );
    lk_assemble( &lk, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_LINK_CMD ) print_raw_cmd( log_out, cmd.buf );
    cc_close_log( log_out );

    // NULL includes_path: no /showIncludes parsing needed for link/lib, but we still
    // want line-by-line capture so [MSVC] prefixing and ORB_OUT_MSVC_OUTPUT gating apply.
    return build_run_cmd_capture_includes( cmd.buf, NULL ) == 0;
}

/*============================================================================================*/
// clang-format on