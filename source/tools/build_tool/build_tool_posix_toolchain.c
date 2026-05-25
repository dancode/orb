/*==============================================================================================

    build_tool_posix_toolchain.c -- GCC / Clang compiler and linker platform layer.

    Provides all platform_cc_* and platform_lk_* functions for GCC and Clang.
    The Win32 counterpart is build_tool_win_toolchain.c; both files expose
    identical symbols so 06_compile.c and 07_link.c branch on none of them.

    Compiler functions:
        platform_cc_exe()               -- compiler executable name
        platform_cc_base_flags()        -- core compile flags (standard, warnings, config)
        platform_cc_append_include()    -- append "-I <path>" to an includes field
        platform_cc_append_define()     -- append "-D<name>" to a defines field
        platform_cc_output_flags()      -- output directory flags (see note below)
        platform_cc_dep_flag()          -- dependency-tracking flag string

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
        platform_cc_output_flags() is therefore a no-op here. When 06_compile.c gains
        a per-unit compilation loop for POSIX, it will build the -o flag itself.
        Until then, GCC outputs .o files into the current working directory.

    Dependency tracking (platform_cc_dep_flag):
        MSVC emits "Note: including file: <path>" inline on stdout (/showIncludes).
        GCC / Clang write a Makefile .d file after compilation (-MMD).
        The inline-stream parser in 05_spawn.c process_includes_line() is MSVC-specific;
        on POSIX the includes file is left empty and incremental header tracking is
        skipped. This means header edits always trigger a full recompile (safe but
        slower than necessary). A future POSIX dep-file parser will fix this.

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
==============================================================================================*/

static void
platform_cc_base_flags( config_t config, char* buf, size_t size )
{
    size_t used = strlen( buf );
    const char* sep = used ? " " : "";
    const char* cfg = ( config == CONFIG_DEBUG ) ? "-g -O0" : "-O2";
    snprintf( buf + used, size - used,
              "%s-c -Wall -Wextra -Werror -std=c11 -fPIC %s", sep, cfg );
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
    snprintf( buf + used, size - used, "%s-I %s", used ? " " : "", path );
}

static void
platform_cc_append_define( const char* name, char* buf, size_t size )
{
    size_t used = strlen( buf );
    snprintf( buf + used, size - used, "%s-D%s", used ? " " : "", name );
}

/*==============================================================================================
    --- Compiler: Output Flags ---

    No-op on POSIX -- GCC/Clang require per-file -o <dir>/<stem>.o rather than a
    directory target. 06_compile.c will need a per-unit loop to build this flag.
    Until then, objects land in the current working directory under <stem>.o.
==============================================================================================*/

static void
platform_cc_output_flags( const char* obj_dir, char* buf, size_t size )
{
    ( void )obj_dir;
    ( void )buf;
    ( void )size;
}

/*==============================================================================================
    --- Compiler: Dependency Tracking Flag ---

    Returns an empty string -- GCC -MMD writes .d files after compilation, but
    the inline-stream parser in 05_spawn.c expects the MSVC /showIncludes format.
    When a POSIX .d-file parser is added, this should return "-MMD".
==============================================================================================*/

static const char*
platform_cc_dep_flag( void )
{
    return "";
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

    Appends the static archive path for a declared dependency. Using the full path
    (bin/libname.a) rather than -l flags avoids needing -Lbin before the -l tokens
    and works for both static archives and direct .so paths.
==============================================================================================*/

static void
platform_lk_append_dep_lib( const char* dep_name, char* buf, size_t size )
{
    size_t used = strlen( buf );
    snprintf( buf + used, size - used, "%sbin/lib%s.a", used ? " " : "", dep_name );
}

/*==============================================================================================
    --- Linker: System Libraries ---

    Appends the POSIX system libraries needed by most ORB targets.
    -ldl covers dlopen (module system); -lpthread covers the scheduler.
==============================================================================================*/

static void
platform_lk_append_sys_libs( char* buf, size_t size )
{
    size_t used = strlen( buf );
    snprintf( buf + used, size - used,
              "%s-lm -lpthread -ldl", used ? " " : "" );
}

/*==============================================================================================
    --- Linker: Pre-Link Cleanup ---

    No-op on POSIX: debug information is embedded as DWARF inside the binary.
    There are no separate per-link PDB files to rotate or sweep.
==============================================================================================*/

static void
platform_lk_pre_link( const char* target_name )
{
    ( void )target_name;
}

/*==============================================================================================
    --- Linker: Fill Static Lib Command ---

    ar archives .o files into a flat .a. No PDB, no dep resolution;
    lk->pdb and lk->libs are left empty.

    Assembled command: ar rcs bin/libname.a obj/name/*.o
    The /bin/sh -c wrapper in platform_spawn expands the *.o glob.
==============================================================================================*/

static void
platform_lk_fill_static( const char* target_name, link_cmd_t* lk )
{
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
==============================================================================================*/

static void
platform_lk_fill_dynamic( build_context_t* ctx, target_info_t* target, link_cmd_t* lk )
{
    const bool  is_dll = ( target->type == TARGET_DYNAMIC_LIB );
    const char* cc     = ( ctx->compiler == COMPILE_CLANG ) ? "clang" : "gcc";

    snprintf( lk->exe, sizeof( lk->exe ), "%s", cc );

    if ( is_dll )
    {
        snprintf( lk->artifact, sizeof( lk->artifact ),  "bin/lib%s.so", target->name );
        snprintf( lk->flags,    sizeof( lk->flags ),     "-shared" );
        snprintf( lk->output,   sizeof( lk->output ),    "-o bin/lib%s.so", target->name );
    }
    else
    {
        snprintf( lk->artifact, sizeof( lk->artifact ),  "bin/%s", target->name );
        snprintf( lk->flags,    sizeof( lk->flags ),     "" );
        snprintf( lk->output,   sizeof( lk->output ),    "-o bin/%s", target->name );
    }
}

// clang-format on
/*============================================================================================*/
