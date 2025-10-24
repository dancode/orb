/*==============================================================================================

    engine_main.c

==============================================================================================*/

// #include <stdlib.h>
// #include <string.h>
// #include <stdio.h>

#include "orb.h"
#include "core/core.h"
#include "core/module_system.h"

/*============================================================================================*/

core_api_t*       g_api;
core_debug_api_t* g_debug_api;

/*============================================================================================*/

void
on_gamma_change( cvar_t* cv )
{
    g_api->log( "[CALLBACK] %s changed!\n", cvar_get_name( cv ) );
}

void
on_brightness_change( cvar_t* cv )
{
    g_api->log( "[CALLBACK] Brightness updated for %s\n", cvar_get_name( cv ) );
}

void
test_cvar()
{
    const char* values[]  = { "off", "low", "medium", "high" };

    int         cvar_size = sizeof( cvar_t );
    UNUSED( cvar_size );

    cvar_t* bvar = cvar_register_b( "engine_paused", "pause engine simulation", false, CVAR_ENGINE );
    cvar_t* ivar = cvar_register_i( "max_fps", "frame cap", 60, 20, 500, CVAR_ENGINE );
    cvar_t* fvar = cvar_register_f( "time_scale", "set execution speed", 1.0f, 0.1f, 5.0f, CVAR_ENGINE );
    cvar_t* svar = cvar_register_s( "shadows", "shadow quality", values, 4, 3, CVAR_ENGINE );
    cvar_t* wvar = cvar_register_w( "player_name", "online display name", "default name", 32, CVAR_USERINFO );
    cvar_t* rvar = cvar_register_r( "base_game", "game path", "base", CVAR_INIT );

    cvar_t* uvar_1   = cvar_register_u( "user_var_1", "1" );
    cvar_t* uvar_2   = cvar_register_u( "user_var_2", "2" );
    cvar_t* uvar_3   = cvar_register_u( "user_var_3", "3" );

    cvar_t* uvar_fix = cvar_register_i( "user_var_2", "a build in variable", 20, 0, 100, 0 );

    cvar_compact_string_pool();

    UNUSED( bvar );
    UNUSED( ivar );
    UNUSED( fvar );
    UNUSED( svar );
    UNUSED( wvar );
    UNUSED( rvar );
    UNUSED( uvar_1 );
    UNUSED( uvar_2 );
    UNUSED( uvar_3 );

    UNUSED( uvar_fix );

    const char* test_string = NULL;
    test_string             = cvar_get_string( svar );
    test_string             = cvar_get_string( wvar );
    test_string             = cvar_get_string( rvar );

    cvar_set_value( "engine_paused", "1" );
    cvar_set_value( "max_fps", "s120" );

    // test callbacks
    cvar_t* gamma = cvar_register_f( "r_gamma", "set display gamma", 2.2f, 1.0f, 3.0f, CVAR_RENDER );
    cvar_t* brightness = cvar_register_f( "r_brightness", "set display brightness", 1.0f, 0.5f, 2.0f, CVAR_RENDER );

    // TODO: implement get_module_id() to avoid hardcoding module id
    cvar_callback_register( gamma, on_gamma_change, 1 );
    cvar_callback_register( gamma, on_brightness_change, 1 );
    cvar_callback_register( brightness, on_brightness_change, 1 );

    cvar_callback_invoke( gamma );

    g_api->log( "Unloading module...\n" );
    cvar_callback_unregister_by_module( 1 );

    cvar_callback_invoke( gamma );    // Should now do nothing

    // cvar command test
    char* info_args[] = { "cvarinfo", "shadows" };
    cmd_cvarinfo( 2, info_args );

    // cvar_t* evar = g_api->cvar_find( "engine_paused" );
    if ( bvar )
    {
        const char* test_name = cvar_get_name( bvar );
        UNUSED( test_name );
        // printf( "Cvar: %s : %s\n", g_debug_api->get_cvar_name( evar ), g_debug_api->get_cvar_desc( evar )
        // printf( "Cvar: %s\n", (const char*)( g_debug_api->string_pool->data ) + evar->name );
    }

    // int cvar_data_size = sizeof( cvar_data_t );
    // UNUSED( cvar_data_size );

    // core_cvar_set_int( "r_draw_debug", 99 );
    // cvar_set_string( "player_name", "dancode!" );
}

/*============================================================================================*/

int
main( int argc, char** argv )
{
    ( void )argc;
    ( void )argv;

    core_init();

    struct module_t* game = module_load( "sample_game", "sample_game.dll" );
    if ( game == NULL )
    {
        return 1;
    }

    test_cvar();    // <-- test cvar system

    for ( int i = 0; i < 3; i++ )
    {
        module_call_tick( game );
    }

    module_reload( game );

    for ( int i = 0; i < 2; i++ )
    {
        module_call_tick( game );
    }

    module_unload( game );

    cvar_system_exit();
    return 0;
}

/*============================================================================================*/