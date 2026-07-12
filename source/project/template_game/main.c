/*==============================================================================================

    main.c -- template_game: ORB game host.

    Windowed game in the shipped-game shape: run_host owns the loop; on_update drives
    the simulation. The starter "game" is a square moved with WASD / arrow keys and
    drawn through the render module -- replace game_update with your game.

    Add modules to k_modules and fetch their APIs in game_ready / game_update.

==============================================================================================*/

#include "orb.h"
#include "engine/core/core_host.h"
#include "engine/app/app_api.h"
#include "runtime_service/rhi/rhi_api.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_modules/render/render_api.h"
#include "runtime/run_host.h"

/*==============================================================================================
    Game state -- one square.
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
    /* Fetch module APIs here as they are added:
       MOD_HOST_FETCH_API( physics ); */
}

static void
game_update( f32 dt )
{
    /* Input -> movement. key_down is level-triggered: held keys keep moving.  This starter links
       no gui service, so reading app() directly is correct.  Once you add gui, gate gameplay input
       on its capture fence -- wrap the key reads in  if ( !gui()->want_capture_keyboard() ) { ... }
       (and gate clicks on want_capture_mouse) -- so typing in a text field or clicking a widget does
       not also drive the game.  See gui_api.h want_capture_* for the pattern. */
    f32 dx = 0.0f, dy = 0.0f;
    if ( app()->key_down( APP_KEY_A ) || app()->key_down( APP_KEY_LEFT ) )  dx -= 1.0f;
    if ( app()->key_down( APP_KEY_D ) || app()->key_down( APP_KEY_RIGHT ) ) dx += 1.0f;
    if ( app()->key_down( APP_KEY_W ) || app()->key_down( APP_KEY_UP ) )    dy -= 1.0f;
    if ( app()->key_down( APP_KEY_S ) || app()->key_down( APP_KEY_DOWN ) )  dy += 1.0f;

    s_x += dx * SQUARE_SPEED * dt;
    s_y += dy * SQUARE_SPEED * dt;

    /* Keep the square inside the surface. */
    i32 ctx = run_host_ctx();
    i32 w = 0, h = 0;
    if ( rhi()->context_size( ctx, &w, &h ) && w > 0 && h > 0 )
    {
        const f32 half = SQUARE_SIZE * 0.5f;
        if ( s_x < half )            s_x = half;
        if ( s_y < half )            s_y = half;
        if ( s_x > ( f32 )w - half ) s_x = ( f32 )w - half;
        if ( s_y > ( f32 )h - half ) s_y = ( f32 )h - half;
    }

    /* Submit this frame's scene. draw_scene replays and clears the list. */
    const f32 white[ 4 ] = { 0.95f, 0.95f, 0.95f, 1.0f };
    render()->submit_rect( ctx, s_x, s_y, SQUARE_SIZE, SQUARE_SIZE, white );
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
    { 0 }
};

static const run_host_desc_t k_desc = {
    .name      = "template_game",
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
