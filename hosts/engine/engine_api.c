/*==============================================================================================

    engine_api.c

==============================================================================================*/
#include <stdio.h>

#include "base/orb.h"
#include "engine_api.h"

/*============================================================================================*/

void
engine_print( const char* fmt )
{
    printf( fmt );
}

/*============================================================================================*/
// internal state of the engine

static bool g_engine_running = true;

static int
engine_should_quit( void )
{
    return !g_engine_running;
}

static void
engine_request_quit( void )
{
    // Function to actually trigger the quit (call this when ESC is pressed, etc.)
    g_engine_running = false;
}

/*============================================================================================*/

const engine_api_t g_engine_api_struct = {
    .print       = engine_print,
    .should_quit = engine_should_quit,
};

/*============================================================================================*/

module_api_t*
engine_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = 0,
        .deps       = { "core" },
        .dep_count  = 1,
        .func_api   = &g_engine_api_struct,
        .init       = NULL,
        .tick       = NULL,
        .exit       = NULL,
        .on_reload  = NULL,
    };
    return &api;
}

void*
engine_get_api( void )
{
    return ( void* )&g_engine_api_struct;
}

/*============================================================================================*/
