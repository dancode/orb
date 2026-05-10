#ifndef BUILD_INVOKER_H
#define BUILD_INVOKER_H
/*==============================================================================================

    build_invoker.h : Run-time build invoker.

    Spawns CMake to compile module DLLs on demand. Designed for editors and sandbox
    hosts that want to rebuild a hot-reloadable module from inside the running
    process.

    Why CMake and not direct cl.exe / clang invocation
    ---------------------------------------------------
    The project's CMakeLists already encodes every flag, include path, define, and
    link dependency. Reproducing that in C would be a permanent maintenance tax.
    CMake exposes a generator-agnostic build interface:

        cmake --build <build_dir> --target <name> --config <Debug|Release>

    On Windows this dispatches to MSBuild or Ninja. On Linux/macOS to make or ninja.
    File globs (file(GLOB_RECURSE) in your CMakeLists) are re-evaluated as needed,
    so newly added .c files just get picked up.

    Recompile + reload flow
    -----------------------
    1. Press C → host calls dev_build_module("example", &r)
    2. cmake links a new example.dll into the build's bin/ directory
    3. Host calls mod_reload("example") on success — OR just lets the file-watch
       debounce in mod_check_reloads() trigger the reload automatically (200ms).

==============================================================================================*/
#include "orb.h"

typedef enum dev_build_config_t
{
    RT_BUILD_DEBUG   = 0,
    RT_BUILD_RELEASE = 1,
} dev_build_config_t;

typedef struct dev_build_settings_s
{
    const char*       build_dir;      /* absolute path to cmake build dir; NULL = auto-detect */
    const char*       cmake_path;     /* absolute path to cmake.exe; NULL = "cmake" on PATH    */
    dev_build_config_t config;         /* defaults to RT_BUILD_DEBUG                            */
    bool              capture_output; /* if true, fills result->log on each build              */

} dev_build_settings_t;

typedef struct dev_build_result_s
{
    bool success; /* exit_code == 0 */
    int  exit_code;
    f64  elapsed_seconds;
    int  log_len;
    char log[ 8 * 1024 ]; /* combined stdout+stderr if capture_output was set */

} dev_build_result_t;

/* Configure the invoker. Pass NULL for auto-detect defaults. */
bool dev_build_init( const dev_build_settings_t* settings );

/* Compile a single CMake target ("example", "render", "audio", ...). Result must be non-NULL.
   Returns true if cmake was launched (the build itself may still have failed — check
   result->success and result->log). */
bool dev_build_module( const char* target, dev_build_result_t* result );

/* Compile every default target (the cmake "all" target). */
bool dev_build_all( dev_build_result_t* result );

/* Last error message from any operation that returned false. */
const char* dev_build_last_error( void );

/*============================================================================================*/
#endif    // BUILD_INVOKER_H