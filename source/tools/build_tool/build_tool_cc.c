/*==============================================================================================

    build_tool_cc.c -- Compiler and linker command generation.

    Builds the cl.exe / link.exe / lib.exe command lines for a single target
    using cmd_buf_t (see build_tool_utils.c) and dispatches them through
    build_run_cmd / build_run_cmd_capture_deps (see build_tool_vcvars.c).

    Two public entry points, both called from build_target():
      build_target_compile() -- cl.exe with /showIncludes for dep capture.
      build_target_link()    -- lib.exe for static libs, link.exe otherwise.

    Flag philosophy:
      - The same defines we emit here must match the per-config IntelliSense
        defines emitted by build_tool_gen.c. Drift between the two surfaces
        as IDE squigglies that don't reproduce at build time, which is
        exactly the kind of bug that wastes hours to chase.
      - Response-file spill is wired in unconditionally at the end of each
        path so any future target growth past the cmd.exe arg limit is
        handled transparently.

==============================================================================================*/
#include "build_tool.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <io.h>
#include <time.h>

/*============================================================================================*/
// --- Internal Helpers ---

/**
 * get_target_upper()
 *
 * Copy `name` to `out`, ASCII-uppercasing as we go. Used to derive the
 * <TARGET>_STATIC preprocessor symbol from a target's lower-case registry
 * name. The same helper is reached from build_tool_gen.c (unity build)
 * so IntelliSense and compile-time defines emit the exact same identifier.
 *
 * Caller must size `out` >= strlen(name)+1.
 */
static void
get_target_upper( const char* name, char* out )
{
    strcpy( out, name );
    for ( char* p = out; *p; ++p ) *p = ( char )toupper( *p );
}

/**
 * cleanup_stale_pdbs()
 *
 * Garbage-collect PDB files left over from previous links. Each link
 * writes bin/<name>_<unix_timestamp>.pdb (see build_target_link below);
 * over time that accumulates one file per rebuild. This function sweeps
 * bin/<name>_*.pdb and removes whatever it can.
 *
 * Files held open by an attached debugger fail `remove()` with sharing-
 * violation, which is silently ignored — exactly the behavior we want:
 * the live debug session keeps the PDB it needs, and only the unlocked
 * stale leftovers are cleaned up. The new link will write a fresh,
 * uniquely-named PDB regardless, so there is never a collision.
 */
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

/*============================================================================================*/
// --- Compilation ---

/**
 * build_target_compile()
 *
 * Assemble and run the cl.exe (or clang-cl.exe) command line for one
 * target. Returns true iff the compiler exited with 0.
 *
 * The function builds up the command incrementally via cmd_append so the
 * truncation logic in cmd_buf_t can flag overflow and the response-file
 * spill at the end can kick in for very large targets.
 */
bool
build_target_compile( build_context_t* ctx, target_info_t* target, const char* obj_dir, const char* gen_dir )
{
    cmd_buf_t cmd = { 0 };
    const char* cc = ctx->is_clang ? "clang-cl.exe" : "cl.exe";

    // Base flags. /showIncludes emits a "Note: including file:" line for every
    // header pulled in, which build_run_cmd_capture_deps strips out into a
    // per-target dep file so the incremental rebuild logic in build_target's
    // step-2 check can detect header edits. Essential for unity builds where
    // the listed .units are just umbrella TUs and the real source surface is
    // hidden behind their #includes.
    cmd_append( &cmd, "%s /c /nologo /W4 /WX /Zc:preprocessor /std:c11 /showIncludes ", cc );

    // Include paths and output directories. /Fo = object output dir, /Fd =
    // PDB output dir. Trailing slash matters — without it cl treats the path
    // as a filename prefix instead of a directory.
    cmd_append( &cmd, "/I source /I %s /Fo%s/ /Fd%s/ ", gen_dir, obj_dir, obj_dir );

    // Architectural defines that every TU sees. Must stay in lockstep with
    // the IntelliSense defines emitted by build_tool_gen.c.
    cmd_append( &cmd, "/DOS_WINDOWS /DCOMPILER_MSVC /DARCH_X64 /D_CRT_SECURE_NO_WARNINGS " );

    // Target-specific static define (e.g. /DBASE_STATIC). Allows public APIs
    // declared in this target to flip between dllexport/dllimport depending
    // on whether the consumer is the same translation unit or a sibling DLL.
    char target_upper[ 128 ];
    get_target_upper( target->name, target_upper );
    cmd_append( &cmd, "/D%s_STATIC ", target_upper );

    // BUILD_STATIC fully disables hot-reload — the whole engine links into
    // one image. Only set when an external caller chose monolithic mode.
    if ( ctx->is_monolithic )
        cmd_append( &cmd, "/DBUILD_STATIC " );

    // Config-specific flags. Debug = full symbols, no optimization, debug
    // CRT. Release = /O2 and the retail CRT. _DEBUG / NDEBUG are the
    // canonical guards downstream code reads to gate assertions etc.
    if ( ctx->config == CONFIG_DEBUG )
        cmd_append( &cmd, "/Zi /Od /MDd /D_DEBUG " );
    else
        cmd_append( &cmd, "/O2 /MD /DNDEBUG " );

    // Add the explicit translation units listed in target_info_t.units[].
    for ( int i = 0; target->units[ i ]; ++i )
        cmd_append( &cmd, "%s/%s ", target->root_dir, target->units[ i ] );

    // Add the reflection-generated translation unit (step 5 of build_target
    // wrote it under <gen_dir>/<rname>.generated.c).
    if ( target->has_reflect )
    {
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        cmd_append( &cmd, "%s/%s.generated.c ", gen_dir, rname );
    }

    // Spill to a cl.rsp response file if the assembled command crossed the
    // shell threshold (or got flagged truncated). The buffer is rewritten
    // to "cl.exe @<path>" form in-place; cl.exe reads the file like a
    // continuation of its argv.
    char rsp_path[ BT_PATH_MAX ];
    snprintf( rsp_path, sizeof( rsp_path ), "%s/cl.rsp", obj_dir );
    cmd_spill_to_response_file( &cmd, rsp_path );

    // Write the captured header list to <obj_dir>/_deps.txt for the next
    // incremental check to read. Coarse-grained (one file per target) which
    // matches the unity-build pattern: any header change forces a target
    // recompile, but headers in OTHER targets don't trigger spurious rebuilds.
    char deps_path[ BT_PATH_MAX ];
    snprintf( deps_path, sizeof( deps_path ), "%s/_deps.txt", obj_dir );

    return build_run_cmd_capture_deps( cmd.buf, deps_path ) == 0;
}

/*============================================================================================*/
// --- Linking / Archiving ---

/**
 * build_target_link()
 *
 * Assemble and run the final-stage command for one target:
 *  - TARGET_STATIC_LIB  → lib.exe (archive .obj files into a .lib).
 *  - TARGET_DYNAMIC_LIB → link.exe /DLL with /IMPLIB so consumers can
 *                         link against the import lib at compile time.
 *  - TARGET_EXECUTABLE  → link.exe straight, producing a .exe.
 *
 * The DLL and EXE paths share a single linker invocation because the only
 * differences (the /DLL switch and the /IMPLIB output) are additive.
 * Returns true iff the tool exited with 0.
 */
bool
build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir )
{
    ( void )ctx;
    cmd_buf_t cmd = { 0 };

    if ( target->type == TARGET_STATIC_LIB )
    {
        // lib.exe simply globs every .obj in the target's obj dir into the
        // archive — no link-time codegen, no per-symbol dep resolution.
        cmd_append( &cmd, "lib.exe /nologo /OUT:bin/%s.lib %s/*.obj", target->name, obj_dir );

        // Static libs can grow large too (many TUs over time), so spill if needed.
        char rsp_path[ BT_PATH_MAX ];
        snprintf( rsp_path, sizeof( rsp_path ), "%s/lib.rsp", obj_dir );
        cmd_spill_to_response_file( &cmd, rsp_path );
    }
    else
    {
        // Shared start of the link.exe command for both DLL and EXE.
        const char* linker = "link.exe";
        cmd_append( &cmd, "%s /nologo ", linker );

        // DLL extras: tell the linker to produce a side-by-side .lib so other
        // targets can /LINK against it without needing the .dll at compile time.
        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            cmd_append( &cmd, "/DLL " );
            cmd_append( &cmd, "/IMPLIB:bin/%s.lib ", target->name );
        }

        // Pick the artifact extension based on target type. Both consume the
        // same `*.obj` glob in the target's obj dir.
        cmd_append( &cmd, "/OUT:bin/%s%s %s/*.obj ", target->name,
                    (target->type == TARGET_EXECUTABLE) ? ".exe" : ".dll", obj_dir );

        // PDB rotation: each link writes a uniquely-named PDB so an attached
        // debugger can never collide with the linker over the previous
        // build's symbol file. cleanup_stale_pdbs() above garbage-collects
        // the leftover files that aren't currently locked by a debugger.
        cleanup_stale_pdbs( target->name );
        cmd_append( &cmd, "/DEBUG /PDB:bin/%s_%lld.pdb ",
                    target->name, ( long long )time( NULL ) );

        // Link against the target's declared dep .libs. The scheduler / dep
        // resolver guarantees these exist by the time we get here.
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            cmd_append( &cmd, "bin/%s.lib ", target->deps[ i ] );
        }

        // Windows system libs every binary in the project happens to want.
        // Hard-coded here because every target so far uses at least one of
        // these — keeping them per-target metadata would be ceremony with
        // zero benefit.
        cmd_append( &cmd, "user32.lib shell32.lib gdi32.lib advapi32.lib " );

        char rsp_path[ BT_PATH_MAX ];
        snprintf( rsp_path, sizeof( rsp_path ), "%s/link.rsp", obj_dir );
        cmd_spill_to_response_file( &cmd, rsp_path );
    }

    return build_run_cmd( cmd.buf ) == 0;
}

/*============================================================================================*/