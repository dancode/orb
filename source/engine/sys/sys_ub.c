/*==============================================================================================

    sys_ub.c

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "orb.h"
#include "engine/sys/sys.h"
#include "engine/sys/sys_api.h"

#include "engine/mod/mod_api.h"

/*==============================================================================================
    platform depdendencies
==============================================================================================*/

#if OS_WINDOWS

#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    define WIN32_EXTRA_LEAN
#    define VC_EXTRALEAN

#    pragma comment( lib, "winmm.lib" )    // timeBeginPeriod

#    include <windows.h>    // required for all windows applications.
#    include <timeapi.h>    // timeBeginPeriod

// #    include <process.h>     // _getpid
// #    include <sys/stat.h>    // _stat for file calls
// #    include <direct.h>      // directory handling. _mkdir

#else

#    define MAX_PATH 260
#    error "sys: platform not implemented"

#endif

/*==============================================================================================
    unity build
==============================================================================================*/

#if OS_WINDOWS

#    include "win/win_tick.c"
#    include "win/win_library.c"
#    include "win/win_file_watch.c"
#    include "win/win_file.c"
#    include "win/win_console_input.c"

#endif

#include "engine/sys/sys_api.c"

/*============================================================================================*/