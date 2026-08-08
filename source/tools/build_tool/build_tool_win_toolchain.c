#if defined( _WIN32 )
/*==============================================================================================

    build_tool_win_toolchain.c -- MSVC / clang-cl compiler and linker platform layer.

    Provides all platform_cc_* and platform_lk_* functions for the MSVC toolchain.
    A future build_tool_posix_toolchain.c would implement the same symbols for GCC/Clang:

        platform_cc_exe()            -- "gcc" / "clang"
        platform_cc_base_flags()     -- "-c -Wall -Wextra -Werror -std=c11" + "-g -O0" / "-O2"
        platform_cc_append_include() -- "-I <path>"
        platform_cc_append_define()  -- "-D<name>"
        platform_cc_output_flags()   -- "-o <obj_dir>/<unit_stem>.o"  (per-file, not per-dir)
        platform_cc_dep_flag()       -- "-MMD"  (.d file next to the object, not a dep dir)
        platform_cc_mp_flag()        -- (empty on POSIX; per-unit compiles are not batched)

        platform_lk_fill_static()    -- "ar rcs bin/lib<name>.a ..."
        platform_lk_fill_dynamic()   -- "gcc -shared -o bin/lib<name>.so ..." /
                                        "clang -dynamiclib -o bin/lib<name>.dylib ..."
        platform_lk_obj_pattern()    -- "<obj_dir>/ *.o" (glob pattern)
        platform_lk_append_dep_lib() -- "bin/<name>.so" or "-l<name>"
        platform_lk_append_sys_libs()-- (empty on POSIX for these abstractions)
        platform_lk_pre_link()       -- no-op on POSIX

    Compiler functions:
        platform_cc_exe()               -- compiler executable name
        platform_cc_base_flags()        -- core compile flags (standard, warnings, config)
        platform_cc_append_include()    -- append "/I <path>" to an includes field
        platform_cc_append_define()     -- append "/D<name>" to a defines field
        platform_cc_output_flags()      -- output-dir flags (/Fo /Fd)
        platform_cc_dep_flag()          -- dependency-tracking flags (/sourceDependencies <dir>/)
        platform_cc_mp_flag()           -- multi-process batch flag (/MP<n>), width-gated

    Linker / archiver functions:
        platform_lk_fill_static()       -- fill link_cmd_t for a static lib (lib.exe)
        platform_lk_fill_dynamic()      -- fill link_cmd_t for a DLL or executable (link.exe)
        platform_lk_obj_pattern()       -- object-file glob for linker inputs
        platform_lk_append_dep_lib()    -- append a dependency .lib path to libs field
        platform_lk_append_sys_libs()   -- append Windows system .libs to libs field
        platform_lk_pre_link()          -- remove stale PDB files before each link

==============================================================================================*/
// clang-format off

#if !defined( _WIN32 )
    #error "build_tool_win_toolchain.c is only for Windows / MSVC builds"
#endif

#include <time.h>

/*==============================================================================================
    --- Compiler: Executable ---
==============================================================================================*/

static const char*
platform_cc_exe( compiler_t compiler )
{
    return compiler == COMPILE_CLANG ? "clang-cl.exe" : "cl.exe";
}

/*==============================================================================================
    --- Compiler: Base Flags ---

    Appends the core compile flags to buf. Config selects Debug or Release variants.
    /showIncludes and output flags are NOT included here -- callers append them separately
    so each entry point can opt in to include tracking and control output placement.
==============================================================================================*/

static void
platform_cc_base_flags( compiler_t compiler, config_t config, bool is_shipping, char* buf, size_t size )
{
    size_t used = strlen( buf );
    const char* sep = used ? " " : "";
    const char* cfg = ( config == CONFIG_DEBUG ) ? "/Zi /Od /MDd"
                    : is_shipping               ? "/O2 /GL /MD"
                                               : "/O2 /MD";
    // /Zc:preprocessor is MSVC-only; clang-cl defaults to conforming preprocessor already.
    // --target is required for standalone LLVM clang-cl: without it the default triple may
    // not define _M_X64 / __x86_64__, causing the architecture detection in orb.h to fail.
    const char* zc     = ( compiler == COMPILE_CLANG ) ? "" : " /Zc:preprocessor";
    const char* target = ( compiler == COMPILE_CLANG ) ? " --target=x86_64-pc-windows-msvc" : "";
    snprintf( buf + used, size - used,
              "%s/c /nologo /W4 /WX%s%s /std:c11 %s", sep, zc, target, cfg );
}

/*==============================================================================================
    --- Compiler: Include and Define Appenders ---

    Each function appends one entry to its respective field, prepending a space
    when the field is already non-empty so the caller does not have to track separators.
==============================================================================================*/

static void
platform_cc_append_include( const char* path, char* buf, size_t size )
{
    size_t used = strlen( buf );
    snprintf( buf + used, size - used, "%s/I %s", used ? " " : "", path );
}

static void
platform_cc_append_define( const char* name, char* buf, size_t size )
{
    size_t used = strlen( buf );
    snprintf( buf + used, size - used, "%s/D%s", used ? " " : "", name );
}

/*==============================================================================================
    --- Compiler: Output Flags ---

    /Fo<dir>/ directs all .obj files into obj_dir (trailing slash required; without it
    cl.exe treats the path as a filename prefix, not a directory).
    /Fd<dir>/ co-locates the per-target compiler PDB with the .obj files.
    unit_path is unused on Win32 -- cl.exe resolves per-file names inside the directory.
==============================================================================================*/

static void
platform_cc_output_flags( const char* obj_dir, const char* unit_path, char* buf, size_t size )
{
    ( void )unit_path;  // Win32 uses a directory target; the unit path is not needed.
    size_t used = strlen( buf );
    snprintf( buf + used, size - used,
              "%s/Fo%s/ /Fd%s/", used ? " " : "", obj_dir, obj_dir );
}

/*==============================================================================================
    --- Compiler: Batch vs Per-Unit Compilation Model ---

    Win32: cl.exe accepts multiple source files in one invocation and writes all
    .obj files into the /Fo<dir>/ directory -- platform_cc_per_unit returns false.
    POSIX: GCC/Clang require a separate -o <dir>/<stem>.o per source file and return true.

    Both models write per-unit dependency files that platform_cc_collect_dep_files reads
    into _includes.txt after the compile: MSVC emits <unit>.c.json (/sourceDependencies),
    GCC/Clang emit <unit>.d (-MMD).
==============================================================================================*/

static bool
platform_cc_per_unit( void )
{
    return false;
}

/*  Delegates to win_collect_dep_files() in build_tool_win_spawn.c. Never called for
    clang-cl -- that path streams its includes inline and 06_spawn.c has already
    written the file by the time the compile returns. */

static void
platform_cc_collect_dep_files( const char* obj_dir, const char* includes_path )
{
    win_collect_dep_files( obj_dir, includes_path );
}

/*==============================================================================================
    --- Compiler: Dependency Tracking Flag ---

    MSVC: /sourceDependencies <dir>/ writes one <unit>.c.json per translation unit listing
    every header it visited. win_parse_dep_json() in build_tool_win_spawn.c flattens those
    into _includes.txt for the incremental header-change check.

    The older /showIncludes streamed the same information inline on stdout, but cl.exe
    refuses to combine it with multi-process compilation:

        cl : Command line warning D9030 : '/showIncludes' is incompatible with
             multiprocessing; ignoring /MP switch

    That warning does not fail the build -- it silently drops /MP -- so moving to
    /sourceDependencies is what makes platform_cc_mp_flag() below actually take effect.

    clang-cl: stays on /showIncludes. It parses /sourceDependencies but does nothing with
    it ("argument unused during compilation") and writes no .json, which would leave
    _includes.txt empty and silently disable header tracking. Its -MD equivalent writes
    dep files into the working directory rather than /Fo, which would litter the repo
    root. Nothing is lost: clang-cl ignores /MP too, so there is no batch split to protect
    and the inline stream costs nothing extra.
==============================================================================================*/

static bool
platform_cc_dep_is_inline( compiler_t compiler )
{
    return compiler == COMPILE_CLANG;
}

static void
platform_cc_dep_flag( compiler_t compiler, const char* obj_dir, char* buf, size_t size )
{
    if ( platform_cc_dep_is_inline( compiler ) )
    {
        snprintf( buf, size, "/showIncludes" );
        return;
    }

    // Trailing slash marks the argument as a directory; without it cl.exe treats
    // the path as a single output filename and every unit overwrites the last.
    snprintf( buf, size, "/sourceDependencies %s/", obj_dir );
}

/*==============================================================================================
    --- Compiler: Multi-Process Compilation ---

    /MP<n> splits one batch compile across n cl.exe child processes. It only pays on a
    target wide enough to fill them -- below MP_MIN_UNITS the spawn cost dominates, and
    most targets in the tree are a single unity unit where /MP has nothing to divide.

    The share is scaled against the scheduler's worker count. Those workers are already
    building independent targets in parallel, so the box is shared: n = cpu / workers is
    each worker's slice of it. When that slice comes out below 2 the scheduler is already
    saturating every core and /MP is left off entirely rather than oversubscribing.

    That makes the single-target case the big winner, which is the point -- `-target gui`
    and build_hot.bat run one job while every other core sits idle. Measured end to end on
    gui (15 units, Debug, 32 logical CPUs): 0.95s -> 0.37s.

    clang-cl accepts /MP and ignores it, so the flag is left off there rather than emitting
    an argument the compiler will only warn about.
==============================================================================================*/

#define MP_MIN_UNITS   4    // Fewest units in a batch that justify multi-process compilation.
#define MP_MAX_SHARE   8    // Past this the batch is I/O bound and extra children add nothing.

static void
platform_cc_mp_flag( compiler_t compiler, int unit_count, char* buf, size_t size )
{
    buf[ 0 ] = '\0';
    if ( compiler == COMPILE_CLANG ) return;
    if ( unit_count < MP_MIN_UNITS ) return;

    int workers = ( g_job_threads > 0 ) ? g_job_threads : 1;
    int share   = platform_cpu_count() / workers;

    if ( share < 2 ) return;   // scheduler already owns every core; do not oversubscribe
    if ( share > MP_MAX_SHARE ) share = MP_MAX_SHARE;
    if ( share > unit_count   ) share = unit_count;

    snprintf( buf, size, "/MP%d", share );
}

/*==============================================================================================
    --- Linker: Object File Pattern ---

    Returns the glob pattern used to reference all compiled objects for a target.
    MSVC uses *.obj; GCC/Clang use *.o.
==============================================================================================*/

static void
platform_lk_obj_pattern( const char* obj_dir, char* buf, size_t size )
{
    snprintf( buf, size, "%s\\*.obj", obj_dir );
}

/*==============================================================================================
    --- Linker: Dependency Library ---

    Appends the import lib path for a declared dependency. Both static libs and DLLs
    produce a .lib that dependents link against -- the extension is always .lib on Win32.
    dep_type is accepted for API symmetry with the POSIX counterpart (which branches on
    it to choose .a vs .so) but is unused here.
==============================================================================================*/

static void
platform_lk_append_dep_lib( const char* dep_name, target_type_t dep_type, char* buf, size_t size )
{
    ( void )dep_type;  // Win32: both static and DLL import libs use .lib.
    size_t used = strlen( buf );
    snprintf( buf + used, size - used, "%sbin/%s.lib", used ? " " : "", dep_name );
}

/*==============================================================================================
    --- Linker: System Libraries ---

    Appends the four Windows import libraries needed by virtually every ORB target.
    On POSIX these are provided by the C runtime and libc -- no explicit link needed.
==============================================================================================*/

static void
platform_lk_append_sys_libs( char* buf, size_t size )
{
    size_t used = strlen( buf );
    snprintf( buf + used, size - used,
              "%suser32.lib shell32.lib gdi32.lib advapi32.lib", used ? " " : "" );
}

/*==============================================================================================
    --- Linker: Pre-Link Cleanup ---

    Removes stale per-target PDB files before each link. Each link produces a
    uniquely-timestamped bin/<name>_<ts>.pdb so an attached debugger holding the
    previous file never blocks the linker. Unlocked leftovers are swept here.
    remove() silently fails for any PDB still held open -- the correct behavior.

    On POSIX this is a no-op: debug info is embedded in DWARF within the binary.
==============================================================================================*/

static void
platform_lk_pre_link( const char* target_name, config_t config )
{
    // Sweep only THIS config's stale PDBs. The other config's PDB may be held
    // open by an attached debugger -- sweeping across configs would silently
    // fail on that file and leave both around, confusing VS symbol loading.
    const char* cfg_str = ( config == CONFIG_DEBUG ) ? "debug" : "release";
    char pattern[ PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "bin\\%s_%s_*.pdb", target_name, cfg_str );

    platform_find_data_t fd;
    platform_find_t h = platform_find_first( pattern, &fd );
    if ( h == PLATFORM_FIND_INVALID ) return;

    do
    {
        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "bin\\%s", fd.name );
        remove( path );
    }
    while ( platform_find_next( h, &fd ) );
    platform_find_close( h );
}

/*==============================================================================================
    --- Toolchain: Resource Compiler and Manifest Embed ---

    platform_compile_rc() and platform_embed_manifest() are defined in
    build_tool_09_exec.c (after 06_spawn.c in the unity include chain) because
    they call build_run_cmd(), which is not yet in scope at this include point.

    See build_tool_09_exec.c for the implementations.

==============================================================================================*/

/*==============================================================================================
    --- Linker: Fill Static Lib Command ---

    lib.exe archives .obj files into a flat .lib. No PDB, no dep resolution needed;
    lk->pdb and lk->libs are left empty.
==============================================================================================*/

static void
platform_lk_fill_static( const char* target_name, bool is_shipping, link_cmd_t* lk )
{
    const char* ltcg = is_shipping ? " /LTCG" : "";
    snprintf( lk->exe,      sizeof( lk->exe ),      "lib.exe" );
    snprintf( lk->artifact, sizeof( lk->artifact ), "bin\\%s.lib", target_name );
    snprintf( lk->flags,    sizeof( lk->flags ),    "/nologo%s", ltcg );
    snprintf( lk->output,   sizeof( lk->output ),   "/OUT:bin\\%s.lib", target_name );
}

/*==============================================================================================
    --- Linker: Fill Dynamic Lib / Executable Command ---

    Fills exe, artifact, flags, output, and pdb for link.exe.
    lk->inputs (obj glob) and lk->libs (dep + system libs) are filled by the caller.

    DLLs also emit an import lib via /IMPLIB so dependents can link against them.
    Each link writes a uniquely-timestamped PDB -- see platform_lk_pre_link().
==============================================================================================*/

static void
platform_lk_fill_dynamic( build_context_t* ctx, target_info_t* target, link_cmd_t* lk )
{
    const bool  is_dll = ( target->type == TARGET_DYNAMIC_LIB );
    const char* ext    = is_dll ? ".dll" : ".exe";
    const char* ltcg   = ctx->is_shipping ? " /LTCG" : "";

    snprintf( lk->exe,      sizeof( lk->exe ),      "link.exe" );
    snprintf( lk->artifact, sizeof( lk->artifact ), "bin\\%s%s", target->name, ext );
    snprintf( lk->flags,    sizeof( lk->flags ),    "/nologo%s%s", is_dll ? " /DLL" : "", ltcg );

    // Emit /SUBSYSTEM for executables. Always explicit so builds are deterministic
    // regardless of the linker's built-in default. DLLs carry no subsystem.
    if ( !is_dll )
    {
        const char* subsys = ( target->subsystem == SUBSYSTEM_WINDOWS ) ? "WINDOWS" : "CONSOLE";
        size_t used = strlen( lk->flags );
        snprintf( lk->flags + used, sizeof( lk->flags ) - used, " /SUBSYSTEM:%s", subsys );
    }

    if ( is_dll )
        snprintf( lk->output, sizeof( lk->output ),
                  "/OUT:bin\\%s.dll /IMPLIB:bin\\%s.lib", target->name, target->name );
    else
        snprintf( lk->output, sizeof( lk->output ), "/OUT:bin\\%s.exe", target->name );

    const char* cfg_str = ( ctx->config == CONFIG_DEBUG ) ? "debug" : "release";
    snprintf( lk->pdb, sizeof( lk->pdb ),
              "/DEBUG /PDB:bin/%s_%s_%lld.pdb", target->name, cfg_str, ( long long )time( NULL ) );

#if defined( BUILD_TOOL_EMBED_MANIFEST )
    // For the build_tool exe only: embed the app manifest via the linker so no
    // post-link mt.exe call is required (mt.exe fails on UNC-style long paths).
    if ( !is_dll && target->is_build_tool )
    {
        size_t used = strlen( lk->flags );
        snprintf( lk->flags + used, sizeof( lk->flags ) - used,
                  " /MANIFEST:EMBED /MANIFESTINPUT:source\\tools\\build_tool\\build_tool.manifest" );
    }
#endif
}

// clang-format on
/*============================================================================================*/
#endif
