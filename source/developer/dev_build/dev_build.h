#ifndef DEV_BUILD_H
#define DEV_BUILD_H
/*==============================================================================================

    dev_build.h : Run-time build invoker for build_tool.exe-based projects.

    Spawns build_tool.exe to compile module DLLs on demand. Designed for editors
    and sandbox hosts that want to rebuild a hot-reloadable module from inside the
    running process.

    It has one job: spawn a process, capture its output, and return a result.

    - Its only engine dependency is sys for the process call.
    - It knows nothing about modules, state, or reload semantics (see: dev_hot)
    - Acts as a build_tool shim; can be used anywhere a process spawn is useful.
    - A future tool could trigger a build without any module system present at all.


    Command format
    --------------
        build_tool.exe -config <Debug|Release> [-target <name>]

    build_tool.exe is auto-located at <build_dir>\bin\build_tool.exe unless an
    explicit path is provided in dev_build_settings_t.build_tool_path.

    Recompile + reload flow
    -----------------------
    1. Press C -> host calls dev_build_module( "example", &r )
    2. build_tool links a new example.dll into bin/
    3. Host calls mod_reload("example") on success -- OR just lets the file-watch
       debounce in mod_check_reloads() trigger the reload automatically (200ms).

==============================================================================================*/
#include "orb.h"

typedef enum dev_build_config_t
{
    DEV_BUILD_DEBUG   = 0,
    DEV_BUILD_RELEASE = 1,

} dev_build_config_t;

typedef struct dev_build_settings_s
{
    const char*        build_dir;       /* absolute path to the repo root; NULL = auto-detect      */
    const char*        build_tool_path; /* absolute path to build_tool.exe; NULL = auto-locate     */
    dev_build_config_t config;          /* defaults to DEV_BUILD_DEBUG                             */
    bool               capture_output;  /* if true, fills result->log on each build                */

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

/* Compile a single target ("example", "render", "audio", ...). Result must be non-NULL.
   Returns true if build_tool was launched (the build itself may still have failed -- check
   result->success and result->log). */
bool dev_build_module( const char* target, dev_build_result_t* result );

/* Compile every default target. */
bool dev_build_all( dev_build_result_t* result );

/* Last error message from any operation that returned false. */
const char* dev_build_last_error( void );

/*============================================================================================*/
#endif    // DEV_BUILD_H
