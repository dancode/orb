/*==============================================================================================

    sandbox_game_main.c — SHIPPED GAME shape.

    Windowed, no hot-reload, no console. The only quit path is the OS window close
    button, routed through app_api()->pump_events() returning false. No developer
    features, no operator keyboard shortcuts.

    on_update drives the simulation: physics then gameplay, in the order that matters.
    Add modules to k_modules; fetch and call their APIs in game_ready / game_update.

    Loop:  RUN_LOOP_RUN
    Flags: (none)

==============================================================================================*/

#include "orb.h"
#include "runtime/host.h"
#include "engine/app/app.h"

/* add module API headers as they are built:
   #include "runtime_modules/physics/physics_api.h"
   #include "project/sample_game/sample_game_api.h"

   MOD_DEFINE_API_PTR( physics_api_t,     physics     );
   MOD_DEFINE_API_PTR( sample_game_api_t, sample_game );   */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
game_ready( void )
{
    /* HOST_FETCH_API( physics_api_t,     physics     );
       HOST_FETCH_API( sample_game_api_t, sample_game ); */
}

static void
game_update( f32 dt )
{
    /* physics_api()->update( dt );
       sample_game_api()->update( dt ); */
    UNUSED( dt );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( app ), RUN_MODULE( render ), RUN_MODULE( sample_game ), { 0 } }; // RUN_MODULE( physics )

static const run_host_desc_t k_desc = {
    .name      = "sandbox_game",
    .flags     = 0, /* no hot-reload, no console — shipping config */
    .loop_mode = RUN_LOOP_RUN,
    .modules   = k_modules,
    .on_ready  = game_ready,
    .on_update = game_update,
};

int
main( int argc, char** argv )
{
    return run_host_main( &k_desc, argc, argv );
}
