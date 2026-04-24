/*==============================================================================================

    platform.c

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "orb.h"
#include "platform.h"

// platform relies on core, sits above it in dependency stack
#include "core/module_get_api.h"
#include "core/module/module_api.h"

/*==============================================================================================
    Platform layer
==============================================================================================*/

#ifdef PLATFORM_WINDOWS

#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    define WIN32_EXTRA_LEAN
#    define VC_EXTRALEAN

#    pragma comment( lib, "winmm.lib" )    // timeBeginPeriod

#    include <windows.h>    // required for all windows applications.
#    include <stdlib.h>      // __argc and __argv
#    include <timeapi.h>    // timeBeginPeriod
// #    include <process.h>     // _getpid
// #    include <sys/stat.h>    // _stat for file calls
// #    include <direct.h>      // directory handling. _mkdir

#else

#    define MAX_PATH 260
#    error "module_sys: platform not implemented"

#endif

/*==============================================================================================
    Platform code
==============================================================================================*/

#ifdef PLATFORM_WINDOWS
#    include "win/win_tick.c"
#endif

/*==============================================================================================
    Platform state
==============================================================================================*/

#define STATE_SENTINEL 0xC0FFEE /* written on first init; lets us detect reloads */

typedef struct platform_state_s
{
    uint32_t sentinel; /* first-load detection                           */
    int32_t  window_count;

} platform_state_t;

/*==============================================================================================
    Platform Lifecycle
==============================================================================================*/

bool
platform_init( void* raw_state, module_sys_api_t* sys )
{
    // printf( "[game] init\n" );

    platform_state_t* s = raw_state;

    /* first load */
    if ( s->sentinel != STATE_SENTINEL )
    {
        s->sentinel = STATE_SENTINEL;
    }

    return true;
}

void
platform_tick( void* raw_state, float dt )
{
    platform_state_t* s = raw_state;
}

void
platform_exit( void* raw_state )
{
    platform_state_t* s = raw_state;
}

static void
platform_on_reload( void* raw_state, module_sys_api_t* sys )
{
    platform_state_t* s = raw_state;
    ( void )sys;
}

/*==============================================================================================
    Platform API
==============================================================================================*/

static platform_api_t g_platform_api = {
    .tick_reset        = sys_tick_reset,
    .tick_seconds      = sys_tick_seconds,
    .tick_microseconds = sys_tick_microseconds,
    .tick_milliseconds = sys_tick_milliseconds,
    .tick_nanoseconds  = sys_tick_nanoseconds,
    .tick_sleep        = sys_tick_sleep,
};

platform_api_t*
platform_get_api( void )
{
    return &g_platform_api;
}

/*==============================================================================================
    Required DLL exports (C linkage)
==============================================================================================*/


/*==============================================================================================
    Platform module
==============================================================================================*/

static module_api_t g_platform_module_api = {
    .version    = 1,
    .state_size = sizeof( platform_state_t ),

    .deps       = { "core" },
    .dep_count  = 1,

    .init       = platform_init,
    .tick       = platform_tick,
    .exit       = platform_exit,
    .on_reload  = platform_on_reload,
};

// static module_api_t*
// get_platform_module_api( void )
// {
//     return &g_platform_module_api;
// }

static void*
get_platform_api( void )
{
    return &g_platform_api;
}

/* public */ void
platform_module_register( void )
{
    // module_register_static( "platform", &g_platform_module_api, get_platform_api() );
}


/*============================================================================================*/