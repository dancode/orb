/*==============================================================================================

    build_tool.h -- Core abstractions for the custom ORB build system.

    This header defines the "schema" for the entire build system. It separates the
    logic of *how* to build (build_tool.c and friends) from the data of *what* to
    build (build_tool_targets.c).

    Architectural Goals:
    - High Performance: Minimal filesystem overhead, direct tool invocation,
      no shell-out indirection beyond what cmd.exe requires for builtins/globs.
    - Simplicity: A flat, unified target pool with explicit dependencies. No
      Makefile / CMake DSL -- everything is described as plain C structs.
    - Flexibility: Multiple standalone IDE solutions share the same target pool.
    - Automation: Recursive dependency resolution, automatic vcvars env setup,
      and self-hosting bootstrap from a single .bat file.
    - Parallelism: A topological worker pool drives CLI builds across N threads
      with per-target output capture; VS solution builds defer to MSBuild's own
      scheduler via ProjectDependencies + the -no-deps flag.

    Unity build:
    - build_tool.exe is itself a unity build. build_tool.c #includes every other
      .c file in execution order (platform layer -> 01_prim -> 02_data ->
      03_registry -> 04_env -> 05_log -> 06_spawn -> 07_compile -> 08_link ->
      09_exec -> 10_sched -> 11_clean -> 12_gen_manifest -> 12_gen_nmake ->
      12_gen_json -> 12_gen_vscode -> 12_gen_msbuild -> test -> 00_util).
      All "static" functions are visible across the whole tool while still
      compiling in a single cl.exe invocation.

    Build Output Format:
    +-------------------------+------------------------------+
    |           Tag           |         Meaning              |
    +-------------------------+------------------------------+
    | [orb build]             | per-target compile start     |
    +-------------------------+------------------------------+
    | [orb completed]         | target built successfully    |
    +-------------------------+------------------------------+
    | [orb skipped]           | target already up to date    |
    +-------------------------+------------------------------+
    | [orb FAILED]            | per-target failure           |
    +-------------------------+------------------------------+
    | [orb parallel]          | scheduler start              |
    +-------------------------+------------------------------+
    | [orb clean]             | clean summary                |
    +-------------------------+------------------------------+
    | [orb reflect]           | codegen step                 |
    +-------------------------+------------------------------+
    | [orb cmd]               | raw command echo             |
    +-------------------------+------------------------------+
    | [orb src]               | source files                 |
    +-------------------------+------------------------------+
    | [orb vcvars]            | VS env discovery             |
    +-------------------------+------------------------------+
    | [orb warn]              | non-fatal warning            |
    +-------------------------+------------------------------+
    | [orb error]             | fatal error                  |
    +-------------------------+------------------------------+

==============================================================================================*/
#ifndef BUILD_TOOL_H
#define BUILD_TOOL_H

#ifdef _WIN64              // 64-bit Windows code (this was added to prevent 32bit cl.exe)
#elif defined( _WIN32 )    // 32-bit Windows code
    #error 32bit windows
#endif

// clang-format off

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*==============================================================================================
    --- Platform Abstraction Types ---

    These types are defined here so build_tool.h (and every module that includes
    it) can use them in function signatures and local variables without depending
    on MSVC-specific headers. The platform layer (build_tool_win.c) provides the
    concrete implementations named platform_*().
==============================================================================================*/

#if defined( _WIN32 )
    typedef int64_t platform_mtime_t;   // file last-modified timestamp (FILETIME epoch)
    #define PATH_SEP                    "\\"
    #define platform_is_abs_path( p )   ( (p)[0] == '\\' || (p)[0] == '/' || (p)[1] == ':' )
#else
    typedef int64_t platform_mtime_t;   // st_mtime is int64_t on 64-bit POSIX
    #define PATH_SEP                    "/"
    #define platform_is_abs_path( p )   ( (p)[0] == '/' )
#endif

typedef struct
{
    char name[ 512 ];   // entry file name (no directory prefix)
    bool is_dir;        // true when the entry is a subdirectory

} platform_find_data_t;

typedef intptr_t platform_find_t;
#define PLATFORM_FIND_INVALID  ( ( platform_find_t )( -1 ) )

/*  Memory-mapped file view. Filled by platform_map_file(); released by platform_unmap_file().
    data is NULL and size is 0 when the file exists but is empty (valid; no entries to check).
    Both handles are NULL in the empty-file case. */

typedef struct
{
    const char* data;   // read-only pointer to mapped bytes, NULL if file is empty
    size_t      size;   // file size in bytes (0 for an empty file)
    void*       _file;  // opaque file handle (platform_unmap_file closes it)
    void*       _map;   // opaque mapping handle (platform_unmap_file closes it)

} platform_mapped_file_t;

/*  Callback type for platform_spawn_capture().
    Called once per complete output line; line is null-terminated, no trailing newline. */

typedef void ( *platform_line_fn_t )( char* line, void* userdata );

/*  Opaque threading types.  Sized to fit the largest platform implementation;
    the platform .c file casts _opaque to the real OS type.
    _Static_assert guards in the implementation catch size underflows. */

#define PLATFORM_MUTEX_BYTES 64
#define PLATFORM_COND_BYTES  64

typedef struct { uint8_t _opaque[ PLATFORM_MUTEX_BYTES ]; } platform_mutex_t;
typedef struct { uint8_t _opaque[ PLATFORM_COND_BYTES  ]; } platform_cond_t;

/*  platform_thread_t is void* on all platforms (Win32 HANDLE / heap-allocated pthread_t). */

typedef void* platform_thread_t;

/*  platform_tls_t is a 32-bit slot index on both Win32 (DWORD) and pthreads (pthread_key_t).
    PLATFORM_TLS_INVALID is the all-bits-set sentinel that signals "not yet allocated". */

#if defined( _WIN32 )
    typedef uint32_t platform_tls_t;
    #define PLATFORM_TLS_INVALID    ( ( platform_tls_t )0xFFFFFFFFu )
    #define PLATFORM_THREAD_ENTRY   unsigned __stdcall
    typedef unsigned ( __stdcall *platform_thread_fn_t )( void* );
#else
    typedef uint32_t platform_tls_t;
    #define PLATFORM_TLS_INVALID    ( ( platform_tls_t )( ~0u ) )
    #define PLATFORM_THREAD_ENTRY   void*
    typedef void* ( *platform_thread_fn_t )( void* );
#endif

/*==============================================================================================
    --- Project Constants ---
==============================================================================================*/

/*  Physical allocation for all command line buffers. 
    CMD_BUF_WORK_MAX is the soft write limit enforced by cmd_append(); 
    The 4096-byte gap absorbs appending text not part of command */

#define CMD_BUF_MAX                 16384
#define CMD_BUF_WORK_MAX            12288

/*  Max entries in each NULL-terminated slot array on target_info_t (units/deps/tool_deps).
    Loops iterate with an `i < TARGET_MAX_SLOTS` bound so a fully-filled array
    (no NULL terminator) cannot read into adjacent struct fields. */

#define TARGET_MAX_SLOTS            16

/*  Capacity of the dynamic target and solution pools in build_tool_02_data.c.
    build_tool and reflect_tool are hardcoded; all others are loaded from orb.targets. */

#define MAX_TARGETS                 64
#define MAX_SOLUTIONS               16
#define MAX_SLN_TARGETS             64

/*  Max extra include directories per target or solution declared via 'include_dir'.
    These are appended to AdditionalIncludeDirectories/NMakeIncludeSearchPath in vcxproj
    gen and to /I flags in CLI compile. Stored as absolute paths. */

#define MAX_EXTRA_INCLUDE_DIRS      8
#define MAX_EXTRA_DEFINES           16
#define MAX_EXTRA_COMPILE_FLAGS     8
#define MAX_EXTRA_LINK_FLAGS        8

/*  Path buffer size for every filesystem path the build tool constructs.
    Windows MAX_PATH is 260; 512 gives generous headroom for composite paths.
    <obj_dir>\<filename> without forcing us to opt into long-path support */

#define PATH_MAX                    512

/*  Root directory for all build outputs: VS project files, intermediates, and generated code.
    NOTE: Update bootstrap_build_tool.bat if you change this. */

#define BUILD_DIR                   "build"

/*==============================================================================================
    --- Known Constraint: no spaces in build paths ---

    Compiler, linker, and reflect command lines are assembled from space-separated,
    UNQUOTED tokens and spawned directly (no cmd.exe shell -- see build_run_cmd). A
    source file, object dir, generated dir, or target root containing a space would be
    split into multiple arguments and the build would fail in confusing ways. Keep the
    engine root and every target source tree on space-free paths.

    The one path we cannot control -- vcvarsall.bat, always under "Program Files" -- IS
    quoted explicitly in build_tool_04_env.c. Everything else assumes space-free paths.
==============================================================================================*/

/*==============================================================================================
    --- Output Flags ---
==============================================================================================*/

/*  Bitfield controlling which sections of the build log are printed.
    Set once at startup via CLI flags (-q / -v / --out <hex>); read by all
    build_tool modules directly as g_out_flags. No need to thread it through
    build_context_t since output verbosity is a process-global setting. */

typedef unsigned int out_flags_t;

// Compile verbose detail (bits 0-5)
#define ORB_OUT_COMPILE_SOURCES  ( 1u << 0  )  // sources: <absolute paths>
#define ORB_OUT_COMPILE_FLAGS    ( 1u << 1  )  // flags:   /W4 /WX /Zi /Od ...
#define ORB_OUT_COMPILE_DEFINES  ( 1u << 2  )  // defines: OS_WINDOWS ARCH_X64 ...
#define ORB_OUT_COMPILE_INCLUDES ( 1u << 3  )  // includes: source gen_dir ...
#define ORB_OUT_COMPILE_OUTPUT   ( 1u << 4  )  // output:  obj=... pdb=...
#define ORB_OUT_COMPILE_CMD      ( 1u << 5  )  // raw cl.exe command line (long string)

// Link verbose detail (bits 6-11)
#define ORB_OUT_LINK_INPUTS      ( 1u << 6  )  // inputs:  objDir/*.obj
#define ORB_OUT_LINK_LIBS        ( 1u << 7  )  // libs:    dep.lib user32.lib ...
#define ORB_OUT_LINK_FLAGS       ( 1u << 8  )  // flags:   /nologo /DLL ...
#define ORB_OUT_LINK_OUTPUT      ( 1u << 9  )  // output:  bin/target.lib
#define ORB_OUT_LINK_PDB         ( 1u << 10 )  // pdb:     bin/target_xxx.pdb
#define ORB_OUT_LINK_CMD         ( 1u << 11 )  // raw link.exe / lib.exe command line

// General / always-on (bits 12+)
#define ORB_OUT_SCHEDULER        ( 1u << 12 )  // [orb parallel] N targets, M threads
#define ORB_OUT_SUMMARY_COMPILE  ( 1u << 13 )  // [orb compiling] or [orb skipped] per-target
#define ORB_OUT_SUMMARY_LINK     ( 1u << 14 )  // [orb link] target -> artifact
#define ORB_OUT_REFLECT          ( 1u << 15 )  // [orb reflect] codegen steps
#define ORB_OUT_VCVARS           ( 1u << 16 )  // [orb vcvars] VS env discovery
#define ORB_OUT_MSVC_OUTPUT      ( 1u << 17 )  // [MSVC] raw cl/link/lib passthrough lines
#define ORB_OUT_ARGS             ( 1u << 18 )  // startup banner: echo raw argv on a second line/col
#define ORB_OUT_TIMING           ( 1u << 19 )  // per-build timing summary (slowest-first table)

// Convenience masks -- verbose detail only, summaries excluded.
#define ORB_OUT_ANY_COMPILE     ( ORB_OUT_COMPILE_SOURCES  | ORB_OUT_COMPILE_FLAGS    | \
                                  ORB_OUT_COMPILE_DEFINES  | ORB_OUT_COMPILE_INCLUDES | \
                                  ORB_OUT_COMPILE_OUTPUT   | ORB_OUT_COMPILE_CMD )

#define ORB_OUT_ANY_LINK        ( ORB_OUT_LINK_INPUTS | ORB_OUT_LINK_LIBS   | \
                                  ORB_OUT_LINK_FLAGS  | ORB_OUT_LINK_OUTPUT | \
                                  ORB_OUT_LINK_PDB    | ORB_OUT_LINK_CMD )

#define ORB_OUT_SUMMARY         ( ORB_OUT_SUMMARY_COMPILE | ORB_OUT_SUMMARY_LINK )

// Preset combinations -- pass as --out <hex> or use -q / -v shorthands.
#define ORB_OUT_QUIET           ( ORB_OUT_SCHEDULER )
#define ORB_OUT_NORMAL          ( ORB_OUT_QUIET | ORB_OUT_SUMMARY_COMPILE | \
                                  ORB_OUT_REFLECT | ORB_OUT_VCVARS | ORB_OUT_MSVC_OUTPUT )

#define ORB_OUT_TESTING         ( ORB_OUT_ANY_COMPILE ) // ( ORB_OUT_SUMMARY_COMPILE | ORB_OUT_VCVARS | ORB_OUT_MSVC_OUTPUT )

#define ORB_OUT_VERBOSE         ( 0xFFFFFFFFu )
#define ORB_OUT_DEFAULT         ( ORB_OUT_TESTING )

/*==============================================================================================
    --- Command Helper Types ---

    -   Reused cmd-line buffer for assembling cl.exe / link.exe / lib.exe invocations.
    -   cmd_append() writes into buf up to CMD_BUF_WORK_MAX bytes.
    -   The remaining headroom to CMD_BUF_MAX is for padding (e.g. prepending "cmd.exe /C ")
    -   Truncated is set when the work limit is hit so the caller can spill to an
        RSP file (see cmd_spill_to_response_file).
==============================================================================================*/

typedef struct cmd_buf_s
{
    size_t size;                // Current length of the command line.
    bool   truncated;           // Set by cmd_append() when CMD_BUF_WORK_MAX is hit.
    char   buf[ CMD_BUF_MAX ];  // Physical buffer; write limit is CMD_BUF_WORK_MAX.

} cmd_buf_t;

/* Safe threshold below cmd.exe's 8191-char command line limit. 
    Leaves room for the vcvars prefix that build_run_cmd() prepends. */

#define CMD_RSP_THRESHOLD 7000

/*==============================================================================================
    --- Build Configuration ---
==============================================================================================*/

typedef enum
{
    CONFIG_DEBUG,               // No optimizations, full debug symbols, MDd runtime.
    CONFIG_RELEASE,             // Full optimizations, minimal debug symbols, MD runtime.
    CONFIG_COUNT,               // Sentinel -- used as "all configs" in warn_suppress_t.

} config_t;

typedef enum
{
    COMPILE_MSVC,
    COMPILE_CLANG,
    COMPILE_ALL,                // compile_flag sentinel: applies regardless of active compiler

} compiler_t;

typedef struct
{
    const char* flag;           // e.g. "/wd4101" or "-Wno-unused-variable"
    config_t    config;         // CONFIG_DEBUG, CONFIG_RELEASE, or CONFIG_COUNT for all configs
    compiler_t  compiler;       // COMPILE_MSVC or COMPILE_CLANG

} warn_suppress_t;

typedef enum
{
    TARGET_STATIC_LIB,          // Compiles to a .lib archive via lib.exe.
    TARGET_DYNAMIC_LIB,         // Compiles to a .dll via link.exe /DLL.
    TARGET_EXECUTABLE           // Compiles to a .exe via link.exe.

} target_type_t;

/*==============================================================================================
    --- Per-Target Compiler Flag Entry ---

    One entry in a target's extra_compile_flags[] list.
    The compiler field gates the flag to a specific toolchain; COMPILE_ALL
    means the flag is appended regardless of the active compiler.
==============================================================================================*/

typedef struct
{
    compiler_t  compiler;       // COMPILE_MSVC, COMPILE_CLANG, or COMPILE_ALL
    char        flag[ 128 ];    // e.g. "/GS-" or "/WHOLEARCHIVE:bin/core.lib"

} extra_flag_t;

/*==============================================================================================
    --- Executable Subsystem ---

    Declares the Win32 /SUBSYSTEM: linker flag for TARGET_EXECUTABLE targets.
    SUBSYSTEM_DEFAULT defers to the linker's built-in default (CONSOLE on Win32).
    Set via 'subsystem <console|windows>' in orb.targets. Ignored for DLL/LIB targets
    and no-op on POSIX (no subsystem concept; target type is determined by the entry point).
==============================================================================================*/

typedef enum
{
    SUBSYSTEM_DEFAULT,   // linker default (CONSOLE on Win32 without an explicit flag)
    SUBSYSTEM_CONSOLE,   // /SUBSYSTEM:CONSOLE -- console window, main() entry point
    SUBSYSTEM_WINDOWS,   // /SUBSYSTEM:WINDOWS -- no console window, WinMain() entry point

} subsystem_t;

/*==============================================================================================
    --- Target Descriptor ---
==============================================================================================*/

/*  Represents a single buildable unit in the ORB ecosystem. 
    It contains all metadata required to compile and link the target. 
    
    Targets are pooled in g_targets[] (see build_tool_targets.c).
    Shared across every IDE solution that references them by name. */

typedef struct target_info_s
{
    const char*     name;           // Unique name (e.g., "base", "core", "app").
    target_type_t   type;           // Artifact type (LIB, DLL, or EXE).
    bool            has_type;       // True if 'type' was explicitly set in orb.targets.
    const char*     root_dir;       // Absolute path to the source directory for this target.
    const char*     virtual_folder; // Display-only folder name (VS Solution Explorer)

    /*  Translation Units (Unity Build Fragments)
        Each entry is typically an umbrella .c file that includes other sources.
        Multiple units allow the scheduler to parallelize cl.exe calls. */

    const char*     units[ TARGET_MAX_SLOTS ];

    /*  Link Dependencies: Other targets that produce .libs this target must link against.
        Drives both the linker's input list and the parallel scheduler's topological order. */

    const char*     deps[ TARGET_MAX_SLOTS ];

    /*  Tool Dependencies: Standalone utilities that must exist to build this target.
        These are built recursively but NOT linked into the final binary.
        Ex: any target with 'has_reflect' depends on reflect_tool.exe as a tool dep. */

    const char*     tool_deps[ TARGET_MAX_SLOTS ];

    /*  Monolithic-only Link Dependencies: modules that are loaded dynamically at runtime
        in modular builds but must be explicitly linked in monolithic builds (BUILD_STATIC).
        Ignored unless ctx->is_monolithic is true at link time. */

    const char*     mono_deps[ TARGET_MAX_SLOTS ];

    /*  Reflection metadata: if true, reflect_tool.exe is invoked on root_dir
        before compilation. Generated files land in <build_dir>/generated/ and
        are appended to the cl.exe command line for this target. */

    bool            has_reflect;

    /*  Base name for generated .c/.h files. Default is NULL, which means the
        files are named after the target (e.g. "core" -> "core.generated.c/h"). */

    const char*     reflect_name;

    /*  If true, this is a build-time tool executable (e.g. reflect_tool).
        Tool targets survive global clean and are always rebuilt by our own
        dep resolution -- never delegated to VS ProjectDependencies. */

    bool            is_tool;

    /*  If true, this is the build orchestrator itself. Every other target in the
        solution implicitly depends on it -- no target can run its NMake command
        until bin\build_tool.exe exists. */

    bool            is_build_tool;

    /*  If true, this is the reflection code-generator tool. Targets with
        has_reflect = true automatically depend on whichever target carries
        this flag -- no hardcoded name needed anywhere in the build logic. */

    bool            is_reflect_tool;

    /*  If true, this target is a host-only engine service (sys, core, mod, app, ref).
        Dynamic targets may not list it as a dep -- globals and state live exclusively
        in the host exe. DLLs access these services through the module API vtable.
        validate_targets() rejects any dynamic target that violates this contract. */

    bool            is_host_only;

    /*  Extra include directories declared via 'include_dir' in orb.targets.
        Stored as absolute paths. Appended to /I flags in CLI compile and to
        AdditionalIncludeDirectories/NMakeIncludeSearchPath in vcxproj gen.
        NULL-terminated; unused slots are NULL. */

    const char*     extra_include_dirs[ MAX_EXTRA_INCLUDE_DIRS ];

    /*  Per-target preprocessor defines declared via 'define' in orb.targets.
        Appended after the global define tables during compile and IntelliSense gen.
        NULL-terminated; unused slots are NULL. */

    const char*     extra_defines[ MAX_EXTRA_DEFINES ];

    /*  Per-target compiler flags declared via 'compile_flag <msvc|clang|all> <flag>'
        in orb.targets. Only entries whose .compiler matches ctx->compiler (or COMPILE_ALL)
        are appended in cc_fill_compile_cmd. Not forwarded to IntelliSense NMakeAdditionalOptions. */

    extra_flag_t extra_compile_flags[ MAX_EXTRA_COMPILE_FLAGS ];
    int                 extra_compile_flag_count;

    /*  Executable subsystem declared via 'subsystem <console|windows>' in orb.targets.
        Translated to /SUBSYSTEM: by platform_lk_fill_dynamic(). SUBSYSTEM_DEFAULT
        preserves the linker's built-in default (CONSOLE). Ignored for DLL/LIB targets. */

    subsystem_t         subsystem;

    /*  Per-target linker flags declared via 'link_flag <msvc|clang|all> <flag>'
        in orb.targets. Appended to lk.flags after platform_lk_fill_dynamic() fills
        the base flags. Only entries whose .compiler matches ctx->compiler (or COMPILE_ALL)
        are applied. Raw flags -- no IntelliSense forwarding needed (linker flags don't
        affect the parse model). */

    extra_flag_t extra_link_flags[ MAX_EXTRA_LINK_FLAGS ];
    int                 extra_link_flag_count;

    /*  True when this target was loaded via an 'import' directive rather than
        from the root orb.targets. External targets are excluded from "build all"
        (no -target flag) and from vcxproj gen, but remain available as deps. */

    bool            is_external;

} target_info_t;

/*==============================================================================================
    --- Build Execution Context ---
==============================================================================================*/

/*  State passed through the build process to maintain consistency. */

typedef struct build_context_s
{
    config_t        config;         // Selected build config (Debug/Release).
    compiler_t      compiler;       // Active compiler (COMPILE_MSVC or COMPILE_CLANG).
    bool            is_monolithic;  // DLL's build as LIB's and BUILD_STATIC defined globally.
    bool            is_shipping;    // Enables /GL + /LTCG for whole-program optimization (-shipping flag).
    bool            skip_deps;      // skip link dep resolution. Set by -no-deps (VS) and scheduler.
    bool            skip_tool_deps; // skip tool dep + implicit reflect dep. Set by scheduler only.
    bool            force_rebuild;  // bypass the up-to-date check; always compile + link.
    bool            compile_only;   // -compile-only: compile all units, no link (VS Ctrl+F7).
    char*           target_name;    // -target <name>: restrict the build to one target (VS and CLI).
    char*           file_path;      // -file <path>: compile one file (CLI use), no link.

} build_context_t;

/*==============================================================================================
    --- Solution Descriptor ---
==============================================================================================*/

/*  Defines a Visual Studio solution, an array of targets from the target pool.
    This allows the build system to generate specialized workspaces such
    as "orb_build.sln" without polluting the full main engine workspace. */

typedef struct
{
    /*  Name of the .sln file (e.g. "orb_make"). */

    const char*     name;

    /*  NULL-terminated list of target names to include (embedded, no heap alloc).
        Populated by registry_load() from orb.targets; the generator looks each
        name up in g_targets[]. Embedded array decays to const char** so all
        existing for(*tn = sln->target_names; *tn; ++tn) loops are unchanged. */

    const char*     target_names[ MAX_SLN_TARGETS ];

    /* If non-NULL it will generate a "mega" source directory folder
       navigation project as "name"_nav" in the .sln file.
       just a browesable directory that does not compile. */

    const char*     nav_dir;

    /* Output directory for .sln and .vcxproj files (e.g. "build\\proj").
       Relative paths inside the emitted XML are computed from this depth
       so all solutions stay at a consistent distance from the project root. */

    const char*     out_dir;

    /* If true, DLL targets are built as static libs (BUILD_STATIC defined globally).
       NMake commands get -monolithic; IntelliSense defines include BUILD_STATIC and
       _STATIC for all deps (including dynamic-lib ones). */

    bool            is_monolithic;

    /*  Extra include directories declared via 'include_dir' in the solution block.
        Stored as absolute paths. Applied to every target vcxproj in this solution
        (both NMake and MSBuild gen). NULL-terminated; unused slots are NULL. */

    const char*     extra_include_dirs[ MAX_EXTRA_INCLUDE_DIRS ];

    /*  If non-NULL, emitted as StartupProject in ExtensibilityGlobals so VS opens
        this target's debugger by default. Set via 'startup <target-name>'. */

    const char*     startup_project;

    /*  True when this solution was loaded via 'import'. External solutions are
        excluded from -gen so the child project doesn't regenerate the engine's
        own .sln/.vcxproj files in its build directory. */

    bool            is_external;

} solution_info_t;

/*  Dynamic target and solution pools. Populated at startup by init_builtin_targets()
    (hardcoded build_tool + reflect_tool) then extended by registry_load("orb.targets"). */

extern target_info_t   g_targets[];
extern int             g_target_count;

extern solution_info_t g_solutions[];
extern int             g_solution_count;

/*==============================================================================================
    --- Compiler Command Type ---

    Holds each logical fragment of the final compiler command line.
    Assembled section-by-section by cc_fill_compile_cmd() in 06_compile.c;
    printed selectively by cc_print(); joined for execution by cc_assemble().
==============================================================================================*/

typedef struct
{
    char exe      [ 64          ];  // cl.exe / clang-cl.exe / gcc / clang
    char flags    [ 512         ];  // /c /nologo /W4 ... or -c -Wall ...
    char includes [ 512         ];  // /I source /I gen_dir  or  -I source -I gen_dir
    char defines  [ 1024        ];  // /DOS_WINDOWS ...  or  -DOS_WINDOWS ...
    char output   [ 512         ];  // /Fo<dir>/ /Fd<dir>/  or  -o <dir>/<unit>.o
    char sources  [ CMD_BUF_MAX ];  // absolute .c paths

} compile_cmd_t;

/*==============================================================================================
    --- Linker Command Type ---

    Holds each logical fragment of the final linker / archiver command line.
    Filled by build_target_link() in 07_link.c via the platform_lk_* helpers.
    lib.exe leaves pdb empty; link.exe fills all fields.
==============================================================================================*/

typedef struct
{
    char exe      [ 32       ];  // lib.exe / link.exe / ar / gcc / clang
    char artifact [ PATH_MAX ];  // final output path (display only)
    char flags    [ 512      ];  // /nologo /DLL /SUBSYSTEM:... ...  or  -shared ...
    char output   [ 512      ];  // /OUT:... /IMPLIB:...  or  -o ...
    char pdb      [ 256      ];  // /DEBUG /PDB:...  (empty on POSIX / for lib.exe)
    char inputs   [ 512      ];  // objDir\*.obj  or  objDir/*.o
    char libs     [ 1024     ];  // dep.lib ... user32.lib ...  or  -lm ...

} link_cmd_t;

/*==============================================================================================
    --- Orchestration API ---

    Everything below is the public surface for the unity-built build_tool.exe.
==============================================================================================*/

/*  Compiles all translation units for a target. Emits the cl.exe command line
    with /showIncludes so build_run_cmd_capture_includes can record the header set
    into <obj_dir>/_includes.txt for the next incremental check. */

bool build_target_compile( build_context_t* ctx, target_info_t* target, const char* obj_dir, const char* gen_dir );

/*  Compiles a single source file with the target's full flag/define/include set.
    No /showIncludes, no link step, no include tracking. CLI tool for targeted error
    checking (-file flag). file_path must be absolute. */

bool build_target_compile_single( build_context_t* ctx, target_info_t* target,
                                  const char* obj_dir, const char* gen_dir, const char* file_path );

/*  Compiles all unity units for a target with reflect, but no link step.
    Used by -compile-only (VS Ctrl+F7 via NMakeCompileFileCommandLine). */

bool build_target_compile_only( build_context_t* ctx, target_info_t* target );

/*  Links or archives the target's objects into the final artifact: lib.exe
    for static libs, link.exe (with /DLL or as an exe) for the rest. PDB paths
    are rotated per-link so an attached debugger never collides with the
    linker over a held-open symbol file. */

bool build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir );

/*  Locates the Visual Studio installation (via vswhere or hard-coded probes)
    and imports vcvarsall.bat's environment into THIS process via platform_putenv().
    Idempotent -- fast-paths out if cl.exe is already on PATH (Dev Cmd Prompt
    or VS-launched terminal). One-time cost, ~2.5s; saves ~50s across a full
    rebuild that would otherwise pay vcvars-prefix overhead per cl invocation. */

void build_setup_vc_env( void );

/*  Returns the Visual Studio internal major version to embed in generated project files
    (e.g. 17 for VS 2022, 18 for VS 2026). Resolution order:
      1. g_vs_major_version > 0: explicit override from -vs-version <year>.
      2. VisualStudioVersion env var set by vcvarsall / Developer Command Prompt.
      3. Fallback: 17 (VS 2022). */

int build_detect_vs_major( void );

/*  Set by -vs-version <year> in main(); 0 means auto-detect from the environment. */

extern int g_vs_major_version;

/*  Appends a formatted string to a command buffer. */

void cmd_append( cmd_buf_t* b, const char* fmt, ... );

/*  If the command buffer is near the shell limit (or already truncated), 
    spill everything after the first token (the tool exe name) to a response
    file at rsp_path and rewrite the buffer to "<exe> @<rsp_path>". Returns
    true if a response file was created. */

bool cmd_spill_to_response_file( cmd_buf_t* b, const char* rsp_path );

/*  Acquire a Windows named mutex scoped to a single target, blocking until
    granted. Used to serialize concurrent invocations of build_tool.exe that
    would otherwise both compile/link the same target's outputs. Returns an
    opaque handle that must be passed to build_unlock_target() -- or NULL on
    failure (in which case the caller proceeds without locking). */

void* build_lock_target( const char* target_name );

/*  Release a lock acquired by build_lock_target(). NULL is a safe no-op. */

void  build_unlock_target( void* lock );

/*  Run a command via the platform spawn layer (CreateProcess on Win32) and
    return its exit code. cmd is executed directly with NO cmd.exe wrapper, so
    the caller must pass a fully-formed command line: no shell builtins (del,
    for) and no shell-side glob expansion are available. (MSVC's cl/link/lib
    perform their own wildcard expansion, so *.obj inputs still resolve.)
    Output is redirected to the per-thread log file if a parallel worker is
    active (see sched_log_path), otherwise inherits the parent's stdout/stderr. */

int build_run_cmd( const char* cmd );

/*  Like build_run_cmd but routes through the shell (cmd.exe /c on Win32) so
    shell builtins (del, rd) and output redirections (>nul 2>nul) work. Used for
    housekeeping invocations such as build_clean() where "file not found" is
    expected; the caller prints its own human-readable summary instead of one
    line per call. */

int build_run_cmd_quiet( const char* cmd );

/*  Pipes the child's stdout+stderr back line-by-line through us. When
    includes_path is non-NULL, /showIncludes lines are parsed out and written
    there (system headers filtered); used for compile steps. When NULL, no
    includes file is written; used for link/lib steps. All non-include lines are
    forwarded to the active sink (worker log or stdout) prefixed with [MSVC]
    when ORB_OUT_MSVC_OUTPUT is set, or silently dropped when not. */

int build_run_cmd_capture_includes( const char* cmd, const char* includes_path );

/*  The core worker function. Handles recursive dependency resolution
    (unless ctx->skip_deps), per-target locking, the incremental-build
    timestamp check (artifact mtime vs. each unit, link dep, and recorded
    header), reflection codegen, then compile + link. Idempotent: a fully
    up-to-date target short-circuits before any cl.exe spawn.
    out_skipped may be NULL; when non-NULL it is set to true if the target was
    skipped because all artifacts were already up to date, false otherwise. */

bool build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped, uint64_t* out_elapsed_ms );

/*  Parallel scheduler. Builds the transitive closure of `root` (or every
    target in g_targets[] if `root` is NULL) using up to `thread_count`
    concurrent workers. Each worker calls build_target() with skip_deps=true;
    the scheduler itself owns dep ordering. Returns true iff all targets
    finished successfully. */

bool build_run_parallel( build_context_t* ctx, target_info_t* root, int thread_count );

/*  Deletes build artifacts. If target is non-NULL, only that target's artifacts
    are removed (bin/<name>.*, obj/<name>/). If NULL, a global wipe runs --
    is_tool executables are excluded so tools survive a full clean. */

void build_clean( target_info_t* target );

/*==============================================================================================
    --- Generation Manifest ---

    Resolved snapshot of generation intent, built once from g_targets[]/g_solutions[]
    before any generator runs. All generators receive a const pointer to this struct
    instead of re-filtering the raw globals -- filtering logic lives in one place.
==============================================================================================*/

typedef struct
{
    solution_info_t*    sln;
    target_info_t*      targets[ MAX_SLN_TARGETS ];
    int                 target_count;
} gen_sln_entry_t;

typedef struct
{
    char  build_tool_exe        [ PATH_MAX + 4 ];   // NMake: backslashes, quoted if absolute
    char  build_tool_exe_fwd    [ PATH_MAX + 4 ];   // VSCode: forward slashes, quoted
    char  engine_src_dir        [ PATH_MAX ];       // forward slashes; empty if no engine root
    char  engine_gen_dir        [ PATH_MAX ];       // forward slashes; empty if no engine root
    char  workspace_name        [ 256 ];            // first local solution name, or "workspace"

    gen_sln_entry_t  solutions[ MAX_SOLUTIONS ];
    int              solution_count;

    target_info_t*  local_targets[ MAX_TARGETS ];
    int             local_target_count;

    target_info_t*  exe_targets[ MAX_TARGETS ];
    int             exe_target_count;

    target_info_t*  ext_ref_targets[ MAX_TARGETS ];
    int             ext_ref_target_count;

} gen_manifest_t;

void gen_manifest_build( gen_manifest_t* m );

/*  Generates all .sln and .vcxproj files defined in the Solution Registry.
    This maps our custom build system into the Visual Studio IDE. */

void build_gen_projects( const gen_manifest_t* m );

/*  Generates compile_commands.json at the project root. Each target contributes
    one entry per unity compilation unit (supports multi-unit targets). Targets
    with has_reflect=true also get an entry for the generated .c file. Debug
    config is always used. Called alongside build_gen_projects() during -gen. */

void build_gen_compile_commands( const gen_manifest_t* m );

/*  Generates .vscode/tasks.json with build, clean, and regen tasks wired to
    build_tool.exe. The "build target" task includes a pickString dropdown
    populated from g_targets[]. Called during -gen. */

void build_gen_vscode( const gen_manifest_t* m );

/*  Generates MSBuild .sln/.vcxproj for IntelliSense. */

void build_gen_projects_msbuild( const gen_manifest_t* m );

// clang-format on
/*============================================================================================*/
#endif    // BUILD_TOOL_H