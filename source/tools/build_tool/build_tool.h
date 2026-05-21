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

==============================================================================================*/
#ifndef BUILD_TOOL_H
#define BUILD_TOOL_H

// clang-format off

#include <stdbool.h>
#include <time.h>

// --- Project Constants ---

// Max size for any single compiler/linker command line.

#define CMD_BUF_MAX 16384    

// Path buffer size for every filesystem path the build tool constructs.
// Windows MAX_PATH is 260; 512 gives generous headroom for composite paths.
// <obj_dir>\<filename> without forcing us to opt into long-path support

#define BT_PATH_MAX 512

// --- Helper Types ---

// Reusable command-line buffer for assembling cl.exe / link.exe / lib.exe
// invocations. cmd_append() formats into `buf` and updates `size`; if an
// append cannot fit, `truncated` is set so the caller can decide whether to
// spill the tail into a response file (see cmd_spill_to_response_file).

typedef struct
{
    char   buf[ CMD_BUF_MAX ];
    size_t size;
    bool   truncated; // Set by cmd_append() when an append could not fit.

} cmd_buf_t;

// Safe threshold below cmd.exe's 8191-char command line limit. Leaves room
// for the vcvars prefix that build_run_cmd() prepends to compiler calls.

#define CMD_RSP_THRESHOLD 7000

// --- Configuration ---

// Standard build configurations. These map to compiler optimization levels
// and debug symbol generation. The enum values are used to index configuration
// specific settings in the orchestrator 

typedef enum
{
    CONFIG_DEBUG,   // No optimizations, full debug symbols, MDd runtime.
    CONFIG_RELEASE, // Full optimizations, minimal debug symbols, MD runtime.
    CONFIG_COUNT

} config_t;

// --- Target Types ---

// Defines the artifact type, determining which toolchain (cl, link, lib)
// is used in the final phase of building a target.

typedef enum
{
    TARGET_STATIC_LIB,  // Compiles to a .lib archive via lib.exe.
    TARGET_DYNAMIC_LIB, // Compiles to a .dll via link.exe /DLL.
    TARGET_EXECUTABLE   // Compiles to a .exe via link.exe.

} target_type_t;

// --- Target Descriptor ---

// Represents a single buildable unit in the ORB ecosystem. It contains all
// metadata required to compile and link the target. 

// Targets are pooled in g_targets[] (see build_tool_targets.c).
// Shared across every IDE solution that references them by name. 

typedef struct target_info_s
{
    const char*   name;             // Unique name (e.g., "base", "core", "app").
    target_type_t type;             // Artifact type (LIB, DLL, or EXE).
    const char*   root_dir;         // Base path for source files relative to project root.
    const char*   sln_folder;       // Virtual folder in the Visual Studio solution.

    // Translation Units (Unity Build Fragments)
    // Each entry is typically an umbrella .c file that includes other sources.
    // Multiple units allow the scheduler to parallelize cl.exe calls.
    const char*   units[ 16 ];

    // Link Dependencies: Other targets that produce .libs this target must link against.
    // Drives both the linker's input list and the parallel scheduler's topological order.
    const char*   deps[ 16 ];

    // Tool Dependencies: Standalone utilities that must exist to build this target.
    // These are built recursively but NOT linked into the final binary. 
    // Ex: any target with 'has_reflect' depends on build_reflect.exe as a tool dep.
    const char*   tool_deps[ 16 ];

    // Reflection metadata: if true, build_reflect.exe is invoked on root_dir
    // before compilation. Generated files land in <build_dir>/generated/ and
    // are appended to the cl.exe command line for this target.
    bool          has_reflect;

    // Base name for generated .c/.h files. Default is NULL, which means the
    // files are named after the target (e.g. "core" -> "core.generated.c/h").
    const char*   reflect_name;

    // If true, this is a build-time tool executable (e.g. build_reflect).
    // Tool targets survive global clean and are always rebuilt by our own
    // dep resolution — never delegated to VS ProjectDependencies.
    bool          is_tool;

    // If true, this is the reflection code-generator tool. Targets with
    // has_reflect=true automatically depend on whichever target carries
    // this flag — no hardcoded name needed anywhere in the build logic.
    bool          is_reflect_tool;

} target_info_t;

// --- Global Registry ---

// The list of all targets defined in build_tool_targets.c and used by
// the orchestrator and solution generator.

extern target_info_t g_targets[];
extern int           g_target_count;

// --- Execution Context ---

// State passed through the build process to maintain consistency.
typedef struct build_context_s
{
    config_t config;        // Selected build config (Debug/Release).
    bool     is_monolithic; // Reserved: for building everything into one binary.
    bool     is_clang;      // If true, uses clang-cl instead of cl.
    bool     skip_deps;     // If true, build_target() does NOT recurse into
                            // its dependencies. The VS solution generator
                            // emits this flag so MSBuild's own scheduler is
                            // the single authority on dep order — preventing
                            // multiple build_tool.exe instances from racing
                            // on shared dep outputs during a parallel build.

} build_context_t;

// --- Solution Descriptor ---

// Defines a Visual Studio solution and which targets from the pool it contains.
// This allows the build system to generate specialized workspaces (e.g. orb_build.sln)
// without polluting the main engine workspace.
typedef struct
{
    const char*  name;         // Name of the .sln file (e.g. "orb_make").
    const char** target_names; // NULL-terminated list of target names to include.
    const char*  nav_dir;      // If non-NULL, generate a "Mega" navigation project for this directory.

} solution_info_t;

// These are defined in build_tool_targets.c.
extern solution_info_t g_solutions[];
extern int             g_solution_count;

// =============================================================================
// --- Orchestration API -------------------------------------------------------
// =============================================================================

// Everything below is the public surface for the unity-built build_tool.exe.
// Implementations live in the corresponding _cc / _utils / _vcvars / _sched
// translation units that build_tool.c #includes.

// Compiles all translation units for a target. Emits the cl.exe command line
// with /showIncludes so build_run_cmd_capture_deps can record the header set
// into <obj_dir>/_deps.txt for the next incremental check.
bool build_target_compile( build_context_t* ctx, target_info_t* target, const char* obj_dir, const char* gen_dir );

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

// Like build_run_cmd, but pipes the child's stdout+stderr back through us
// so /showIncludes lines can be parsed out and written to deps_path. All
// other lines are forwarded to the active sink (worker log or stdout).
// System headers from the VC toolchain and Windows SDK are filtered from
// the deps file (they cannot be invalidated by project edits).
int build_run_cmd_capture_deps( const char* cmd, const char* deps_path );

// The core worker function. Handles recursive dependency resolution
// (unless ctx->skip_deps), per-target locking, the incremental-build
// timestamp check (artifact mtime vs. each unit, link dep, and recorded
// header), reflection codegen, then compile + link. Idempotent: a fully
// up-to-date target short-circuits before any cl.exe spawn.
bool build_target( build_context_t* ctx, target_info_t* target );

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
#endif // BUILD_TOOL_H