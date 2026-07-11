/*==============================================================================================

    sys.c — Unity build entry point for the sys module.

    Includes platform-specific implementations and the platform-agnostic API wiring.
    Only one .c file from this module is passed to the compiler.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#    pragma comment( lib, "winmm.lib" )     // timeBeginPeriod
#    pragma comment( lib, "advapi32.lib" )  // Reg* font-name lookup (win_font.c)
#    pragma comment( lib, "ws2_32.lib" )    // Winsock2 (win_socket.c)

#    include <winsock2.h>                   // must precede windows.h
#    include <ws2tcpip.h>
#    include <mstcpip.h>                    // SIO_UDP_CONNRESET (win_socket.c)
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
#    include "win/win_font.c"
#    include "win/win_thread.c"
#    include "win/win_thread_mutex.c"
#    include "win/win_thread_sema.c"
#    include "win/win_thread_atomic.c"
#    include "win/win_process.c"
#    include "win/win_sys.c"
#    include "win/win_memory.c"
#    include "win/win_console_input.c"
#    include "win/win_socket.c"

#endif

/*==============================================================================================
    Engine headers
==============================================================================================*/

#ifndef SYS_API_C_PRELUDE
#include "engine/sys/sys_api.c"
#endif 


/*============================================================================================*/