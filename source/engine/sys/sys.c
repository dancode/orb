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

/*==============================================================================================
    Unity build
==============================================================================================*/


#include "engine/sys/sys_api.h"
#include "engine/sys/sys.h"

#if OS_WINDOWS

#    include "win/win_tick.c"
#    include "win/win_library.c"
#    include "win/win_file_watch.c"
#    include "win/win_file.c"
#    include "win/win_thread.c"
#    include "win/win_mutex.c"
#    include "win/win_process.c"
#    include "win/win_console_input.c"

#endif

#include "engine/sys/sys_api.c"

/*============================================================================================*/