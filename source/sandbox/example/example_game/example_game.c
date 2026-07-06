/*==============================================================================================

    sandbox_game_main.c -- SHIPPED GAME shape.

    Windowed, no hot-reload, no console. The only quit path is the OS window close
    button, routed through app()->pump_events() returning false. No developer
    features, no operator keyboard shortcuts.

    on_update drives the simulation: physics then gameplay, in the order that matters.
    Add modules to k_modules; fetch and call their APIs in game_ready / game_update.

    The 0.1 "game": a square driven by WASD / arrow keys, submitted to the render
    module each frame via render()->submit_rect and drawn by render()->draw_scene
    through the draw service. Proves the input -> update -> submit -> render path
    end to end before the game framework layer takes over the update.

    Loop:  RUN_LOOP_RUN
    Flags: (none)

==============================================================================================*/

#include "orb.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"
#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
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

static f32 s_x = 640.0f; /* center of the default 1280x720 window */
static f32 s_y = 360.0f;

/*==============================================================================================
    Host callbacks
==============================================================================================*/

static void
game_ready( void )
{
    /* MOD_HOST_FETCH_API( physics     );
       MOD_HOST_FETCH_API( sample_game ); */
}

static void
game_update( f32 dt )
{
    /* Input -> movement.  key_down is level-triggered: held keys keep moving the square. */
    f32 dx = 0.0f, dy = 0.0f;
    if ( app()->key_down( APP_KEY_A ) || app()->key_down( APP_KEY_LEFT ) )  dx -= 1.0f;
    if ( app()->key_down( APP_KEY_D ) || app()->key_down( APP_KEY_RIGHT ) ) dx += 1.0f;
    if ( app()->key_down( APP_KEY_W ) || app()->key_down( APP_KEY_UP ) )    dy -= 1.0f;
    if ( app()->key_down( APP_KEY_S ) || app()->key_down( APP_KEY_DOWN ) )  dy += 1.0f;

    s_x += dx * SQUARE_SPEED * dt;
    s_y += dy * SQUARE_SPEED * dt;

    /* Keep the square inside the surface. */
    i32 w = 0, h = 0;
    if ( rhi()->context_size( 0, &w, &h ) && w > 0 && h > 0 )
    {
        const f32 half = SQUARE_SIZE * 0.5f;
        if ( s_x < half )              s_x = half;
        if ( s_y < half )              s_y = half;
        if ( s_x > ( f32 )w - half )   s_x = ( f32 )w - half;
        if ( s_y > ( f32 )h - half )   s_y = ( f32 )h - half;
    }

    /* Submit this frame's scene.  draw_scene replays and clears the list. */
    const f32 white[ 4 ] = { 0.95f, 0.95f, 0.95f, 1.0f };
    render()->submit_rect( s_x, s_y, SQUARE_SIZE, SQUARE_SIZE, white );
}

/*==============================================================================================
    Host descriptor
==============================================================================================*/

static const run_module_entry_t k_modules[] = {
    RUN_SERVICE( core   ),
    RUN_SERVICE( app    ),
    RUN_SERVICE( rhi    ),
    RUN_SERVICE( draw   ),
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
