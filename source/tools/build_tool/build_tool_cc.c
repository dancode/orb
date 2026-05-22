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
#include "build_tool.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <io.h>
#include <time.h>

/*============================================================================================*/
// --- Structured command types ---
//
// Each field holds the raw fragment that goes into the final cl/link/lib
// command. Assembled in order via cc_assemble() / lk_assemble(); printed
// selectively via cc_print() / lk_print() based on g_out_flags.

typedef struct
{
    char exe     [ 64         ]; // cl.exe or clang-cl.exe
    char flags   [ 512        ]; // /c /nologo /W4 /WX /Zi /Od /MDd ...
    char includes[ 512        ]; // /I source /I gen_dir
    char defines [ 1024       ]; // /DOS_WINDOWS /DCOMPILER_MSVC /D_DEBUG ...
    char output  [ 512        ]; // /FoobjDir/ /FdobjDir/
    char sources [ CMD_BUF_MAX]; // absolute .c paths

} compile_cmd_t;

typedef struct
{
    char exe     [ 32         ]; // lib.exe or link.exe
    char artifact[ BT_PATH_MAX]; // final output path (summary display only)
    char flags   [ 256        ]; // /nologo /DLL ...
    char output  [ 512        ]; // /OUT:... /IMPLIB:...
    char pdb     [ 256        ]; // /DEBUG /PDB:... (empty for lib.exe)
    char inputs  [ 512        ]; // objDir/*.obj
    char libs    [ 1024       ]; // dep.lib ... user32.lib ...

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

// get_target_upper() -- derive <TARGET>_STATIC from a target name.
// Must match the IntelliSense defines emitted by build_tool_gen.c (unity build).
static void
get_target_upper( const char* name, char* out )
{
    strcpy( out, name );
    for ( char* p = out; *p; ++p ) *p = ( char )toupper( *p );
}

// cleanup_stale_pdbs() -- remove PDB files from previous links.
// Files held by an attached debugger fail remove() silently; the new link
// writes a fresh uniquely-named PDB so there is never a collision.
static void
cleanup_stale_pdbs( const char* target_name )
{
    char pattern[ BT_PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "bin/%s_*.pdb", target_name );

    struct _finddata_t fd;
    intptr_t h = _findfirst( pattern, &fd );
    if ( h == -1 ) return;
    do
    {
        char path[ BT_PATH_MAX ];
        snprintf( path, sizeof( path ), "bin/%s", fd.name );
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
static void
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
}

// Print a labeled section line: "  <label>  <content>".
// `strip` is forwarded to print_tokens — pass NULL for no stripping.
#define SECTION_FMT  "            %-10s"

static void
print_section( FILE* out, const char* label, const char* content, const char* strip )
{
    fprintf( out, SECTION_FMT, label );
    print_tokens( out, content, strip );
}

// Print the compile output section: /FoobjDir/ /FdobjDir/ → "obj=... pdb=..."
static void
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
        // /Fo prefix → obj label; /Fd prefix → pdb label.
        const char* label = NULL;
        if ( len > 3 && s[ 0 ] == '/' && s[ 1 ] == 'F' )
        {
            if ( s[ 2 ] == 'o' ) { label = "obj=";  s += 3; len -= 3; }
            if ( s[ 2 ] == 'd' ) { label = "  pdb="; s += 3; len -= 3; }
        }
        if ( !first && !label ) fputc( ' ', out );
        if ( label ) fputs( label, out );
        fwrite( s, 1, len, out );
        first = false;
    }
    fputc( '\n', out );
}

// Print compile command sections to `out` according to g_out_flags.
static void
cc_print( FILE* out, const compile_cmd_t* cc, const target_info_t* target, const char* config )
{
    if ( g_out_flags & ORB_OUT_COMPILE_SUMMARY )
        fprintf( out, ORB_INDENT "[orb compile] %s (%s)\n\n", target->name, config );
    if ( g_out_flags & ORB_OUT_COMPILE_SOURCES  ) print_section( out, "sources:",  cc->sources,  NULL );
    if ( g_out_flags & ORB_OUT_COMPILE_FLAGS    ) print_section( out, "flags:",    cc->flags,    NULL );
    if ( g_out_flags & ORB_OUT_COMPILE_DEFINES  ) print_section( out, "defines:",  cc->defines,  "/D" );
    if ( g_out_flags & ORB_OUT_COMPILE_INCLUDES ) print_section( out, "includes:", cc->includes, "/I" );
    if ( g_out_flags & ORB_OUT_COMPILE_OUTPUT   ) print_compile_output( out, cc->output );
}

// Print link command sections to `out` according to g_out_flags.
static void
lk_print( FILE* out, const link_cmd_t* lk, const target_info_t* target )
{
    if ( g_out_flags & ORB_OUT_LINK_SUMMARY )
        fprintf( out, ORB_INDENT "[orb link] %s -> %s\n\n", target->name, lk->artifact );
    if ( g_out_flags & ORB_OUT_LINK_INPUTS  ) print_section( out, "inputs:",  lk->inputs,  NULL );
    if ( g_out_flags & ORB_OUT_LINK_LIBS    ) print_section( out, "libs:",    lk->libs,    NULL );
    if ( g_out_flags & ORB_OUT_LINK_FLAGS   ) print_section( out, "flags:",   lk->flags,   NULL );
    if ( g_out_flags & ORB_OUT_LINK_OUTPUT  ) print_section( out, "output:",  lk->output,  "/OUT:" );
    if ( g_out_flags & ORB_OUT_LINK_PDB     ) print_section( out, "pdb:",     lk->pdb,     "/PDB:" );
}

/*============================================================================================*/
// --- Command assembly ---
//
// Join the struct fields into a single string for the actual cl/link/lib call.
// Response-file spill happens here when the assembled string exceeds the shell
// arg limit — same mechanism as before, now operating on the joined result.

static void
cc_assemble( const compile_cmd_t* cc, cmd_buf_t* cmd, const char* rsp_path )
{
    cmd_append( cmd, "%s %s %s %s %s %s",
                cc->exe, cc->flags, cc->includes, cc->defines, cc->output, cc->sources );
    cmd_spill_to_response_file( cmd, rsp_path );
}

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
static void
print_raw_cmd( FILE* out, const char* cmd )
{
    static const int k_wrap = 100;
    static const int k_cont = 20;  // continuation indent width
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
    fputc( '\n', out );
}

/*============================================================================================*/
// --- MSVC output classification ---

// Returns true for bare source-file echo lines cl.exe emits per compiled TU
// (e.g. "rs_gen.c") — plain filename, no spaces or separators, .c/.cpp extension.
// Filtered out of log dumps since the orb build log already shows sources.
static bool
is_msvc_source_echo( const char* line )
{
    while ( *line == ' ' || *line == '\t' ) ++line;
    int len = ( int )strlen( line );
    while ( len > 0 && ( line[ len - 1 ] == '\n' || line[ len - 1 ] == '\r' || line[ len - 1 ] == ' ' ) ) --len;
    if ( len == 0 ) return false;
    for ( int i = 0; i < len; ++i )
        if ( line[ i ] == ' ' || line[ i ] == '/' || line[ i ] == '\\' ) return false;
    int dot = -1;
    for ( int i = len - 1; i >= 0; --i ) { if ( line[ i ] == '.' ) { dot = i; break; } }
    if ( dot < 0 ) return false;
    const char* ext = line + dot + 1;
    return ( _stricmp( ext, "c" ) == 0 || _stricmp( ext, "cpp" ) == 0 );
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
    const char*   config = ( ctx->config == BT_CONFIG_DEBUG ) ? "Debug" : "Release";

    // Exe
    snprintf( cc.exe, sizeof( cc.exe ), "%s", ctx->compiler == BT_COMPILER_CLANG ? "clang-cl.exe" : "cl.exe" );

    // Flags: compiler settings common to all targets.
    // /showIncludes drives the dep-capture path in build_run_cmd_capture_deps;
    // it produces a "Note: including file:" line for every resolved header.
    cc_field( cc.flags, sizeof( cc.flags ),
              "/c /nologo /W4 /WX /Zc:preprocessor /std:c11 /showIncludes" );
    if ( ctx->config == BT_CONFIG_DEBUG )
        cc_field( cc.flags, sizeof( cc.flags ), " /Zi /Od /MDd" );
    else
        cc_field( cc.flags, sizeof( cc.flags ), " /O2 /MD" );

    // Includes: header search paths.
    // Trailing space intentional — fields are space-joined at assembly time.
    cc_field( cc.includes, sizeof( cc.includes ), "/I source /I %s", gen_dir );

    // Defines: preprocessor symbols every TU sees.
    // Must stay in lockstep with the IntelliSense defines in build_tool_gen.c.
    cc_field( cc.defines, sizeof( cc.defines ),
              "/DOS_WINDOWS /DCOMPILER_MSVC /DARCH_X64 /D_CRT_SECURE_NO_WARNINGS" );
    {
        char upper[ 128 ];
        get_target_upper( target->name, upper );
        cc_field( cc.defines, sizeof( cc.defines ), " /D%s_STATIC", upper );
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
            cc_field( cc.defines, sizeof( cc.defines ), " /D%s_STATIC", dep_upper );
        }
    }
    if ( ctx->is_monolithic )
        cc_field( cc.defines, sizeof( cc.defines ), " /DBUILD_STATIC" );
    if ( ctx->config == BT_CONFIG_DEBUG )
        cc_field( cc.defines, sizeof( cc.defines ), " /D_DEBUG" );
    else
        cc_field( cc.defines, sizeof( cc.defines ), " /DNDEBUG" );

    // Warning suppressions: filter g_warn_suppressions[] by active config and compiler.
    {
        for ( int i = 0; i < g_warn_suppression_count; ++i )
        {
            warn_suppress_t* s = &g_warn_suppressions[ i ];
            if ( ( s->config == ctx->config || s->config == BT_CONFIG_COUNT ) &&
                 ( s->compiler_mask & (unsigned int)ctx->compiler ) )
                cc_field( cc.flags, sizeof( cc.flags ), " %s", s->flag );
        }
    }

    // Output dirs: /Fo = .obj destination, /Fd = compile-PDB destination.
    // Trailing slash is required — without it cl treats the path as a
    // filename prefix instead of a directory.
    cc_field( cc.output, sizeof( cc.output ), "/Fo%s/ /Fd%s/", obj_dir, obj_dir );

    // Sources: absolute paths so MSVC error messages are navigable from any
    // context (terminal, log file, IDE) regardless of the viewer's CWD.
    {
        char rel[ BT_PATH_MAX ], abs_p[ BT_PATH_MAX ];
        for ( int i = 0; target->units[ i ]; ++i )
        {
            snprintf( rel, sizeof( rel ), "%s/%s", target->root_dir, target->units[ i ] );
            if ( !_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            cc_field( cc.sources, sizeof( cc.sources ), "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }
        // Reflection-generated TU (written by build_reflect.exe in step 5).
        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            snprintf( rel, sizeof( rel ), "%s/%s.generated.c", gen_dir, rname );
            if ( !_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            cc_field( cc.sources, sizeof( cc.sources ), "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }
    }

    // Print the requested sections. Routes to the per-target log when called
    // from a parallel worker so the output lands with child-process output.
    FILE* log_out = cc_open_log();
    cc_print( log_out, &cc, target, config );

    // Assemble the full command, optionally echo the raw line, then run.
    char      rsp_path[ BT_PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s/cl.rsp", obj_dir );
    cc_assemble( &cc, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_COMPILE_CMD ) print_raw_cmd( log_out, cmd.buf );
    if ( log_out != stdout ) fclose( log_out );

    char deps_path[ BT_PATH_MAX ];
    snprintf( deps_path, sizeof( deps_path ), "%s/_deps.txt", obj_dir );
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
    const char*   config  = ( ctx->config == BT_CONFIG_DEBUG ) ? "Debug" : "Release";

    // Exe
    snprintf( cc.exe, sizeof( cc.exe ), "%s", ctx->compiler == BT_COMPILER_CLANG ? "clang-cl.exe" : "cl.exe" );

    // Flags: same as full compile, but no /showIncludes (dep tracking not needed).
    cc_field( cc.flags, sizeof( cc.flags ), "/c /nologo /W4 /WX /Zc:preprocessor /std:c11" );
    if ( ctx->config == BT_CONFIG_DEBUG )
        cc_field( cc.flags, sizeof( cc.flags ), " /Zi /Od /MDd" );
    else
        cc_field( cc.flags, sizeof( cc.flags ), " /O2 /MD" );

    // Includes, defines: identical to build_target_compile().
    cc_field( cc.includes, sizeof( cc.includes ), "/I source /I %s", gen_dir );

    cc_field( cc.defines, sizeof( cc.defines ),
              "/DOS_WINDOWS /DCOMPILER_MSVC /DARCH_X64 /D_CRT_SECURE_NO_WARNINGS" );
    {
        char upper[ 128 ];
        get_target_upper( target->name, upper );
        cc_field( cc.defines, sizeof( cc.defines ), " /D%s_STATIC", upper );
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
            cc_field( cc.defines, sizeof( cc.defines ), " /D%s_STATIC", dep_upper );
        }
    }
    if ( ctx->is_monolithic )
        cc_field( cc.defines, sizeof( cc.defines ), " /DBUILD_STATIC" );
    if ( ctx->config == BT_CONFIG_DEBUG )
        cc_field( cc.defines, sizeof( cc.defines ), " /D_DEBUG" );
    else
        cc_field( cc.defines, sizeof( cc.defines ), " /DNDEBUG" );

    // Warning suppressions: same table, same logic as build_target_compile().
    {
        for ( int i = 0; i < g_warn_suppression_count; ++i )
        {
            warn_suppress_t* s = &g_warn_suppressions[ i ];
            if ( ( s->config == ctx->config || s->config == BT_CONFIG_COUNT ) &&
                 ( s->compiler_mask & (unsigned int)ctx->compiler ) )
                cc_field( cc.flags, sizeof( cc.flags ), " %s", s->flag );
        }
    }

    // Output into the same obj_dir as a full build so the .obj lands where the linker expects it.
    cc_field( cc.output, sizeof( cc.output ), "/Fo%s/ /Fd%s/", obj_dir, obj_dir );

    // Source: just the one file VS handed us.
    cc_field( cc.sources, sizeof( cc.sources ), "%s", file_path );

    // Print active sections; single-file runs are always serial so stdout is fine.
    FILE* log_out = cc_open_log();
    cc_print( log_out, &cc, target, config );

    char      rsp_path[ BT_PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s/cl_file.rsp", obj_dir );
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

    // In monolithic mode, dynamic modules are archived as static libs instead of DLLs.
    // This lets host executables link them directly with no hot-reload overhead.
    target_type_t effective_type = target->type;
    if ( ctx->is_monolithic && effective_type == TARGET_DYNAMIC_LIB )
        effective_type = TARGET_STATIC_LIB;

    if ( effective_type == TARGET_STATIC_LIB )
    {
        snprintf( lk.exe,      sizeof( lk.exe ),      "lib.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin/%s.lib", target->name );
        snprintf( lk.flags,    sizeof( lk.flags ),     "/nologo" );
        snprintf( lk.output,   sizeof( lk.output ),    "/OUT:bin/%s.lib", target->name );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s/*.obj", obj_dir );
        // lib.exe has no PDB and no dep libs — lk.pdb and lk.libs stay empty.
    }
    else
    {
        const char* ext = ( effective_type == TARGET_DYNAMIC_LIB ) ? ".dll" : ".exe";

        snprintf( lk.exe,      sizeof( lk.exe ),      "link.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin/%s%s", target->name, ext );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s/*.obj", obj_dir );

        // Flags: /DLL only for shared libs.
        if ( effective_type == TARGET_DYNAMIC_LIB )
            snprintf( lk.flags, sizeof( lk.flags ), "/nologo /DLL" );
        else
            snprintf( lk.flags, sizeof( lk.flags ), "/nologo" );

        // Output: artifact + optional import lib for DLLs.
        if ( effective_type == TARGET_DYNAMIC_LIB )
            snprintf( lk.output, sizeof( lk.output ),
                      "/OUT:bin/%s.dll /IMPLIB:bin/%s.lib", target->name, target->name );
        else
            snprintf( lk.output, sizeof( lk.output ), "/OUT:bin/%s.exe", target->name );

        // PDB: rotated per-link so a held-open debugger session never blocks
        // the linker. Stale leftovers are garbage-collected before each link.
        cleanup_stale_pdbs( target->name );
        snprintf( lk.pdb, sizeof( lk.pdb ),
                  "/DEBUG /PDB:bin/%s_%lld.pdb", target->name, ( long long )time( NULL ) );

        // Libs: declared dep .libs + Windows system libs every target uses.
        for ( int i = 0; target->deps[ i ]; ++i )
            cc_field( lk.libs, sizeof( lk.libs ), "%sbin/%s.lib",
                      lk.libs[ 0 ] ? " " : "", target->deps[ i ] );
        cc_field( lk.libs, sizeof( lk.libs ), "%suser32.lib shell32.lib gdi32.lib advapi32.lib",
                  lk.libs[ 0 ] ? " " : "" );
    }

    // Print sections, assemble, optionally echo raw command, then run.
    FILE* log_out = cc_open_log();
    lk_print( log_out, &lk, target );

    char      rsp_path[ BT_PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s/%s.rsp", obj_dir,
              target->type == TARGET_STATIC_LIB ? "lib" : "link" );
    lk_assemble( &lk, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_LINK_CMD ) print_raw_cmd( log_out, cmd.buf );
    if ( log_out != stdout ) fclose( log_out );

    // NULL deps_path: no /showIncludes parsing needed for link/lib, but we still
    // want line-by-line capture so [MSVC] prefixing and ORB_OUT_MSVC_OUTPUT gating apply.
    return build_run_cmd_capture_deps( cmd.buf, NULL ) == 0;
}

/*============================================================================================*/
