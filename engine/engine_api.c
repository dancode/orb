/*==============================================================================================

    engine_api.c

==============================================================================================*/
#include <stdio.h>
#include "orb.h"
#include "engine_api.h"

#include "module/module_api.h"

/*============================================================================================*/

void
engine_print( const char* fmt )
{
    printf( fmt );
}

/*============================================================================================*/
// internal state of the engine

static bool g_engine_running = true;

int
engine_should_quit( void )
{
    return !g_engine_running;
}

void
engine_request_quit( void )
{
    // Function to actually trigger the quit (call this when ESC is pressed, etc.)
    g_engine_running = false;
}

/*============================================================================================*/

static module_api_t g_module_api = {
    .version    = 1,
    .state_size = 0,
    .deps       = { "core" },
    .dep_count  = 1,

    .init       = NULL,
    .tick       = NULL,
    .exit       = NULL,
    .on_reload  = NULL,
};

module_api_t*
engine_get_module_api( void )
{
    return &g_module_api;
}

/*============================================================================================*/

static engine_api_t g_engine_api = {

    .print = engine_print,
    .should_quit = engine_should_quit,
};

/*============================================================================================*/

engine_api_t*
engine_get_api( void )
{
    return &g_engine_api;
}

/*============================================================================================*/
