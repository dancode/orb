/*==============================================================================================

    game_example.c : example game module.

    Stub module demonstrating the game module pattern.
    Hot-reloadable in dynamic builds; statically linked in monolithic builds.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "game_modules/game_example/game_example_api.h"

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    int init;

} game_example_state_t;

static game_example_state_t* s_state = NULL;

/*==============================================================================================
    API implementations
==============================================================================================*/

static void
game_example_placeholder( void )
{
}

/*==============================================================================================
    Public API struct
==============================================================================================*/

const game_example_api_t g_game_example_api = {
    .placeholder = game_example_placeholder,
};

/*==============================================================================================
    Lifecycle callbacks
==============================================================================================*/

static bool
game_example_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s_state       = ( game_example_state_t* )raw_state;
    s_state->init = 1;
    return true;
}

static bool
game_example_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s_state = ( game_example_state_t* )raw_state;
    return true;
}

static void
game_example_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
game_example_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( game_example_state_t ),
        .func_api_size = sizeof( game_example_api_t ),
        .dep_count     = 0,
        .deps          = { NULL },
        .func_api      = &g_game_example_api,
        .init          = game_example_init,
        .exit          = game_example_exit,
        .reload        = game_example_reload,
    };
    return &desc;
}

void*
game_example_get_api( void )
{
    return ( void* )&g_game_example_api;
}

/*==============================================================================================
    DLL exports — only present in dynamic builds
==============================================================================================*/

MOD_DEFINE_EXPORTS( game_example )

/*============================================================================================*/
