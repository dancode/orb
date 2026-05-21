/*==============================================================================================

    build_tool.h -- Core abstractions for the custom ORB build system.

==============================================================================================*/
#ifndef BUILD_TOOL_H
#define BUILD_TOOL_H

#include <stdbool.h>

typedef enum
{
    CONFIG_DEBUG,
    CONFIG_RELEASE,
    CONFIG_COUNT

} config_t;

typedef enum
{
    TARGET_STATIC_LIB,
    TARGET_DYNAMIC_LIB,
    TARGET_EXECUTABLE

} target_type_t;

typedef struct
{
    const char*   name;
    target_type_t type;
    const char*   root_dir;
    const char*   sln_folder;
    const char*   units[ 16 ];
    int           unit_count;

} target_info_t;

// --- Target Registry ---
extern target_info_t g_targets[];
extern int           g_target_count;

typedef struct
{
    config_t config;
    bool     is_monolithic;
    bool     is_clang;

} build_context_t;

// --- Helper Functions ---

// Run a shell command and return the exit code.
int build_run_cmd( const char* cmd );

// Build a specific target using the given context.
bool build_target( build_context_t* ctx, target_info_t* target );

// Clean build artifacts.
void build_clean( void );

// Generate Visual Studio project files.
void build_gen_projects( void );

/*============================================================================================*/
#endif // BUILD_TOOL_H