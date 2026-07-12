/*==============================================================================================

    host_game.c -- SHIPPED GAME shape.

    Windowed, no hot-reload, no console. The only quit path is the OS window close
    button, routed through app()->pump_events() returning false. No developer
    features, no operator keyboard shortcuts.

    on_update drives the simulation: physics then gameplay, in the order that matters.
    Add modules to k_modules; fetch and call their APIs in game_ready / game_update.

    The 0.1 "game": a square driven by the INPUT ACTION SERVICE -- game code reads the
    "move" vector and "attack" edges by id and never sees a key code; the bindings
    (WASD / arrows as digital composites, left stick with radial deadzone, mouse1 /
    pad_a for attack) are plain bind/bindaxis lines a config file could own.  Submitted
    to the render module each frame via render()->submit_rect and drawn by
    render()->draw_scene through the draw service.  Proves device -> bind -> action ->
    update -> submit -> render end to end before the game framework layer takes over.

    Loop:  RUN_LOOP_RUN
    Flags: (none)

==============================================================================================*/

#include "orb.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"
#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/input/input_host.h"
#include "runtime_modules/render/render_api.h"
#include "runtime/runtime_host.h"

/* add module API headers as they are built:
   #include "runtime_modules/physics/physics_api.h"
   #include "project/sample_game/sample_game_api.h"

   MOD_DEFINE_API_PTR( physics_api_t,     physics     );
   MOD_DEFINE_API_PTR( sample_game_api_t, sample_game );   */

/*==============================================================================================
    Game state -- the whole 0.1 simulation: one square.
==============================================================================================*/

#define SQUARE_SIZE  60.0f
#define SQUARE_SPEED 400.0f /* pixels per second */
#define ATTACK_FLASH 0.15f  /* seconds the square stays hot after an attack edge */

static f32 s_x = 640.0f; /* center of the default 1280x720 window */
static f32 s_y = 360.0f;
static f32 s_flash = 0.0f;

static input_action_t s_move   = INPUT_ACTION_INVALID; /* AXIS2: WASD/arrows + left stick */
static input_action_t s_attack = INPUT_ACTION_INVALID; /* BUTTON: mouse1 / pad_a / space  */

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
game_ready( void )
{
    /* MOD_HOST_FETCH_API( physics     );
       MOD_HOST_FETCH_API( sample_game ); */

    /* The game declares WHAT it responds to (actions, by id) ... */
    s_move   = input()->action_register( "move",   INPUT_ACTION_AXIS2,  0 );
    s_attack = input()->action_register( "attack", INPUT_ACTION_BUTTON, 0 );

    /* ... and the bindings decide WHICH devices drive them.  Programmatic here because
       this target ships without a console; a real game execs these from default.cfg
       (writeconfig emits the same lines).  +y is up in action space. */
    core()->cmd_execute_string( "bindaxis w move 0 1" );
    core()->cmd_execute_string( "bindaxis s move 0 -1" );
    core()->cmd_execute_string( "bindaxis a move -1 0" );
    core()->cmd_execute_string( "bindaxis d move 1 0" );
    core()->cmd_execute_string( "bindaxis up move 0 1" );
    core()->cmd_execute_string( "bindaxis down move 0 -1" );
    core()->cmd_execute_string( "bindaxis left move -1 0" );
    core()->cmd_execute_string( "bindaxis right move 1 0" );
    core()->cmd_execute_string( "bindaxis pad_lstick move" );

    core()->cmd_execute_string( "bind mouse1 +attack" );
    core()->cmd_execute_string( "bind pad_a +attack" );
    core()->cmd_execute_string( "bind space +attack" );
}

static void
game_update( f32 dt )
{
    /* Action -> movement.  One read covers keyboard composites and stick (summed and
       clamped by the service); +y up in action space, +y down on screen. */
    f32 mx = 0.0f, my = 0.0f;
    input()->value2( s_move, &mx, &my );

    s_x += mx * SQUARE_SPEED * dt;
    s_y -= my * SQUARE_SPEED * dt;

    /* Attack edges flash the square; pressed() counts so no sub-frame tap is lost. */
    if ( input()->pressed( s_attack ) )
        s_flash = ATTACK_FLASH;
    if ( s_flash > 0.0f )
        s_flash -= dt;

    /* Keep the square inside the surface. */
    i32 ctx = run_host_ctx();
    i32 w = 0, h = 0;
    if ( rhi()->context_size( ctx, &w, &h ) && w > 0 && h > 0 )
    {
        const f32 half = SQUARE_SIZE * 0.5f;
        if ( s_x < half )              s_x = half;
        if ( s_y < half )              s_y = half;
        if ( s_x > ( f32 )w - half )   s_x = ( f32 )w - half;
        if ( s_y > ( f32 )h - half )   s_y = ( f32 )h - half;
    }

    /* Submit this frame's scene.  draw_scene replays and clears the list. */
    const f32 white[ 4 ] = { 0.95f, 0.95f, 0.95f, 1.0f };
    const f32 hot[ 4 ]   = { 1.0f, 0.35f, 0.2f, 1.0f };
    render()->submit_rect( ctx, s_x, s_y, SQUARE_SIZE, SQUARE_SIZE, ( s_flash > 0.0f ) ? hot : white );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( core   ),
    RUN_SERVICE( app    ),
    RUN_SERVICE( rhi    ),
    RUN_SERVICE( draw   ),
    RUN_SERVICE( input  ),
    RUN_MODULE ( render ),
    // RUN_MODULE( sample_game ),   /* uncomment when sample_game_api.h exists */
    { 0 }
};  // add RUN_MODULE( physics ) when ready

static const run_host_desc_t k_desc = {
    .name      = "sandbox_game",
    .flags     = 0, /* no hot-reload, no console -- shipping config */
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
