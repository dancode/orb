/*============================================================================================*/

#include "engine/mod/mod_export.h"

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    int window_count;

} app_state_t;

static app_state_t* state = NULL;

/*==============================================================================================
    API Start / Shutdown
==============================================================================================*/


/*==============================================================================================
    API implementation
==============================================================================================*/

void
app_function( void )
{
    /* example function to export in the API struct */
}


/*==============================================================================================
    API struct
==============================================================================================*/


const app_api_t g_app_api_struct = { .app_function = app_function };

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
app_mod_init( void* raw_state, get_api_fn get_api )
{
    state = ( app_state_t* )raw_state;

    UNUSED( state );
    UNUSED( get_api );

    // if ( !MOD_FETCH_API( core_api_t, core ) )
    //    return false;

    /* sys calls are made directly here — no lookup needed */
    // core_api()->log( "[app] init\n" );
    // return app_open_window( "My Game", 1280, 720 );
    return true;
}

static void
app_mod_exit( void* raw_state )
{
    state = (app_state_t*)raw_state;
    UNUSED( state );
    // core_api()->log( "[app] exit\n" );
    // app_close_window();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
app_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( app_state_t ),
        .deps       = { NULL },
        .dep_count  = 0,

        .init       = app_mod_init,
        .exit       = app_mod_exit,
        .reload     = NULL,
    };
    return &api;
}

/*============================================================================================*/