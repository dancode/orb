/*==============================================================================================

    build_tool.h -- Core abstractions for the custom ORB build system.

    This header defines the "schema" for the entire build system. It separates the
    logic of *how* to build (build_tool.c and friends) from the data of *what* to
    build (build_tool_targets.c).

    Architectural Goals:
    - High Performance: Minimal filesystem overhead, direct tool invocation,
      no shell-out indirection beyond what cmd.exe requires for builtins/globs.
    - Simplicity: A flat, unified target pool with explicit dependencies. No
      Makefile / CMake DSL — everything is described as plain C structs.
    - Flexibility: Multiple standalone IDE solutions share the same target pool.
    - Automation: Recursive dependency resolution, automatic vcvars env setup,
      and self-hosting bootstrap from a single .bat file.
    - Parallelism: A topological worker pool drives CLI builds across N threads
      with per-target output capture; VS solution builds defer to MSBuild's own
      scheduler via ProjectDependencies + the -no-deps flag.

    Unity build:
    - build_tool.exe is itself a unity build. build_tool.c #includes every other
      .c file in this directory in dependency order. This means all "static"
      functions are visible across the whole tool while still compiling in a
      single cl.exe invocation — and bootstrapping needs just one command line.

    Build Outout Format:
    ┌─────────────────────────┬──────────────────────────────┐
    │           Tag           │         Meaning              │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb build]             │ per-target compile start     │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb compiled]          │ target built successfully    │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb skipped]           │ target already up to date    │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb FAILED]            │ per-target failure           │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb parallel]          │ scheduler start              │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb clean]             │ clean summary                │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb reflect]           │ codegen step                 │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb cmd]               │ raw command echo             │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb src]               │ source files                 │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb vcvars]            │ VS env discovery             │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb warn]              │ non-fatal warning            │
    ├─────────────────────────┼──────────────────────────────┤
    │ [orb error]             │ fatal error                  │
    └─────────────────────────┴──────────────────────────────┘

==============================================================================================*/
#ifndef BUILD_TOOL_H
#define BUILD_TOOL_H

// clang-format off

#include <stdbool.h>
#include <time.h>

// =============================================================================
// --- Project Constants ---
// =============================================================================

// Max size for any single compiler/linker command line.

#define CMD_BUF_MAX 16384    

// Path buffer size for every filesystem path the build tool constructs.
// Windows MAX_PATH is 260; 512 gives generous headroom for composite paths.
// <obj_dir>\<filename> without forcing us to opt into long-path support

#define BT_PATH_MAX 512

// =============================================================================
// --- Helper Types ---
// =============================================================================

// Reused cmd-line buffer for assembling cl.exe / link.exe / lib.exe invocations.
// cmd_append() formats into `buf` and updates `size`; if an append cannot fit.
// `truncated` is set so the caller can decide whether to spill the tail into 
// a response file (see cmd_spill_to_response_file).

typedef struct cmd_buf_s
{
    size_t size;                // Current length of the command line.
    bool   truncated;           // Set by cmd_append() when an append could not fit.
    char   buf[ CMD_BUF_MAX ];  // The actual command line fragment.                                
       
} cmd_buf_t;

// Safe threshold below cmd.exe's 8191-char command line limit. 
// Leaves room for the vcvars prefix that build_run_cmd() prepends.

#define CMD_RSP_THRESHOLD 7000

// =============================================================================
// --- Build Configuration ---
// =============================================================================

typedef enum
{
    BT_CONFIG_DEBUG,   // No optimizations, full debug symbols, MDd runtime.
    BT_CONFIG_RELEASE, // Full optimizations, minimal debug symbols, MD runtime.
    BT_CONFIG_COUNT,   // Sentinel — used as "all configs" in warn_suppress_t.

} config_t;

// Compiler identity bitmask for warn_suppress_t.compiler_mask.

typedef enum
{
    BT_COMPILER_MSVC  = ( 1u << 0 ),  // cl.exe (native MSVC)
    BT_COMPILER_CLANG = ( 1u << 1 ),  // clang-cl.exe
    BT_COMPILER_ALL   = ( BT_COMPILER_MSVC | BT_COMPILER_CLANG ),

} compiler_flag_t;

// One entry in g_warn_suppressions[]. A suppression fires when:
//   config   == ctx->config  OR  config == BT_CONFIG_COUNT  (matches all)
//   compiler bit is set in compiler_mask

typedef struct
{
    const char*   flag;           // e.g. "/wd4101" or "-Wno-unused-variable"
    config_t      config;         // BT_CONFIG_DEBUG, BT_CONFIG_RELEASE, or BT_CONFIG_COUNT for all
    unsigned int  compiler_mask;  // BT_COMPILER_MSVC | BT_COMPILER_CLANG

} warn_suppress_t;

// These are defined in build_tool_targets.c. a list of all active warning suppressions
// that build_target_compile() iterates over.

extern warn_suppress_t g_warn_suppressions[];
extern int             g_warn_suppression_count;

typedef enum
{
    TARGET_STATIC_LIB,  // Compiles to a .lib archive via lib.exe.
    TARGET_DYNAMIC_LIB, // Compiles to a .dll via link.exe /DLL.
    TARGET_EXECUTABLE   // Compiles to a .exe via link.exe.

} target_type_t;

// =============================================================================
// --- Target Descriptor ---
// =============================================================================

// Represents a single buildable unit in the ORB ecosystem. 
// It contains all metadata required to compile and link the target. 
//
// Targets are pooled in g_targets[] (see build_tool_targets.c).
// Shared across every IDE solution that references them by name. */

typedef struct target_info_s
{
    const char*     name;           // Unique name (e.g., "base", "core", "app").
    target_type_t   type;           // Artifact type (LIB, DLL, or EXE).
    const char*     root_dir;       // Base path for source files relative to project root.
    const char*     sln_folder;     // Virtual folder in the Visual Studio solution.

    // Translation Units (Unity Build Fragments)
    // Each entry is typically an umbrella .c file that includes other sources.
    // Multiple units allow the scheduler to parallelize cl.exe calls.
    const char*     units[ 16 ];

    // Link Dependencies: Other targets that produce .libs this target must link against.
    // Drives both the linker's input list and the parallel scheduler's topological order.
    const char*     deps[ 16 ];

    // Tool Dependencies: Standalone utilities that must exist to build this target.
    // These are built recursively but NOT linked into the final binary. 
    // Ex: any target with 'has_reflect' depends on build_reflect.exe as a tool dep.
    const char*     tool_deps[ 16 ];

    // Reflection metadata: if true, build_reflect.exe is invoked on root_dir
    // before compilation. Generated files land in <build_dir>/generated/ and
    // are appended to the cl.exe command line for this target.
    bool            has_reflect;

    // Base name for generated .c/.h files. Default is NULL, which means the
    // files are named after the target (e.g. "core" -> "core.generated.c/h").
    const char*     reflect_name;

    // If true, this is a build-time tool executable (e.g. build_reflect).
    // Tool targets survive global clean and are always rebuilt by our own
    // dep resolution — never delegated to VS ProjectDependencies.
    bool            is_tool;

    // If true, this is the reflection code-generator tool. Targets with
    // has_reflect = true automatically depend on whichever target carries
    // this flag — no hardcoded name needed anywhere in the build logic.
    bool            is_reflect_tool;

} target_info_t;

// =============================================================================
// --- Global Target Registry ---
// =============================================================================

// The list of all targets defined in build_tool_targets.c and used by
// the orchestrator and solution generator.

extern target_info_t g_targets[];
extern int           g_target_count;

// =============================================================================
// --- Build Execution Context ---
// =============================================================================

// State passed through the build process to maintain consistency.

typedef struct build_context_s
{
    config_t        config;         // Selected build config (Debug/Release).
    bool            is_monolithic;  // If true, TARGET_DYNAMIC_LIB targets build as static libs with BUILD_STATIC defined globally.
    compiler_flag_t compiler;       // Active compiler (BT_COMPILER_MSVC or BT_COMPILER_CLANG).
    bool            skip_deps;      // skip recurse into dependencies. See build_target().
    bool            force_rebuild;  // bypass the up-to-date check; always compile + link.

/*  skip_deps: If true, build_target() does NOT recurse into its dependencies.
    The VS solution generator emits this flag so MSBuild's own scheduler is
    the single authority on dep order — preventing multiple build_tool.exe
    instances from racing on shared dep outputs during a parallel build  */

} build_context_t;

// =============================================================================
// --- Solution Descriptor ---
// =============================================================================

// Defines a Visual Studio solution, an array of targets from the target pool.
// This allows the build system to generate specialized workspaces such
// as "orb_build.sln" without polluting the full main engine workspace.

typedef struct
{
    // Name of the .sln file (e.g. "orb_make").
    const char*     name;

    // A NULL-terminated list of target names to include.
    // The generator looks up these names in g_targets[].
    const char**    target_names;

    // If non-NULL it will generate a "mega" source directory folder 
    // navigation project as "name"_nav" in the .sln file.
    // just a browesable directory that does not compile.
    const char*     nav_dir;

} solution_info_t;

// These are defined in build_tool_targets.c.

extern solution_info_t g_solutions[];
extern int             g_solution_count;

// =============================================================================
// --- Output Flags ---
// =============================================================================

// Bitfield controlling which sections of the build log are printed.
// Set once at startup via CLI flags (-q / -v / --out <hex>); read by all
// build_tool modules directly as g_out_flags. No need to thread it through
// build_context_t since output verbosity is a process-global setting.

typedef unsigned int out_flags_t;

// Compile-step sections — each bit enables one category of cl.exe output.
#define ORB_OUT_COMPILE_SUMMARY  ( 1u << 0  )  // [orb compile] target (config)
#define ORB_OUT_COMPILE_SOURCES  ( 1u << 1  )  // sources: <absolute paths>
#define ORB_OUT_COMPILE_FLAGS    ( 1u << 2  )  // flags:   /W4 /WX /Zi /Od ...
#define ORB_OUT_COMPILE_DEFINES  ( 1u << 3  )  // defines: OS_WINDOWS ARCH_X64 ...
#define ORB_OUT_COMPILE_INCLUDES ( 1u << 4  )  // includes: source gen_dir ...
#define ORB_OUT_COMPILE_OUTPUT   ( 1u << 5  )  // output:  obj=... pdb=...
#define ORB_OUT_COMPILE_CMD      ( 1u << 6  )  // raw cl.exe command line (long string)

// Link / archive-step sections.
#define ORB_OUT_LINK_SUMMARY     ( 1u << 7  )  // [orb link] target -> artifact
#define ORB_OUT_LINK_INPUTS      ( 1u << 8  )  // inputs:  objDir/*.obj
#define ORB_OUT_LINK_LIBS        ( 1u << 9  )  // libs:    dep.lib user32.lib ...
#define ORB_OUT_LINK_FLAGS       ( 1u << 10 )  // flags:   /nologo /DLL ...
#define ORB_OUT_LINK_OUTPUT      ( 1u << 11 )  // output:  bin/target.lib
#define ORB_OUT_LINK_PDB         ( 1u << 12 )  // pdb:     bin/target_xxx.pdb
#define ORB_OUT_LINK_CMD         ( 1u << 13 )  // raw link.exe / lib.exe command line

// General sections.
#define ORB_OUT_SCHEDULER        ( 1u << 14 )  // [orb parallel] N targets, M threads
#define ORB_OUT_TARGET_RESULT    ( 1u << 15 )  // [orb compiled] / [orb skipped] per-target result
#define ORB_OUT_REFLECT          ( 1u << 16 )  // [orb reflect] codegen steps
#define ORB_OUT_VCVARS           ( 1u << 17 )  // [orb vcvars] VS env discovery
#define ORB_OUT_MSVC_OUTPUT      ( 1u << 18 )  // [MSVC] raw cl/link/lib passthrough lines

// Convenience masks: any compile or link detail flag set.
#define ORB_OUT_ANY_COMPILE  ( ORB_OUT_COMPILE_SUMMARY  | ORB_OUT_COMPILE_SOURCES  | \
                               ORB_OUT_COMPILE_FLAGS    | ORB_OUT_COMPILE_DEFINES  | \
                               ORB_OUT_COMPILE_INCLUDES | ORB_OUT_COMPILE_OUTPUT   | \
                               ORB_OUT_COMPILE_CMD )

#define ORB_OUT_ANY_LINK     ( ORB_OUT_LINK_SUMMARY | ORB_OUT_LINK_INPUTS | \
                               ORB_OUT_LINK_LIBS    | ORB_OUT_LINK_FLAGS  | \
                               ORB_OUT_LINK_OUTPUT  | ORB_OUT_LINK_PDB   | \
                               ORB_OUT_LINK_CMD )

// Preset combinations — pass as --out <hex> or use -q / -v shorthands.
#define ORB_OUT_QUIET   ( ORB_OUT_TARGET_RESULT | ORB_OUT_SCHEDULER )
#define ORB_OUT_NORMAL  ( ORB_OUT_QUIET | ORB_OUT_COMPILE_SUMMARY | ORB_OUT_COMPILE_SOURCES | \
                          ORB_OUT_LINK_SUMMARY | ORB_OUT_REFLECT | ORB_OUT_VCVARS | ORB_OUT_MSVC_OUTPUT )

#define OBB_OUT_TESTING ( ORB_OUT_TARGET_RESULT | ORB_OUT_MSVC_OUTPUT | \
                          ORB_OUT_COMPILE_SUMMARY )

#define ORB_OUT_VERBOSE ( 0xFFFFFFFFu )
#define ORB_OUT_DEFAULT ( OBB_OUT_TESTING ) // ( ORB_OUT_NORMAL | ORB_OUT_REFLECT )

// Defined in build_tool.c; all other translation units read this directly.

extern out_flags_t g_out_flags;

// =============================================================================
// --- Orchestration API ---
// =============================================================================

// Everything below is the public surface for the unity-built build_tool.exe.
// Implementations live in the corresponding _cc / _utils / _vcvars / _sched
// translation units that build_tool.c #includes.

// Compiles all translation units for a target. Emits the cl.exe command line
// with /showIncludes so build_run_cmd_capture_deps can record the header set
// into <obj_dir>/_deps.txt for the next incremental check.
bool build_target_compile( build_context_t* ctx, target_info_t* target, const char* obj_dir, const char* gen_dir );

// Compiles a single source file with the target's full flag/define/include set.
// No /showIncludes, no link step, no dep tracking. CLI tool for targeted error
// checking (-file flag). file_path must be absolute.
bool build_target_compile_single( build_context_t* ctx, target_info_t* target,
                                  const char* obj_dir, const char* gen_dir, const char* file_path );

// Compiles all unity units for a target with no link step.
// Used by -compile-only (VS Ctrl+F7 via NMakeCompileFileCommandLine).
bool build_target_compile_only( build_context_t* ctx, target_info_t* target );

// Links or archives the target's objects into the final artifact: lib.exe
// for static libs, link.exe (with /DLL or as an exe) for the rest. PDB paths
// are rotated per-link so an attached debugger never collides with the
// linker over a held-open symbol file.
bool build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir );

// Locates the Visual Studio installation (via vswhere or hard-coded probes)
// and imports vcvarsall.bat's environment into THIS process via _putenv_s.
// Idempotent — fast-paths out if cl.exe is already on PATH (Dev Cmd Prompt
// or VS-launched terminal). One-time cost, ~2.5s; saves ~50s across a full
// rebuild that would otherwise pay vcvars-prefix overhead per cl invocation.
void build_setup_vc_env( void );

// Appends a formatted string to a command buffer.
void cmd_append( cmd_buf_t* b, const char* fmt, ... );

// If the command buffer is near the shell limit (or already truncated),
// spill everything after the first token (the tool exe name) to a response
// file at rsp_path and rewrite the buffer to "<exe> @<rsp_path>". Returns
// true if a response file was created.
bool cmd_spill_to_response_file( cmd_buf_t* b, const char* rsp_path );

// Returns the last modification time of a file. Returns 0 if not found.
__time64_t build_get_mtime( const char* path );

// Acquire a Windows named mutex scoped to a single target, blocking until
// granted. Used to serialize concurrent invocations of build_tool.exe that
// would otherwise both compile/link the same target's outputs. Returns an
// opaque handle that must be passed to build_unlock_target() — or NULL on
// failure (in which case the caller proceeds without locking).
void* build_lock_target( const char* target_name );

// Release a lock acquired by build_lock_target(). NULL is a safe no-op.
void  build_unlock_target( void* lock );

// Run a shell command via CreateProcess and return its exit code. cmd is
// wrapped with "cmd.exe /C" so shell builtins (del, for) and glob args
// (*.obj) keep working. Output is redirected to the per-thread log file if
// a parallel worker is active (see sched_log_path), otherwise inherits the
// parent's stdout/stderr.
int build_run_cmd( const char* cmd );

// Same as build_run_cmd but suppresses the "[cmd] ..." echo. Use for trivial
// housekeeping invocations (e.g. del/rd during clean) where the caller will
// print a single human-readable summary itself instead of one line per call.
int build_run_cmd_quiet( const char* cmd );

// Pipes the child's stdout+stderr back line-by-line through us. When
// deps_path is non-NULL, /showIncludes lines are parsed out and written
// there (system headers filtered); used for compile steps. When NULL, no
// deps file is written; used for link/lib steps. All non-deps lines are
// forwarded to the active sink (worker log or stdout) prefixed with [MSVC]
// when ORB_OUT_MSVC_OUTPUT is set, or silently dropped when not.
int build_run_cmd_capture_deps( const char* cmd, const char* deps_path );

// The core worker function. Handles recursive dependency resolution
// (unless ctx->skip_deps), per-target locking, the incremental-build
// timestamp check (artifact mtime vs. each unit, link dep, and recorded
// header), reflection codegen, then compile + link. Idempotent: a fully
// up-to-date target short-circuits before any cl.exe spawn.
// out_skipped may be NULL; when non-NULL it is set to true if the target was
// skipped because all artifacts were already up to date, false otherwise.
bool build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped );

// Parallel scheduler. Builds the transitive closure of `root` (or every
// target in g_targets[] if `root` is NULL) using up to `thread_count`
// concurrent workers. Each worker calls build_target() with skip_deps=true;
// the scheduler itself owns dep ordering. Returns true iff all targets
// finished successfully.
bool build_run_parallel( build_context_t* ctx, target_info_t* root, int thread_count );

// Deletes build artifacts. If target is non-NULL, only that target's artifacts
// are removed (bin/<name>.*, obj/<name>/). If NULL, a global wipe runs —
// is_tool executables are excluded so tools survive a full clean.
void build_clean( target_info_t* target );

// Generates all .sln and .vcxproj files defined in the Solution Registry.
// This maps our custom build system into the Visual Studio IDE.
void build_gen_projects( void );

// clang-format on
/*============================================================================================*/
#endif    // BUILD_TOOL_H