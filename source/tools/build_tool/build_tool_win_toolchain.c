/*==============================================================================================

    build_tool_win_toolchain.c -- MSVC / clang-cl compiler and linker platform layer.

    Provides all platform_cc_* and platform_lk_* functions for the MSVC toolchain.
    A future build_tool_posix_toolchain.c would implement the same symbols for GCC/Clang:

        platform_cc_exe()            -- "gcc" / "clang"
        platform_cc_base_flags()     -- "-c -Wall -Wextra -Werror -std=c11" + "-g -O0" / "-O2"
        platform_cc_append_include() -- "-I <path>"
        platform_cc_append_define()  -- "-D<name>"
        platform_cc_output_flags()   -- "-o <obj_dir>/<unit_stem>.o"  (per-file, not per-dir)
        platform_cc_dep_flag()       -- "-MMD -MF <depfile>"  (post-compile .d file, not inline stream)

        platform_lk_fill_static()    -- "ar rcs bin/lib<name>.a ..."
        platform_lk_fill_dynamic()   -- "gcc -shared -o bin/lib<name>.so ..." /
                                        "clang -dynamiclib -o bin/lib<name>.dylib ..."
        platform_lk_obj_pattern()    -- "<obj_dir>/*.o"
        platform_lk_append_dep_lib() -- "bin/<name>.so" or "-l<name>"
        platform_lk_append_sys_libs()-- (empty on POSIX for these abstractions)
        platform_lk_pre_link()       -- no-op on POSIX

    Compiler functions:
        platform_cc_exe()               -- compiler executable name
        platform_cc_base_flags()        -- core compile flags (standard, warnings, config)
        platform_cc_append_include()    -- append "/I <path>" to an includes field
        platform_cc_append_define()     -- append "/D<name>" to a defines field
        platform_cc_output_flags()      -- output-dir flags (/Fo /Fd)
        platform_cc_dep_flag()          -- dependency-tracking flag string (/showIncludes)

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
platform_cc_base_flags( config_t config, char* buf, size_t size )
{
    size_t used = strlen( buf );
    const char* sep = used ? " " : "";
    const char* cfg = ( config == CONFIG_DEBUG ) ? "/Zi /Od /MDd" : "/O2 /MD";
    snprintf( buf + used, size - used,
              "%s/c /nologo /W4 /WX /Zc:preprocessor /std:c11 %s", sep, cfg );
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
    --- Compiler: Batch Compilation Flag ---

    Win32: cl.exe accepts multiple source files in one invocation and writes all
    .obj files into the /Fo<dir>/ directory -- platform_cc_per_unit returns false.
    POSIX: GCC/Clang require a separate -o <dir>/<stem>.o per source file.
==============================================================================================*/

static bool
platform_cc_per_unit( void )
{
    return false;
}

/*==============================================================================================
    --- Compiler: Dependency Tracking Flag ---

    MSVC: /showIncludes emits "Note: including file: <path>" on stdout for every
    header visited. build_run_cmd_capture_includes() streams and parses this output
    into _includes.txt for the incremental header-change check.

    GCC/Clang: -MMD writes a Makefile .d file after compilation.
    The posix toolchain returns "-MMD"; build_collect_dep_files() in 05_spawn.c
    reads those .d files into _includes.txt after the per-unit compile loop.
==============================================================================================*/

static const char*
platform_cc_dep_flag( void )
{
    return "/showIncludes";
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
    produce a .lib that dependents link against -- the name is the same in both cases.
    On POSIX there is no import lib; dependents link directly against .so / .dylib.
==============================================================================================*/

static void
platform_lk_append_dep_lib( const char* dep_name, char* buf, size_t size )
{
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
platform_lk_pre_link( const char* target_name )
{
    char pattern[ PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "bin\\%s_*.pdb", target_name );

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
    --- Linker: Fill Static Lib Command ---

    lib.exe archives .obj files into a flat .lib. No PDB, no dep resolution needed;
    lk->pdb and lk->libs are left empty.
==============================================================================================*/

static void
platform_lk_fill_static( const char* target_name, link_cmd_t* lk )
{
    snprintf( lk->exe,      sizeof( lk->exe ),      "lib.exe" );
    snprintf( lk->artifact, sizeof( lk->artifact ),  "bin\\%s.lib", target_name );
    snprintf( lk->flags,    sizeof( lk->flags ),     "/nologo" );
    snprintf( lk->output,   sizeof( lk->output ),    "/OUT:bin\\%s.lib", target_name );
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
    ( void )ctx;
    const bool  is_dll = ( target->type == TARGET_DYNAMIC_LIB );
    const char* ext    = is_dll ? ".dll" : ".exe";

    snprintf( lk->exe,      sizeof( lk->exe ),      "link.exe" );
    snprintf( lk->artifact, sizeof( lk->artifact ), "bin\\%s%s", target->name, ext );
    snprintf( lk->flags,    sizeof( lk->flags ),    "/nologo%s", is_dll ? " /DLL" : "" );

    if ( is_dll )
        snprintf( lk->output, sizeof( lk->output ),
                  "/OUT:bin/%s.dll /IMPLIB:bin/%s.lib", target->name, target->name );
    else
        snprintf( lk->output, sizeof( lk->output ), "/OUT:bin\\%s.exe", target->name );

    snprintf( lk->pdb, sizeof( lk->pdb ),
              "/DEBUG /PDB:bin/%s_%lld.pdb", target->name, ( long long )time( NULL ) );
}

// clang-format on
/*============================================================================================*/
