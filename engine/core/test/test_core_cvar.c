/*============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "core/core.h"

extern core_api_t*       g_api;
extern core_debug_api_t* g_debug_api;

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
test_core_cvar_old( void )
{
    int cvar_type_size = sizeof( cvar_type_t );
    UNUSED( cvar_type_size );

    int cvar_size = sizeof( cvar_t );
    UNUSED( cvar_size );

    /**************************************************************/

    const char* values[] = { "off", "low", "medium", "high" };

    /**************************************************************/

    cvar_t* bvar = cvar_register_b( "engine_paused", "pause engine simulation", false, CVAR_NONE );
    cvar_t* ivar = cvar_register_i( "max_fps", "frame cap", 60, 20, 500, CVAR_NONE );
    cvar_t* fvar = cvar_register_f( "time_scale", "set execution speed", 1.0f, 0.1f, 5.0f, CVAR_NONE );
    cvar_t* svar = cvar_register_s( "shadows", "shadow quality", values, 4, 3, CVAR_NONE );
    cvar_t* wvar = cvar_register_w( "player_name", "online display name", "default name", 32, CVAR_USERINFO );
    cvar_t* rvar = cvar_register_r( "base_game", "game path", "base", CVAR_INIT );

    cvar_t* uvar_1   = cvar_register_u( "user_var_1", "1" );
    cvar_t* uvar_2   = cvar_register_u( "user_var_2", "2" );
    cvar_t* uvar_3   = cvar_register_u( "user_var_3", "3" );

    cvar_t* uvar_fix = cvar_register_i( "user_var_2", "a build in variable", 20, 0, 100, 0 );

    cvar_compact_user_pool();

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

    /**************************************************************/

    const char* test_string = NULL;
    test_string             = cvar_get_string( svar );
    test_string             = cvar_get_string( wvar );
    test_string             = cvar_get_string( rvar );

    cvar_set_value( "engine_paused", "1" );
    cvar_set_value( "max_fps", "s120" );

    /**************************************************************/
    // test callbacks

    cvar_t* gamma = cvar_register_f( "r_gamma", "set display gamma", 2.2f, 1.0f, 3.0f, CVAR_NONE );
    cvar_t* brightness = cvar_register_f( "r_brightness", "set display brightness", 1.0f, 0.5f, 2.0f, CVAR_NONE );


    // TODO: implement get_module_id() to avoid hardcoding module id
    cvar_callback_register( gamma, on_gamma_change, 1 );
    cvar_callback_register( gamma, on_brightness_change, 1 );
    cvar_callback_register( brightness, on_brightness_change, 1 );

    cvar_callback_invoke( gamma );

    g_api->log( "Unloading module...\n" );
    cvar_callback_unregister_by_module( 1 );

    cvar_callback_invoke( gamma );    // Should now do nothing

    /**************************************************************/
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

    /**************************************************************/
    // test config write / exec

    cvar_write_config( "test_config.cfg", CVAR_ALL );
    cvar_exec_config( "test_config.cfg" );

    /**************************************************************/
}

/*==============================================================================================
    Example 1: Basic CVar Registration and Usage
==============================================================================================*/

void
example_basic_usage( void )
{
    printf( "\n=== Example 1: Basic Usage ===\n\n" );

    /* Initialize the cvar system */

    cvar_system_init();

    const char* quality_options[] = { "low", "medium", "high", "ultra" };

    /* Register various cvar types */
    cvar_t* cv_debug = cvar_register_b( "debug", "Enable debug mode", false, CVAR_ARCHIVE );
    cvar_t* cv_maxfps = cvar_register_i( "com_maxfps", "Maximum frames per second", 60, 30, 300, CVAR_ARCHIVE );
    cvar_t* cv_volume = cvar_register_f( "s_volume", "Sound volume", 0.8f, 0.0f, 1.0f, CVAR_ARCHIVE );
    cvar_t* cv_quality = cvar_register_s( "r_quality", "Rendering quality", quality_options, 4, 2, CVAR_ARCHIVE );
    cvar_t* cv_version = cvar_register_r( "version", "Engine version", "1.0.0", CVAR_ROM );

    /* Access cvar values */
    bool        debug   = cvar_get_bool( cv_debug );    // bool stored as int
    i32         maxfps  = cvar_get_int( cv_maxfps );
    f32         volume  = cvar_get_float( cv_volume );
    const char* quality = cvar_get_string( cv_quality );
    const char* version = cvar_get_string( cv_version );

    printf( "debug: %d\n", debug );
    printf( "com_maxfps: %d\n", maxfps );
    printf( "s_volume: %.2f\n", volume );
    printf( "r_quality: %s\n", quality );
    printf( "version: %s\n", version );

    /* Cleanup */
    cvar_system_exit();
}

/*==============================================================================================
    Example 2: Modifying CVars at Runtime
==============================================================================================*/

void
example_runtime_modification( void )
{
    printf( "\n=== Example 2: Runtime Modification ===\n\n" );

    cvar_system_init();

    /* Register a cvar */
    cvar_register_i( "test_value", "Test integer", 100, 0, 1000, CVAR_ARCHIVE );

    /* Set by name */
    printf( "Initial value: %s\n", cvar_get_value( "test_value" ) );

    cvar_set_value( "test_value", "250" );
    printf( "After set to 250: %s\n", cvar_get_value( "test_value" ) );

    /* Bounds checking */
    cvar_set_value( "test_value", "5000" );    // Will clamp to max
    printf( "After set to 5000 (clamped): %s\n", cvar_get_value( "test_value" ) );

    cvar_set_value( "test_value", "-50" );    // Will clamp to min
    printf( "After set to -50 (clamped): %s\n", cvar_get_value( "test_value" ) );

    /* Reset to default */
    cvar_t* cv = cvar_find( "test_value" );
    cvar_reset( cv );
    printf( "After reset: %s\n", cvar_get_value( "test_value" ) );

    cvar_system_exit();
}

/*==============================================================================================
    Example 3: Callbacks
==============================================================================================*/

void
my_callback( cvar_t* cv )
{
    printf( "Callback triggered for '%s' - new value: %s\n", cvar_get_name( cv ),
            cvar_get_value( cvar_get_name( cv ) ) );
}

void
example_callbacks( void )
{
    printf( "\n=== Example 3: Callbacks ===\n\n" );

    cvar_system_init();

    /* Register cvar */
    cvar_t* cv = cvar_register_i( "callback_test", "Test callback", 0, 0, 100, 0 );

    /* Register callback */
    cvar_callback_register( cv, my_callback, 0 );

    /* Trigger callback by changing value */
    cvar_set_value( "callback_test", "50" );
    cvar_set_value( "callback_test", "75" );

    /* Remove callbacks */
    cvar_callback_unregister( cv );

    /* No callback will be called changing values */
    cvar_set_value( "callback_test", "50" );
    cvar_set_value( "callback_test", "75" );

    cvar_system_exit();
}

/*==============================================================================================
    Example 4: Latched CVars
==============================================================================================*/

void
example_latched_cvars( void )
{
    printf( "\n=== Example 4: Latched CVars ===\n\n" );

    cvar_system_init();

    /* Register latched cvar (change takes effect after apply) */
    cvar_t* cv = cvar_register_i( "r_mode", "Video mode (requires restart)", 0, 0, 10,
                                  CVAR_LATCH | CVAR_ARCHIVE );

    printf( "Current value: %s\n", cvar_get_value( "r_mode" ) );

    /* Change value (will be latched) */
    cvar_set_value( "r_mode", "5" );
    printf( "Value after setting to 5 (latched): %s\n", cvar_get_value( "r_mode" ) );

    /* Value hasn't changed yet */
    printf( "Is latched? %s\n", ( cv->flag & CVAR_LATCHED ) ? "yes" : "no" );

    /* Apply latched changes */
    cvar_apply_latched();
    printf( "After apply: %s\n", cvar_get_value( "r_mode" ) );

    cvar_system_exit();
}

/*==============================================================================================
    Example 5: Config Files
==============================================================================================*/

void
example_config_files( void )
{
    printf( "\n=== Example 5: Config Files ===\n\n" );

    cvar_system_init();

    /* Register some cvars */
    cvar_register_b( "cl_showfps", "Show FPS counter", false, CVAR_ARCHIVE );
    cvar_register_i( "com_maxfps", "Max FPS", 60, 30, 300, CVAR_ARCHIVE );
    cvar_register_f( "s_volume", "Sound volume", 0.8f, 0.0f, 1.0f, CVAR_ARCHIVE );

    /* Set some values */
    cvar_set_value( "cl_showfps", "1" );
    cvar_set_value( "com_maxfps", "120" );
    cvar_set_value( "s_volume", "0.5" );

    /* Write config file */
    printf( "Writing config.cfg...\n" );
    cvar_write_config( "config.cfg", CVAR_ARCHIVE );

    /* Reset all values */
    printf( "\nResetting all cvars...\n" );

    cvar_reset_all();

    printf( "cl_showfps: %s\n", cvar_get_value( "cl_showfps" ) );
    printf( "com_maxfps: %s\n", cvar_get_value( "com_maxfps" ) );
    printf( "s_volume: %s\n", cvar_get_value( "s_volume" ) );

    /* Load config file */
    printf( "\nLoading config.cfg...\n" );

    cvar_exec_config( "config.cfg" );

    printf( "cl_showfps: %s\n", cvar_get_value( "cl_showfps" ) );
    printf( "com_maxfps: %s\n", cvar_get_value( "com_maxfps" ) );
    printf( "s_volume: %s\n", cvar_get_value( "s_volume" ) );

    cvar_system_exit();
}

/*============================================================================================*/

void
test_core_cvar( int argc, char** argv )
{
    printf( "\n========================================\n" );
    printf( "\tCVar System Examples\n" );
    printf( "========================================\n" );

    UNUSED( argc );
    UNUSED( argv );

    example_basic_usage();
    example_runtime_modification();
    example_callbacks();
    example_latched_cvars();
    example_config_files();
    // example_command_line( argc, argv );
    // example_iteration();
    // example_hot_reload();
    // example_full_application();

    printf( "\n========================================\n" );
    printf( "\tAll examples completed!\n" );
    printf( "========================================\n" );
}

/*============================================================================================*/
