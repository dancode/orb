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
    TARGET_HOST_GAME,
    TARGET_HOST_EDITOR,
    TARGET_HOST_SANDBOX,
    TARGET_HOST_TOOL,
    TARGET_COUNT

} target_t;

typedef struct
{
    config_t config;
    target_t target;
    bool     is_monolithic;
    bool     is_clang;

} build_context_t;

// --- Helper Functions ---

// Run a shell command and return the exit code.
int build_run_cmd( const char* cmd );

// Build a specific target using the given context.
bool build_target( build_context_t* ctx );

// Clean build artifacts.
void build_clean( void );

// Generate Visual Studio project files.
void build_gen_projects( void );

/*============================================================================================*/
#endif // BUILD_TOOL_H