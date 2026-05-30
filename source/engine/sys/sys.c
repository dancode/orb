/*==============================================================================================

    sys.c — Unity build entry point for the sys module.

    Includes platform-specific implementations and the platform-agnostic API wiring.
    Only one .c file from this module is passed to the compiler.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "orb.h"

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "engine/mod/mod_export.h" /* mod_desc_t, get_api_fn */
#include "engine/sys/sys_host.h"

/*==============================================================================================
    Platform includes
==============================================================================================*/

#if OS_WINDOWS

#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    define WIN32_EXTRA_LEAN
#    define VC_EXTRALEAN

#    pragma comment( lib, "winmm.lib" )    // timeBeginPeriod

#    include <windows.h>
#    include <timeapi.h> 

#else

#    define MAX_PATH 260
#    error "sys: platform not implemented"

#endif

#include "engine/sys/sys.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

#if OS_WINDOWS

#    include "win/win_tick.c"
#    include "win/win_library.c"
#    include "win/win_file_watch.c"
#    include "win/win_file.c"
#    include "win/win_thread.c"
#    include "win/win_thread_mutex.c"
#    include "win/win_thread_sema.c"
#    include "win/win_process.c"
#    include "win/win_sys.c"
#    include "win/win_memory.c"
#    include "win/win_console_input.c"

#endif

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "engine/sys/sys_api.c"


/*============================================================================================*/