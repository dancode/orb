/*==============================================================================================

    build_tool.h -- Core abstractions for the custom ORB build system.

    This header defines the "schema" for the entire build system. It separates the 
    logic of *how* to build (build_tool.c) from the data of *what* to build 
    (build_tool_targets.c).

==============================================================================================*/
#ifndef BUILD_TOOL_H
#define BUILD_TOOL_H

#include <stdbool.h>

// --- Configuration ---

// Standard build configurations. These map to compiler optimization levels 
// and debug symbol generation.
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
typedef struct
{
    const char*   name;         // Unique name (e.g., "base", "core", "app").
    target_type_t type;         // Artifact type (Lib, DLL, Exe).
    const char*   root_dir;     // Base path for source files relative to project root.
    const char*   sln_folder;   // Virtual folder in the Visual Studio solution.
    const char*   units[ 16 ];  // Translation units (.c files) to compile.
    int           unit_count;
    const char*   deps[ 16 ];   // Names of other targets this target depends on.
    int           dep_count;

    // Reflection metadata: If true, the build tool runs build_reflect.exe 
    // on this target's root_dir before compilation.
    bool          has_reflect;
    const char*   reflect_name; // Base name for generated .c/.h files.

} target_info_t;

// --- Global Registry ---

// These are defined in build_tool_targets.c and used by the orchestrator.
extern target_info_t g_targets[];
extern int           g_target_count;

// --- Execution Context ---

// State passed through the build process to maintain consistency.
typedef struct
{
    config_t config;        // Selected build config.
    bool     is_monolithic; // Reserved: for building everything into one binary.
    bool     is_clang;      // If true, uses clang-cl instead of cl.

} build_context_t;

// --- Orchestration API ---

// Run a shell command and return the exit code. 
// Automatically handles Visual Studio environment (vcvarsall) if necessary.
int build_run_cmd( const char* cmd );

// The core worker function. Handles directory creation, reflection generation,
// compilation of all units, and the final link/archive step for a target.
bool build_target( build_context_t* ctx, target_info_t* target );

// Deletes build artifacts from bin/ and obj/. 
// Surgically avoids deleting the build_tool.exe itself to prevent locking.
void build_clean( void );

// Generates the .sln and .vcxproj files for Visual Studio.
// Enables "F5 debugging" and IDE navigation while keeping the build custom.
void build_gen_projects( void );

/*============================================================================================*/
#endif // BUILD_TOOL_H