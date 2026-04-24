/*==============================================================================================

    engine_api.c

==============================================================================================*/
#include <stdio.h>
#include "orb.h"
#include "engine_api.h"

/*============================================================================================*/

void
engine_print( const char* fmt )
{
    printf( fmt );
}

/*============================================================================================*/

// Internal state of the engine
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

static engine_api_t g_engine_api_internal = {

    .print = engine_print,
    .should_quit = engine_should_quit,
};

/*============================================================================================*/
/* exported api */

engine_api_t*
engine_get_api( void )
{
    return &g_engine_api_internal;
}

/*============================================================================================*/

static engine_api_t*       g_engine_api;

void
engine_api_init( void )
{
    g_engine_api = engine_get_api();
}

void
engine_api_exit( void )
{
    g_engine_api = NULL;
}


/*============================================================================================*/
