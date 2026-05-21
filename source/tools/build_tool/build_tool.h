/*==============================================================================================

    build_tool.h -- Core abstractions for the custom ORB build system.

    This header defines the "schema" for the entire build system. It separates the 
    logic of *how* to build (build_tool.c) from the data of *what* to build 
    (build_tool_targets.c).

    Architectural Goals:
    - High Performance: Minimal filesystem overhead and direct tool invocation.
    - Simplicity: A flat, unified target pool with explicit dependencies.
    - Flexibility: Support for multiple standalone IDE solutions sharing common targets.
    - Automation: Recursive dependency resolution and automatic tool bootstrapping.

==============================================================================================*/
#ifndef BUILD_TOOL_H
#define BUILD_TOOL_H

#include <stdbool.h>
#include <time.h>

// --- Project Constants ---

#define CMD_BUF_MAX 65536  // Max size for any single compiler/linker command line.

// --- Helper Types ---

typedef struct
{
    char   buf[ CMD_BUF_MAX ];
    size_t size;
    bool   truncated;   // Set by cmd_append() when an append could not fit.

} cmd_buf_t;

// Safe threshold below cmd.exe's 8191-char command line limit. Leaves room
// for the vcvars prefix that build_run_cmd() prepends to compiler calls.
#define CMD_RSP_THRESHOLD 7000

// --- Configuration ---
// ... (rest of the file remains same but I need to provide it all or a good chunk)

// Standard build configurations. These map to compiler optimization levels 
// and debug symbol generation. The enum values are used to index configuration
// specific settings in the orchestrator.
typedef enum
{
    CONFIG_DEBUG,   // No optimizations, full debug symbols, MDd runtime.
    CONFIG_RELEASE, // Full optimizations, minimal debug symbols, MD runtime.
    CONFIG_COUNT

} config_t;

// --- Target Types ---

// Defines the output artifact type. This dictates which tool (cl, link, lib)
// is used in the final phase of building a target.
typedef enum
{
    TARGET_STATIC_LIB,  // Compiles to a .lib archive via lib.exe.
    TARGET_DYNAMIC_LIB, // Compiles to a .dll via link.exe /DLL.
    TARGET_EXECUTABLE   // Compiles to a .exe via link.exe.

} target_type_t;

// --- Target Descriptor ---

// A target_info_t represents a single buildable unit in the ORB ecosystem.
// It contains all metadata required to compile and link the target.
// Targets are shared across different IDE solutions.
typedef struct
{
    const char*   name;             // Unique name (e.g., "base", "core", "app").
    target_type_t type;             // Artifact type (Lib, DLL, Exe).
    const char*   root_dir;         // Base path for source files relative to project root.
    const char*   sln_folder;       // Virtual folder in the Visual Studio solution.
    const char*   units[ 16 ];      // Translation units (.c files) to compile.
    int           unit_count;
    
    // Link Dependencies: Other targets that produce .libs this target must link against.
    const char*   deps[ 16 ];   
    int           dep_count;

    // Tool Dependencies: Standalone utilities that must exist to build this target.
    // These are built recursively but NOT linked into the final binary.
    const char*   tool_deps[ 16 ]; 
    int           tool_dep_count;

    // Reflection metadata: If true, the build tool runs build_reflect.exe 
    // on this target's root_dir before compilation.
    bool          has_reflect;
    const char*   reflect_name;     // Base name for generated .c/.h files.

} target_info_t;

// --- Global Registry ---

// These are defined in build_tool_targets.c and used by the orchestrator.
extern target_info_t g_targets[];
extern int           g_target_count;

// --- Execution Context ---

// State passed through the build process to maintain consistency.
typedef struct
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

// --- Orchestration API ---

// ... (other declarations)

// Compiles all translation units for a target.
bool build_target_compile( build_context_t* ctx, target_info_t* target, const char* obj_dir, const char* gen_dir );

// Links or archives the target's objects into the final artifact.
bool build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir );

// Locates the Visual Studio installation and prepares the environment.
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

// Run a shell command and return the exit code.
// Automatically handles Visual Studio environment (vcvarsall) if necessary.
int build_run_cmd( const char* cmd );

// Like build_run_cmd, but captures /showIncludes output to deps_path.
// Non-include lines are forwarded to stdout. See build_tool_vcvars.c.
int build_run_cmd_capture_deps( const char* cmd, const char* deps_path );

// The core worker function. Handles recursive dependency resolution,
// incremental build timestamp checks, reflection generation,
// and the final compile/link steps for a target.
bool build_target( build_context_t* ctx, target_info_t* target );

// Parallel scheduler. Builds the transitive closure of `root` (or every
// target in g_targets[] if `root` is NULL) using up to `thread_count`
// concurrent workers. Each worker calls build_target() with skip_deps=true;
// the scheduler itself owns dep ordering. Returns true iff all targets
// finished successfully.
bool build_run_parallel( build_context_t* ctx, target_info_t* root, int thread_count );

// Deletes build artifacts from bin/ and obj/. 
// Surgically avoids deleting the build_tool.exe itself to prevent locking.
void build_clean( void );

// Generates all .sln and .vcxproj files defined in the Solution Registry.
// This maps our custom build system into the Visual Studio IDE.
void build_gen_projects( void );

/*============================================================================================*/
#endif // BUILD_TOOL_H