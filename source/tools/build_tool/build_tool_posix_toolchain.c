#if !defined( _WIN32 )
/*==============================================================================================

    build_tool_posix_toolchain.c -- GCC / Clang compiler and linker platform layer.

    Provides all platform_cc_* and platform_lk_* functions for GCC and Clang.
    The Win32 counterpart is build_tool_win_toolchain.c; both files expose
    identical symbols so 07_compile.c and 08_link.c branch on none of them.

    Compiler functions:
        platform_cc_exe()               -- compiler executable name
        platform_cc_base_flags()        -- core compile flags (standard, warnings, config)
        platform_cc_append_include()    -- append "-I <path>" to an includes field
        platform_cc_append_define()     -- append "-D<name>" to a defines field
        platform_cc_output_flags()      -- output directory flags (see note below)
        platform_cc_dep_flag()          -- dependency-tracking flag string
        platform_cc_mp_flag()           -- multi-process batch flag (no-op; see note below)

    Linker / archiver functions:
        platform_lk_fill_static()       -- fill link_cmd_t for a static lib (ar rcs)
        platform_lk_fill_dynamic()      -- fill link_cmd_t for a shared lib or executable
        platform_lk_obj_pattern()       -- object-file glob for linker inputs
        platform_lk_append_dep_lib()    -- append a dependency .a/.so path to libs field
        platform_lk_append_sys_libs()   -- append POSIX system libs to libs field
        platform_lk_pre_link()          -- no-op: DWARF debug info is embedded in the binary

    Notes on known POSIX / Win32 asymmetries:

    Output flags (platform_cc_output_flags):
        MSVC /Fo<dir>/ directs all .obj files from a single cl.exe invocation into
        a directory, allowing multiple sources per cl.exe call. GCC / Clang require
        a per-file -o <dir>/<stem>.o flag -- there is no directory-target equivalent.
        platform_cc_output_flags() therefore builds that flag from the unit path, and
        platform_cc_per_unit() returns true so 07_compile.c drives one compiler call
        per source file.

    Dependency tracking (platform_cc_dep_flag):
        Both toolchains write per-unit dependency files that are collected after the
        compile: MSVC emits <unit>.c.json (/sourceDependencies <dir>/), GCC / Clang emit
        <unit>.d (-MMD). Only the file format differs -- the parsers converge on the same
        flat _includes.txt that the incremental header check in 09_exec.c replays.

    Multi-process compilation (platform_cc_mp_flag):
        MSVC batches every source into one cl.exe call, so /MP<n> is what splits that
        batch across child compilers. GCC / Clang have no batch to split -- POSIX already
        compiles per-unit -- so the hook returns empty here. Driving those per-unit calls
        concurrently is a job for the scheduler in 10_sched.c, not a compiler flag.

==============================================================================================*/
// clang-format off

#if defined( _WIN32 )
    #error "build_tool_posix_toolchain.c is only for POSIX builds"
#endif

#include <string.h>
#include <stdio.h>

/*==============================================================================================
    --- Compiler: Executable ---
==============================================================================================*/

static const char*
platform_cc_exe( compiler_t compiler )
{
    return compiler == COMPILE_CLANG ? "clang" : "gcc";
}

/*==============================================================================================
    --- Compiler: Base Flags ---

    Appends the core compile flags to buf. -fPIC is always included so objects
    can go into shared libraries without recompilation. Config selects Debug or
    Release variants. Output flags and dependency tracking are NOT included here.

    is_shipping adds -flto, the GCC/Clang counterpart of MSVC /GL: it defers code
    generation to link time so the linker can optimize across translation units.
    The archive and link steps pass their own -flto to match.
==============================================================================================*/

static void
platform_cc_base_flags( compiler_t compiler, config_t config, bool is_shipping, char* buf, size_t size )
{
    ( void )compiler;
    const char* cfg = ( config == CONFIG_DEBUG ) ? "-g -O0"
                    : is_shipping               ? "-O2 -flto"
                                                : "-O2";
    str_append_tok( buf, size, "-c -Wall -Wextra -Werror -std=c11 -fPIC %s", cfg );
}

/*==============================================================================================
    --- Compiler: Include and Define Appenders ---

    Each function appends one entry to its respective field, prepending a space
    when the field is already non-empty so the caller does not have to track separators.
==============================================================================================*/

static void
platform_cc_append_include( const char* path, char* buf, size_t size )
{
    str_append_tok( buf, size, "-I %s", path );
}

static void
platform_cc_append_define( const char* name, char* buf, size_t size )
{
    str_append_tok( buf, size, "-D%s", name );
}

/*==============================================================================================
    --- Compiler: Output Flags ---

    GCC/Clang require a per-file -o <dir>/<stem>.o flag.
    unit_path is used to extract the filename stem (e.g. "core.c" -> "core.o").
    Called once per source file from the per-unit compile loop in 07_compile.c.
==============================================================================================*/

static void
platform_cc_output_flags( const char* obj_dir, const char* unit_path, char* buf, size_t size )
{
    if ( !unit_path || !unit_path[ 0 ] ) return;

    // Extract the filename from the path, then strip the last extension.
    const char* fname = strrchr( unit_path, '/' );
    if ( !fname ) fname = strrchr( unit_path, '\\' );
    fname = fname ? fname + 1 : unit_path;

    char stem[ 256 ];
    snprintf( stem, sizeof( stem ), "%s", fname );
    char* dot = strrchr( stem, '.' );
    if ( dot ) *dot = '\0';

    str_append_tok( buf, size, "-o %s/%s.o", obj_dir, stem );
}

/*==============================================================================================
    --- Compiler: Batch Compilation Flag ---

    POSIX: GCC/Clang require a separate -o <dir>/<stem>.o per source file --
    platform_cc_per_unit returns true so 07_compile.c runs the per-unit loop.
    Win32: cl.exe accepts all sources in one invocation via /Fo<dir>/.
==============================================================================================*/

static bool
platform_cc_per_unit( void )
{
    return true;
}

/* Delegates to posix_collect_dep_files() in build_tool_posix_spawn.c. */

static void
platform_cc_collect_dep_files( const char* obj_dir, const char* includes_path )
{
    posix_collect_dep_files( obj_dir, includes_path );
}

/*==============================================================================================
    --- Compiler: Dependency Tracking Flag ---

    GCC/Clang: -MMD writes a Makefile .d file alongside each .o after compilation.
    posix_collect_dep_files() in build_tool_posix_spawn.c reads those .d files into
    _includes.txt after the per-unit compile loop for the next incremental check.
==============================================================================================*/

/*  GCC and Clang both write dep files; neither streams the list on stdout. */

static bool
platform_cc_dep_is_inline( compiler_t compiler )
{
    ( void )compiler;
    return false;
}

static void
platform_cc_dep_flag( compiler_t compiler, const char* obj_dir, char* buf, size_t size )
{
    ( void )compiler;
    ( void )obj_dir;   // -MMD writes the .d next to the -o object; no directory argument.
    snprintf( buf, size, "-MMD" );
}

/*==============================================================================================
    --- Compiler: Multi-Process Compilation ---

    No equivalent on GCC / Clang: they take one source per invocation, so there is no
    batch for a flag to split. Concurrency on POSIX comes from the target scheduler.
==============================================================================================*/

static void
platform_cc_mp_flag( compiler_t compiler, int unit_count, char* buf, size_t size )
{
    ( void )compiler;
    ( void )unit_count;
    ( void )size;
    buf[ 0 ] = '\0';
}

/*==============================================================================================
    --- Linker: Object File Pattern ---

    Returns the glob pattern used to reference all compiled objects for a target.
    GCC/Clang use *.o; MSVC uses *.obj.
==============================================================================================*/

static void
platform_lk_obj_pattern( const char* obj_dir, char* buf, size_t size )
{
    snprintf( buf, size, "%s/*.o", obj_dir );
}

/*==============================================================================================
    --- Linker: Dependency Library ---

    Appends the correct artifact path for a declared dependency. Using the full path
    (bin/libname.a or bin/libname.so) rather than -l flags avoids needing -Lbin before
    -l tokens and works for both static archives and shared libraries.

    dep_type drives the extension: TARGET_DYNAMIC_LIB -> .so, anything else -> .a.
    Callers are responsible for mapping TARGET_DYNAMIC_LIB -> TARGET_STATIC_LIB when
    in monolithic mode before calling here.
==============================================================================================*/

static void
platform_lk_append_dep_lib( const char* dep_name, target_type_t dep_type, char* buf, size_t size )
{
    if ( dep_type == TARGET_DYNAMIC_LIB )
        str_append_tok( buf, size, "bin/lib%s.so", dep_name );
    else
        str_append_tok( buf, size, "bin/lib%s.a",  dep_name );
}

/*==============================================================================================
    --- Linker: System Libraries ---

    Appends the POSIX system libraries needed by most ORB targets.
    -ldl covers dlopen (module system); -lpthread covers the scheduler.
==============================================================================================*/

static void
platform_lk_append_sys_libs( char* buf, size_t size )
{
    str_append_tok( buf, size, "-lm -lpthread -ldl" );
}

/*==============================================================================================
    --- Linker: Pre-Link Cleanup ---

    No-op on POSIX: debug information is embedded as DWARF inside the binary.
    There are no separate per-link PDB files to rotate or sweep, so neither the
    target name nor the config selects anything to sweep here.
==============================================================================================*/

static void
platform_lk_pre_link( const char* target_name, config_t config )
{
    ( void )target_name;
    ( void )config;
}

/*==============================================================================================
    --- Linker: Fill Static Lib Command ---

    ar archives .o files into a flat .a. No PDB, no dep resolution;
    lk->pdb and lk->libs are left empty.

    Assembled command: ar rcs bin/libname.a obj/name/*.o
    The /bin/sh -c wrapper in platform_spawn expands the *.o glob.

    A shipping build compiles with -flto, so the members are LTO bytecode rather than
    native objects. "ar" reads them through the linker plugin binutils loads by default,
    so the archive command itself is unchanged -- is_shipping is taken for signature
    parity with the Win32 counterpart, where lib.exe does need an explicit /LTCG.
==============================================================================================*/

static void
platform_lk_fill_static( const char* target_name, bool is_shipping, link_cmd_t* lk )
{
    ( void )is_shipping;
    snprintf( lk->exe,      sizeof( lk->exe ),      "ar" );
    snprintf( lk->artifact, sizeof( lk->artifact ),  "bin/lib%s.a", target_name );
    snprintf( lk->flags,    sizeof( lk->flags ),     "rcs" );
    snprintf( lk->output,   sizeof( lk->output ),    "bin/lib%s.a", target_name );
}

/*==============================================================================================
    --- Linker: Fill Shared Lib / Executable Command ---

    Fills exe, artifact, flags, output for gcc/clang. lk->pdb is left empty --
    DWARF debug info is embedded in the binary by the compiler flags (-g).
    lk->inputs (obj glob) and lk->libs (dep + system libs) are filled by the caller.

    DLLs use -shared and produce bin/libname.so.
    Executables produce bin/name with no special flags.

    A shipping build repeats -flto here: the compile step emitted bytecode instead of
    native code, so the link is where that code is actually generated and optimized
    across units. The 'subsystem' setting has no POSIX counterpart -- an executable is
    distinguished by its entry point, not by a header field -- and is ignored.
==============================================================================================*/

static void
platform_lk_fill_dynamic( build_context_t* ctx, target_info_t* target, link_cmd_t* lk )
{
    const bool  is_dll = ( target->type == TARGET_DYNAMIC_LIB );
    const char* cc     = ( ctx->compiler == COMPILE_CLANG ) ? "clang" : "gcc";
    const char* lto    = ctx->is_shipping ? "-flto" : "";

    snprintf( lk->exe, sizeof( lk->exe ), "%s", cc );

    if ( is_dll )
    {
        snprintf( lk->artifact, sizeof( lk->artifact ),  "bin/lib%s.so", target->name );
        snprintf( lk->flags,    sizeof( lk->flags ),     "-shared%s%s", lto[ 0 ] ? " " : "", lto );
        snprintf( lk->output,   sizeof( lk->output ),    "-o bin/lib%s.so", target->name );
    }
    else
    {
        snprintf( lk->artifact, sizeof( lk->artifact ),  "bin/%s", target->name );
        snprintf( lk->flags,    sizeof( lk->flags ),     "%s", lto );
        snprintf( lk->output,   sizeof( lk->output ),    "-o bin/%s", target->name );
    }
}

// clang-format on
/*============================================================================================*/
#endif
