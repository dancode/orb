/*==============================================================================================

    sys.c :

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "base/orb.h"
#include "sys_api.h"
#include "sys.h"

#include "engine/module/module_api.h"

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
#    include <timeapi.h>    // timeBeginPeriod

// #    include <process.h>     // _getpid
// #    include <sys/stat.h>    // _stat for file calls
// #    include <direct.h>      // directory handling. _mkdir

#else

#    define MAX_PATH 260
#    error "sys: platform not implemented"

#endif

/*==============================================================================================
    Platform code
==============================================================================================*/

#ifdef PLATFORM_WINDOWS

#    include "win/win_tick.c"
#    include "win/win_library.c"
#    include "win/win_file_watch.c"
#    include "win/win_file.c"
#    include "win/win_console_input.c"

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
platform_init( void* raw_state, get_api_fn get_api )
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
platform_on_reload( void* raw_state, get_api_fn get_api )
{
    platform_state_t* s = raw_state;
    ( void )get_api;
}

/*==============================================================================================
    Platform API
==============================================================================================*/

const sys_api_t g_sys_api_struct = {
    .tick_reset        = sys_tick_reset,
    .tick_seconds      = sys_tick_seconds,
    .tick_microseconds = sys_tick_microseconds,
    .tick_milliseconds = sys_tick_milliseconds,
    .tick_nanoseconds  = sys_tick_nanoseconds,
    .tick_sleep        = sys_tick_sleep,
};

module_api_t*
sys_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = sizeof( platform_state_t ),

        .deps       = { "core" },
        .dep_count  = 1,

        .func_api   = &g_sys_api_struct,

        .init       = platform_init,
        .tick       = platform_tick,
        .exit       = platform_exit,
        .on_reload  = platform_on_reload,
    };
    return &api;
}

void*
sys_get_api( void )
{
    return (void*)&g_sys_api_struct;
}

/*============================================================================================*/