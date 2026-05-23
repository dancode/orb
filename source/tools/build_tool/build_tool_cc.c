/*==============================================================================================

    build_tool_cc.c -- Compiler and linker command generation.

    Builds the cl.exe / link.exe / lib.exe command lines for a single target.
    Commands are assembled section-by-section into compile_cmd_t / link_cmd_t
    structs so each logical group (flags, defines, includes, sources, output)
    can be printed independently under g_out_flags control, then joined into
    one string for actual execution.

    Two public entry points, both called from build_target():
      build_target_compile() -- cl.exe with /showIncludes for dep capture.
      build_target_link()    -- lib.exe for static libs, link.exe otherwise.

    Flag/define lockstep:
      The defines emitted here must match the IntelliSense defines in
      build_tool_gen.c. Drift surfaces as IDE squigglies that don't reproduce
      at build time.

==============================================================================================*/
// clang-format off

/*============================================================================================*/
// --- Structured command types ---
//
// Each field holds the raw fragment that goes into the final cl/link/lib
// command. Assembled in order via cc_assemble() / lk_assemble(); printed
// selectively via cc_print() / lk_print() based on g_out_flags.

typedef struct
{
    char exe     [ 64          ]; // cl.exe or clang-cl.exe
    char flags   [ 512         ]; // /c /nologo /W4 /WX /Zi /Od /MDd ...
    char includes[ 512         ]; // /I source /I gen_dir
    char defines [ 1024        ]; // /DOS_WINDOWS /DCOMPILE_MSVC /D_DEBUG ...
    char output  [ 512         ]; // /FoobjDir/ /FdobjDir/
    char sources [ CMD_BUF_MAX ]; // absolute .c paths
                               
} compile_cmd_t;               
                               
typedef struct                 
{                              
    char exe     [ 32          ]; // lib.exe or link.exe
    char artifact[ BT_PATH_MAX ]; // final output path (summary display only)
    char flags   [ 256         ]; // /nologo /DLL ...
    char output  [ 512         ]; // /OUT:... /IMPLIB:...
    char pdb     [ 256         ]; // /DEBUG /PDB:... (empty for lib.exe)
    char inputs  [ 512         ]; // objDir/*.obj
    char libs    [ 1024        ]; // dep.lib ... user32.lib ...

} link_cmd_t;

/*============================================================================================*/
// --- Internal helpers ---

// Append a formatted string to a fixed-size field.
// Reports overflow via printf so it's never a silent data loss.
static void
cc_field( char* dst, size_t dst_size, const char* fmt, ... )
{
    size_t used = strlen( dst );
    if ( used >= dst_size - 1 )
    {
        printf( ORB_INDENT "[orb error] cc_field overflow (capacity %zu)\n", dst_size );
        return;
    }
    size_t  remaining = dst_size - used;
    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( dst + used, remaining, fmt, args );
    va_end( args );
    if ( written < 0 || ( size_t )written >= remaining )
        printf( ORB_INDENT "[orb error] cc_field truncated (needed %d, had %zu)\n", written, remaining );
}

// Convenience wrapper around cc_field(): infers the destination capacity from
// the field's declared size, removing the need for callers to pass sizeof().
#define CC_APPEND( field, ... ) cc_field( ( field ), sizeof( field ), __VA_ARGS__ )

// get_target_upper() -- derive <TARGET>_STATIC from a target name.
// Must match the IntelliSense defines emitted by build_tool_gen.c (unity build).
static void
get_target_upper( const char* name, char* out )
{
    strcpy( out, name );
    for ( char* p = out; *p; ++p ) *p = ( char )toupper( *p );
}

// cleanup_stale_pdbs() -- remove PDB files from previous links.
//
// Each link emits a uniquely-named bin/<target>_<timestamp>.pdb so the linker
// never has to overwrite a PDB that a debugger may still hold open. Across
// many builds those uniquely-named files accumulate; this routine sweeps the
// unlocked ones away at the start of each link.
//
// remove() silently fails for any PDB still locked by an attached debugger,
// which is exactly what we want — we keep going, the held file survives, and
// the new link goes to a fresh name with a newer timestamp.
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
    if ( h == -1 ) return;          // No matches → nothing to do.

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

// Return the active output sink: per-thread log when inside a parallel worker
// (so section output lands with child-process output), stdout otherwise.
// Caller is responsible for fclose() if the returned FILE* != stdout.
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

/*============================================================================================*/
// --- Section printers ---

// Print space-separated tokens from `raw`, stripping the leading `prefix`
// from each token when non-NULL (e.g. "/D" strips preprocessor flag chars).
//
// Example: raw = "/DOS_WINDOWS /DARCH_X64", prefix = "/D"
//          prints: "OS_WINDOWS ARCH_X64\n"
//
// This is the workhorse that renders the human-readable section view of
// flags / defines / includes / etc. We can't just printf(raw) because the
// caller wants per-token prefix-stripping and a final newline.
// Returns true if at least one token was written (content was non-empty).
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
        if ( !*p ) break;             // ran off the end inside the gap → done

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
        // fwrite is used instead of fputs because `s` is NOT null-terminated —
        // it points into the middle of `raw`.
        if ( !first ) fputc( ' ', out );
        fwrite( s, 1, len, out );
        first = false;
    }
    fputc( '\n', out );
    return !first;  // first stayed true → no tokens → empty content
}

// Print a labeled section line: "  <label>  <content>".
// Returns true if content had tokens (non-empty). `strip` is forwarded to print_tokens.
#define SECTION_FMT  "            %-10s"

static bool
print_section( FILE* out, const char* label, const char* content, const char* strip )
{
    fprintf( out, SECTION_FMT, label );
    return print_tokens( out, content, strip );
}

// Print the compile output section: /FoobjDir/ /FdobjDir/ → "obj=... pdb=..."
//
// Specialised version of print_tokens(): instead of just stripping one fixed
// prefix, it inspects each token's prefix character to decide which human
// label ("obj=" or "pdb=") to print in front of the stripped path.
//
// Example: raw = "/Fobuild\\obj\\foo/ /Fdbuild\\obj\\foo/"
//          prints: "obj=build\obj\foo/  pdb=build\obj\foo/\n"
static bool
print_compile_output( FILE* out, const char* raw )
{
    fprintf( out, SECTION_FMT, "output:" );
    const char* p     = raw;
    bool        first = true;

    while ( *p )
    {
        // Skip whitespace, find the token bounds — same scan loop as print_tokens.
        while ( *p == ' ' ) ++p;
        if ( !*p ) break;
        const char* s   = p;
        while ( *p && *p != ' ' ) ++p;
        int         len = (int)( p - s );

        // Decide which prefix this token uses, if any. The MSVC flags are
        // "/Foxxx" (object output dir) and "/Fdxxx" (compile-PDB output dir).
        // `len > 3` ensures there is at least one character after "/Fo" or
        // "/Fd" to print — empty flags would just produce noise.
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

// Print compile command sections to `out` according to g_out_flags.
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

// Print link command sections to `out` according to g_out_flags.
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

/*============================================================================================*/
// --- Command assembly ---
//
// Join the struct fields into a single string for the actual cl/link/lib call.
// Response-file spill happens here when the assembled string exceeds the shell
// arg limit — same mechanism as before, now operating on the joined result.

// Join the compile fields in a fixed order into one command line:
//   <exe> <flags> <includes> <defines> <output> <sources>
// The sources field alone can be CMD_BUF_MAX bytes; joining everything through
// cmd_buf_t (also CMD_BUF_MAX) would silently truncate the sources before the
// spill check ever runs. Instead, format args into a dedicated oversize buffer,
// measure the total, then write the rsp file directly from that buffer while
// the data is still complete.
static void
cc_assemble( const compile_cmd_t* cc, cmd_buf_t* cmd, const char* rsp_path )
{
    // ARGS_BUF covers the maximum: sources (CMD_BUF_MAX) + all other fields
    // (flags 512 + includes 512 + defines 1024 + output 512 + 4 spaces).
    enum { ARGS_BUF = CMD_BUF_MAX + 3200 };
    char args[ ARGS_BUF ];
    int  written = snprintf( args, sizeof( args ), "%s %s %s %s %s",
                             cc->flags, cc->includes, cc->defines, cc->output, cc->sources );
    if ( written < 0 || ( size_t )written >= sizeof( args ) )
        printf( ORB_INDENT "[orb error] cc_assemble args truncated (needed %d)\n", written );

    size_t total = strlen( cc->exe ) + 1 + ( size_t )( written < 0 ? 0 : written );
    if ( total >= CMD_RSP_THRESHOLD )
    {
        // Write the full args to the response file before any truncation can occur.
        FILE* f = fopen( rsp_path, "w" );
        if ( f ) { fputs( args, f ); fclose( f ); }
        else printf( ORB_INDENT "[orb error] could not open response file %s\n", rsp_path );
        cmd->size      = 0;
        cmd->truncated = false;
        cmd_append( cmd, "%s @%s", cc->exe, rsp_path );
    }
    else
    {
        cmd_append( cmd, "%s %s", cc->exe, args );
    }
}

// Same idea for the link/archive side. The PDB section is optional: lib.exe
// doesn't take a /DEBUG /PDB pair, so we omit that field when lk->pdb is
// empty rather than emitting a stray pair of spaces.
static void
lk_assemble( const link_cmd_t* lk, cmd_buf_t* cmd, const char* rsp_path )
{
    if ( lk->pdb[ 0 ] )
        cmd_append( cmd, "%s %s %s %s %s %s", lk->exe, lk->flags, lk->output, lk->pdb, lk->inputs, lk->libs );
    else
        cmd_append( cmd, "%s %s %s %s %s",    lk->exe, lk->flags, lk->output, lk->inputs, lk->libs );
    cmd_spill_to_response_file( cmd, rsp_path );
}

/*============================================================================================*/
// --- Raw-command echo (ORB_OUT_*_CMD) ---

// Print the assembled command to `out` when the caller's CMD flag is set.
// Wraps at 100 columns after the first token so long lines stay readable.
//
// Each token is kept intact (no mid-token line breaks). If appending the next
// token would push past k_wrap, we line-break first and indent the new line
// by k_cont spaces so the continuation visually nests under the command.
static void
print_raw_cmd( FILE* out, const char* cmd )
{
    static const int k_wrap = 100;  // soft line width target
    static const int k_cont = 22;   // continuation indent width — matches ORB_INDENT(10) + "[orb cmd]   "(12)

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
        //   (a) The token would push the line past k_wrap → wrap to a new
        //       indented line. col == 0 after wrap, no leading space needed.
        //   (b) Token fits on the current line → emit a single separator
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

/*============================================================================================*/
// --- MSVC output classification ---

// Returns true for the bare source-file banner cl.exe prints once per compiled
// translation unit (e.g. it prints "rs_gen.c" on its own line before emitting
// any diagnostics for that TU). We filter these out of our log dumps because
// the orb build log already shows the source list in the [orb compiling] section
// — leaving cl's echo in just creates duplicate noise.
//
// A line qualifies as a source echo iff (after trimming whitespace) it is:
//   - non-empty
//   - contains no internal spaces or path separators (so "src/foo.c" doesn't
//     match — that's a diagnostic, not a banner)
//   - has a '.' producing an extension equal to "c" or "cpp" (case-insensitive)
//
// Implementation walks the line in three passes for readability over speed:
// the volume here is tiny (a few hundred lines per build at worst).
static bool
is_msvc_source_echo( const char* line )
{
    // 1. Skip any leading whitespace so " foo.c" matches just like "foo.c".
    while ( *line == ' ' || *line == '\t' ) ++line;

    // 2. Compute the effective length, ignoring any trailing CR / LF / space
    //    that survived the line-buffer split. We work with a length rather
    //    than mutating the buffer because the caller may want the line again.
    int len = ( int )strlen( line );
    while ( len > 0 && ( line[ len - 1 ] == '\n'
                      || line[ len - 1 ] == '\r'
                      || line[ len - 1 ] == ' ' ) ) --len;
    if ( len == 0 ) return false;

    // 3. Reject anything that contains an interior space or a path separator —
    //    cl's TU banner is a bare filename, never a path. This is what
    //    distinguishes "foo.c" (echo) from "F:\orb\src\foo.c(12): error C..."
    //    (diagnostic — keep!).
    for ( int i = 0; i < len; ++i )
        if ( line[ i ] == ' ' || line[ i ] == '/' || line[ i ] == '\\' ) return false;

    // 4. Find the last '.' so we can look at the extension. No dot = no
    //    extension = not a source banner.
    int dot = -1;
    for ( int i = len - 1; i >= 0; --i ) { if ( line[ i ] == '.' ) { dot = i; break; } }
    if ( dot < 0 ) return false;

    // 5. Compare the extension (case-insensitive). We use _strnicmp + an
    //    explicit length check rather than _stricmp because `line` may still
    //    have trailing CR/LF/spaces past `len` (we trimmed by adjusting len
    //    only, not by null-terminating). E.g. "foo.c\n" gives ext = "c\n"
    //    and _stricmp("c\n", "c") would mismatch — losing the filter.
    const char* ext     = line + dot + 1;
    int         ext_len = len - ( dot + 1 );
    if ( ext_len == 1 && _strnicmp( ext, "c",   1 ) == 0 ) return true;
    if ( ext_len == 3 && _strnicmp( ext, "cpp", 3 ) == 0 ) return true;
    return false;
}

/*============================================================================================*/
// --- Compilation ---

/**
 * build_target_compile()
 *
 * Fill a compile_cmd_t section-by-section, print the active sections to the
 * build log (or stdout in serial mode), assemble the full cl.exe command, and
 * run it via build_run_cmd_capture_deps so /showIncludes output is parsed into
 * a per-target dep file for the next incremental check.
 */
bool
build_target_compile( build_context_t* ctx, target_info_t* target,
                      const char* obj_dir, const char* gen_dir )
{
    compile_cmd_t cc = { 0 };
    const char*   config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";

    // Exe
    snprintf( cc.exe, sizeof( cc.exe ), "%s", ctx->compiler == COMPILE_CLANG ? "clang-cl.exe" : "cl.exe" );

    // Flags: compiler settings common to all targets.
    // /showIncludes drives the dep-capture path in build_run_cmd_capture_deps;
    // it produces a "Note: including file:" line for every resolved header.
    CC_APPEND( cc.flags,
              "/c /nologo /W4 /WX /Zc:preprocessor /std:c11 /showIncludes" );
    if ( ctx->config == CONFIG_DEBUG )
        CC_APPEND( cc.flags, " /Zi /Od /MDd" );
    else
        CC_APPEND( cc.flags, " /O2 /MD" );

    // Includes: header search paths.
    // Trailing space intentional — fields are space-joined at assembly time.
    CC_APPEND( cc.includes, "/I source /I %s", gen_dir );

    // Defines: preprocessor symbols every TU sees.
    // Must stay in lockstep with the IntelliSense defines in build_tool_gen.c.
    CC_APPEND( cc.defines, "/D_CRT_SECURE_NO_WARNINGS" );
    {
        char upper[ 128 ];
        get_target_upper( target->name, upper );
        CC_APPEND( cc.defines, " /D%s_STATIC", upper );
    }
    // Propagate _STATIC for each dep so API gateways resolve correctly.
    // Static lib deps are always static; dynamic lib deps become static in monolithic mode.
    for ( int i = 0; target->deps[ i ]; ++i )
    {
        target_info_t* dep = find_target( target->deps[ i ] );
        if ( !dep ) continue;
        bool dep_is_static = ( dep->type == TARGET_STATIC_LIB ) ||
                             ( dep->type == TARGET_DYNAMIC_LIB && ctx->is_monolithic );
        if ( dep_is_static )
        {
            char dep_upper[ 128 ];
            get_target_upper( dep->name, dep_upper );
            CC_APPEND( cc.defines, " /D%s_STATIC", dep_upper );
        }
    }
    if ( ctx->is_monolithic )
        CC_APPEND( cc.defines, " /DBUILD_STATIC" );
    if ( ctx->config == CONFIG_DEBUG )
        CC_APPEND( cc.defines, " /D_DEBUG" );
    else
        CC_APPEND( cc.defines, " /DNDEBUG" );

    // Warning suppressions: filter g_warn_suppressions[] by active config and compiler.
    {
        for ( int i = 0; i < g_warn_suppression_count; ++i )
        {
            warn_suppress_t* s = &g_warn_suppressions[ i ];
            if ( ( s->config == ctx->config || s->config == CONFIG_COUNT ) &&
                 ( s->compiler_mask & (unsigned int)ctx->compiler ) )
                CC_APPEND( cc.flags, " %s", s->flag );
        }
    }

    // Output dirs: /Fo = .obj destination, /Fd = compile-PDB destination.
    // Trailing slash is required — without it cl treats the path as a
    // filename prefix instead of a directory.
    CC_APPEND( cc.output, "/Fo%s/ /Fd%s/", obj_dir, obj_dir );

    // Sources: absolute paths so MSVC error messages are navigable from any
    // context (terminal, log file, IDE) regardless of the viewer's CWD.
    //
    // For each unit: build the relative path, run it through _fullpath() to
    // canonicalize to an absolute path, and append it to the sources field
    // separated by a single space. The `cc.sources[ 0 ] ? " " : ""` trick
    // suppresses the leading space on the first entry — the field starts
    // out as an empty string, so the test is "is anything here yet?".
    {
        char rel[ BT_PATH_MAX ], abs_p[ BT_PATH_MAX ];
        for ( int i = 0; target->units[ i ]; ++i )
        {
            snprintf( rel, sizeof( rel ), "%s\\%s", target->root_dir, target->units[ i ] );
            // _fullpath returns NULL only on truly broken paths; fall back to
            // the relative form so the build still proceeds and the user
            // sees a recognisable error from cl rather than a silent skip.
            if ( !_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            CC_APPEND( cc.sources, "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }
        // Reflection-generated TU (written by build_reflect.exe in step 5
        // of build_target). Same absolute-path treatment as the user units.
        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            snprintf( rel, sizeof( rel ), "%s\\%s.generated.c", gen_dir, rname );
            if ( !_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            CC_APPEND( cc.sources, "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }
    }

    // Print the requested sections. Routes to the per-target log when called
    // from a parallel worker so the output lands with child-process output.
    FILE* log_out = cc_open_log();
    cc_print( log_out, &cc, target, config );

    // Assemble the full command, optionally echo the raw line, then run.
    char      rsp_path[ BT_PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s\\cl.rsp", obj_dir );
    cc_assemble( &cc, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_COMPILE_CMD ) print_raw_cmd( log_out, cmd.buf );
    if ( log_out != stdout ) fclose( log_out );

    char deps_path[ BT_PATH_MAX ];
    snprintf( deps_path, sizeof( deps_path ), "%s\\_deps.txt", obj_dir );
    return build_run_cmd_capture_deps( cmd.buf, deps_path ) == 0;
}

/*============================================================================================*/
// --- Single-file Compilation ---

/**
 * build_target_compile_single()
 *
 * Compiles one source file with the target's full flag/define/include set.
 * Mirrors build_target_compile() but omits /showIncludes (no dep tracking)
 * and replaces the full unity source list with the single file path VS passes
 * via $(NMakeCompileFile). No link step — this is for error feedback only.
 */
bool
build_target_compile_single( build_context_t* ctx, target_info_t* target,
                             const char* obj_dir, const char* gen_dir, const char* file_path )
{
    compile_cmd_t cc      = { 0 };
    const char*   config  = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";

    // Exe
    snprintf( cc.exe, sizeof( cc.exe ), "%s", ctx->compiler == COMPILE_CLANG ? "clang-cl.exe" : "cl.exe" );

    // Flags: same as full compile, but no /showIncludes (dep tracking not needed).
    CC_APPEND( cc.flags, "/c /nologo /W4 /WX /Zc:preprocessor /std:c11" );
    if ( ctx->config == CONFIG_DEBUG )
        CC_APPEND( cc.flags, " /Zi /Od /MDd" );
    else
        CC_APPEND( cc.flags, " /O2 /MD" );

    // Includes, defines: identical to build_target_compile().
    CC_APPEND( cc.includes, "/I source /I %s", gen_dir );

    CC_APPEND( cc.defines,
              "/DOS_WINDOWS /DCOMPILE_MSVC /DARCH_X64 /D_CRT_SECURE_NO_WARNINGS" );
    {
        char upper[ 128 ];
        get_target_upper( target->name, upper );
        CC_APPEND( cc.defines, " /D%s_STATIC", upper );
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
            get_target_upper( dep->name, dep_upper );
            CC_APPEND( cc.defines, " /D%s_STATIC", dep_upper );
        }
    }
    if ( ctx->is_monolithic )
        CC_APPEND( cc.defines, " /DBUILD_STATIC" );
    if ( ctx->config == CONFIG_DEBUG )
        CC_APPEND( cc.defines, " /D_DEBUG" );
    else
        CC_APPEND( cc.defines, " /DNDEBUG" );

    // Warning suppressions: same table, same logic as build_target_compile().
    {
        for ( int i = 0; i < g_warn_suppression_count; ++i )
        {
            warn_suppress_t* s = &g_warn_suppressions[ i ];
            if ( ( s->config == ctx->config || s->config == CONFIG_COUNT ) &&
                 ( s->compiler_mask & (unsigned int)ctx->compiler ) )
                CC_APPEND( cc.flags, " %s", s->flag );
        }
    }

    // Output into the same obj_dir as a full build so the .obj lands where the linker expects it.
    CC_APPEND( cc.output, "/Fo%s/ /Fd%s/", obj_dir, obj_dir );

    // Source: just the one file VS handed us.
    CC_APPEND( cc.sources, "%s", file_path );

    // Print active sections; single-file runs are always serial so stdout is fine.
    FILE* log_out = cc_open_log();
    cc_print( log_out, &cc, target, config );

    char      rsp_path[ BT_PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s\\cl_file.rsp", obj_dir );
    cc_assemble( &cc, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_COMPILE_CMD ) print_raw_cmd( log_out, cmd.buf );
    if ( log_out != stdout ) fclose( log_out );

    // No deps_path: single-file compiles are not tracked incrementally.
    return build_run_cmd_capture_deps( cmd.buf, NULL ) == 0;
}

/*============================================================================================*/
// --- Linking / Archiving ---

/**
 * build_target_link()
 *
 * Fill a link_cmd_t, print the active sections, assemble the command, and run
 * it via build_run_cmd.
 *  - TARGET_STATIC_LIB  -> lib.exe
 *  - TARGET_DYNAMIC_LIB -> link.exe /DLL /IMPLIB
 *  - TARGET_EXECUTABLE  -> link.exe
 */
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
        // linking, no PDB, no dep resolution — just a flat archive of
        // every *.obj in obj_dir.
        snprintf( lk.exe,      sizeof( lk.exe ),      "lib.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin\\%s.lib", target->name );
        snprintf( lk.flags,    sizeof( lk.flags ),     "/nologo" );
        snprintf( lk.output,   sizeof( lk.output ),    "/OUT:bin\\%s.lib", target->name );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s\\*.obj", obj_dir );
        // lk.pdb and lk.libs stay empty by zero-init — lib.exe ignores both.
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

        // Output: DLLs also produce an "import library" — the .lib that
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
        // suppresses a leading space when lk.libs is still empty — same
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
    lk_assemble( &lk, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_LINK_CMD ) print_raw_cmd( log_out, cmd.buf );
    if ( log_out != stdout ) fclose( log_out );

    // NULL deps_path: no /showIncludes parsing needed for link/lib, but we still
    // want line-by-line capture so [MSVC] prefixing and ORB_OUT_MSVC_OUTPUT gating apply.
    return build_run_cmd_capture_deps( cmd.buf, NULL ) == 0;
}

/*============================================================================================*/
// clang-format on