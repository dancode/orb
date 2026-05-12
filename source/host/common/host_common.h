/*==============================================================================================

    host/common/host_common.h — Pre-runtime host setup.

    Runs before the engine exists. Pure stdlib — no mod, sys, or core involvement.

    Parses argc/argv into a flat launch_params_t struct that the host uses to
    configure the runtime before calling runtime_host_run().

    Recognised flags
    ----------------
        -module  <name>   Override the default module to load (sandbox_module use-case).
        -project <path>   Set the project root path.
        -dev              Enable developer features (hot-reload, verbose logging).

==============================================================================================*/
#ifndef HOST_COMMON_H
#define HOST_COMMON_H

#include "orb.h"

/*==============================================================================================
    Constants
==============================================================================================*/

#define HOST_NAME_MAX   64
#define HOST_MODULE_MAX 64
#define HOST_PATH_MAX   260

/*==============================================================================================
    Launch Parameters
==============================================================================================*/

typedef struct launch_params_s
{
    char host_name[ HOST_NAME_MAX ];         /* set by the host before calling parse; not from argv */
    char module_override[ HOST_MODULE_MAX ]; /* -module <name>, or "" if not specified */
    char project_path[ HOST_PATH_MAX ];      /* -project <path>, or "" to use cwd */
    bool dev_mode;                           /* -dev flag present */

} launch_params_t;

/*==============================================================================================
    API
==============================================================================================*/

/* Parse argc/argv into *out. Always succeeds — unknown args are silently ignored.
 *out is zeroed first so every field has a safe default before flag processing. */

void host_args_parse( int argc, char** argv, launch_params_t* out );

/*============================================================================================*/
#endif
