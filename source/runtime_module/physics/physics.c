/*==============================================================================================

    physics.c

    Physics module.

==============================================================================================*/

#include <stdio.h>
#include "orb.h"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"

#include "physics_api.h"

MOD_DEFINE_API_PTR( core_api_t, core );

/*============================================================================================*/

typedef struct physics_state_s
{
    int   frame_count;

} physics_state_t;

static physics_state_t* g_state = NULL;

/*==============================================================================================
    Implementation — shared by both build modes
==============================================================================================*/
void 
physics_function( void )
{
    if ( !g_state )
        return;

    g_state->frame_count++;
    core_api()->log( "physics: physics_function called (frame_count=%d)", g_state->frame_count );
}

const physics_api_t g_physics_api_struct = {
    .physics_function = physics_function
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
physics_init( void* raw_state, get_api_fn get_api )
{
    g_state = ( physics_state_t* )raw_state;

    if ( !MOD_FETCH_API( core_api_t, core ) )
        return false;

    core_api()->log( "physics: init (state=%p)", ( void* )g_state );
    return true;
}

void
physics_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( core_api() )
        core_api()->log( "physics: exit" );
}

static bool
physics_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );

    g_state = ( physics_state_t* )raw_state;
    MOD_FETCH_API( core_api_t, core );

    core_api()->log( "physics: reloaded (frames so far = %d)", g_state->frame_count );
    return true;
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
physics_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( physics_state_t ),
        .deps       = { "core" },    // "app" + remove "engine"
        .dep_count  = 1,
        .func_api   = &g_physics_api_struct,
        .init       = physics_init,
        .exit       = physics_exit,
        .reload     = physics_reload,
    };
    return &api;
}

void*
physics_get_api( void )
{
    return ( void* )&g_physics_api_struct;
}

MOD_DEFINE_EXPORTS( physics );

/*============================================================================================*/
