/*==============================================================================================

    sample_game.c

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "core/core.h"
#include "sample_game.h"

/*==============================================================================================

    dll api

==============================================================================================*/

core_api_t*       g_api       = NULL;
core_debug_api_t* g_debug_api = NULL;

static int32_t g_draw_debug          = 10;
static char*   g_player_name         = "my_name";

API_EXPORT void
module_init( core_api_t* api )
{
    // printf( "[game] init\n" );

    g_api = api;
    g_api->log( "[game] init" );

    g_debug_api = api->debug_api;

    // Register our cvar, binding registry to &g_draw_debug
    // g_api->cvar_register( "r_draw_debug", CVAR_INT, &g_draw_debug );
    // g_api->cvar_register( "player_name", CVAR_STRING, &g_player_name );

    // g_api->log( "[game] init: r_draw_debug = %d", g_draw_debug );
}

API_EXPORT void
module_tick( void )
{
    // printf( "[game] tick\n" );

    if ( g_draw_debug )
    {
        g_api->log( "[game] drawing debug stuff" );
    }

    cvar_t* evar = g_api->cvar_find( "engine_paused" );
    // g_api->log( "%s",  )
        
    if ( evar )
    {
        // printf( "Cvar: %s : %s\n", g_debug_api->get_cvar_name( evar ), g_debug_api->get_cvar_desc( evar ) );
    }

    if ( evar )
    {
        // printf( "Cvar: %s\n", (const char*)( g_debug_api->string_pool->data + evar->name ) );
    }

    // Slow path — registry by name
    // int dd = g_api->cvar_get_int( "r_draw_debug" );
    // if ( dd )
    // {
    //     g_api->log( "[game] registry also sees r_draw_debug = %d", dd );
    // }

    // const char* var_string = g_api->cvar_get_string( "player_name" );
    // if ( var_string )
    // {
    //     g_api->log( "[game] registry also sees debug_string = %s", var_string );
    // }

    // cvar_value_t val = g_api->cvar_get( "r_draw_debug" );
    // if ( val )
    // {
    //     g_api->log( "[game] registry also sees r_draw_debug = %d", val );
    // }
}

API_EXPORT void
module_exit( void )
{
    // printf( "[game] shutdown\n" );

    g_api->log( "[game] shutdown" );
}

/*==============================================================================================

    main

==============================================================================================*/

int
main( int argc, char** argv )
{
    (void)argc;
    (void)argv;


    return 1;
}

/*============================================================================================*/