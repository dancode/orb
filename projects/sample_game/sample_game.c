/*==============================================================================================

    sample_game.c

==============================================================================================*/

#include <stdio.h>
#include "orb.h"
#include "core/core.h"
#include "sample_game.h"

/*==============================================================================================
    Forward declarations of public API implementations.
==============================================================================================*/

typedef struct player_desc_s player_desc_t;

static void
sg_spawn_player( const player_desc_t* desc )
{
    //
}

static int
sg_get_player_count( void )
{
    return 0;
}

// TODO: add module registry api to query and obtain module api structs

/*==============================================================================================
    Public API struct (generated ideally)
==============================================================================================*/

typedef struct module_api_header_s
{
    uint32_t api_version;    // e.g. SAMPLE_GAME_API_VERSION
    uint32_t struct_size;    // sizeof( sample_game_api_t )

} module_api_header_t;

typedef struct sample_game_api_s
{
    // This should be a generated header we expect modules to implement.
    // For now we define it here manually.
    // We should have a versioned api struct for each module type.
        // Each module needs to export a header defining its api struct.
        // All the types and function pointers required to use the module api should be defined there.
        // so that modules can be compiled against the correct version of the api struct.
        // and types are known at compile time.

    module_api_header_t header;
    
    void ( *spawn_player )( const player_desc_t* desc );    // function pointer
    int ( *get_player_count )( void );                      // function pointer

} sample_game_api_t;

static sample_game_api_t g_sample_game_api = { .header.api_version      = 1,
                                               .header.struct_size      = sizeof( sample_game_api_t ),
                                               .spawn_player     = sg_spawn_player,
                                               .get_player_count = sg_get_player_count };

/*==============================================================================================
    Exported descriptor accessor

    This creates a struct to implementt the module info and update callbacks in one go.
    Rather than grabbing each symbol individually.
    This is called by the module system when loading the module.
    This way we can set the names without having to rely on fixed symbol names.
    (except for this one function).
==============================================================================================*/

// API_EXPORT module_t*
// module_get_descriptor( void )
// {
//     static const char* required[] = { /* "physics", */ NULL };
//     static module_t    desc       = { .name             = "sample_game",
//                                       .module_version   = 1,
//                                       .flags            = 0,
//                                       .required_modules = required,
//                                       .api_version      = g_sample_game_api.api_version,
//                                       .api              = &g_sample_game_api,
//                                       .init             = sample_game_init,
//                                       .tick             = sample_game_tick,
//                                       .shutdown         = sample_game_shutdown };
//     return &desc;
// }

/*==============================================================================================

    dll api

==============================================================================================*/

core_api_t*       g_core_api  = NULL;    // Core API pointer
core_debug_api_t* g_debug_api = NULL;    // Core Debug API pointer

/*============================================================================================*/

static int32_t g_draw_debug  = 10;
static char*   g_player_name = "my_name";

/*============================================================================================*/

API_EXPORT void
module_init( core_api_t* core_api /* add module (registry) api field */ )
{
    // printf( "[game] init\n" );

    /**************************************************************/
    /* Store core api pointer for later use */

    g_core_api  = core_api;
    g_debug_api = core_api->debug_api;

    g_core_api->log( "[sample_game] init" );

    // Example: validate required dependency (if any)
    // auto other_api = reg->get_module_api( "physics", 1 );
    // if ( !other_api ) { core->log("[sample_game] missing dependency physics"); return false; }

    // Optionally register cvars
    // core->cvar_register( "r_draw_debug", CVAR_INT, &g_draw_debug );
    // core->cvar_register( "player_name", CVAR_STRING, &g_player_name );

    /**************************************************************/


    // Register our cvar, binding registry to &g_draw_debug
    // g_api->cvar_register( "r_draw_debug", CVAR_INT, &g_draw_debug );
    // g_api->cvar_register( "player_name", CVAR_STRING, &g_player_name );

    // g_api->log( "[game] init: r_draw_debug = %d", g_draw_debug );
}

API_EXPORT void
module_tick( float dt )
{
    // printf( "[game] tick\n" );

    if ( g_draw_debug )
    {
        g_core_api->log( "[game] drawing debug stuff" );
    }
}

API_EXPORT void
module_exit( void )
{
    // printf( "[game] shutdown\n" );

    g_core_api->log( "[game] shutdown" );
}

/*==============================================================================================

    main (only for Windows DLL projects; ignore on Linux/macOS)

==============================================================================================*/

#if defined(_WIN32) || defined(_WIN64)
int
main( int argc, char** argv )
{
    (void)argc; (void)argv;
    return 1;
}
#endif

/*============================================================================================*/