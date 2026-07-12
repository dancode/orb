#ifndef GAME_API_H
#define GAME_API_H
/*==============================================================================================

    game/game_api.h -- game framework API struct and gateway macro.

    The framework is the STANDARD DRIVER of the project contract (runtime/run_project.h):
    it owns the play state machine and the fixed-step clock so hosts don't have to.  A
    host binds the loaded project once, controls the session, and ticks every frame:

        game()->project_bind( name );          // on_ready -- after mod_init_all
        game()->play();                        // or behind an editor Play button
        game()->tick( dt, &view );             // every frame; no-op while GAME_STOPPED

    tick() runs the fixed-step accumulator (game_fixed_hz cvar) -- on_sim xN at the fixed
    step, then on_frame, then on_draw with the interpolation alpha.  pause() freezes the
    sim but keeps frame/draw running; step() advances exactly one sim tick while paused.

    Hosts that want full control can skip this module entirely and drive the project
    vtable directly -- the contract has no framework dependency by design.

==============================================================================================*/

#include "game/game.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct game_api_s
{
    /* session control -- the only way a play session starts or stops */

    bool ( *project_bind )( const char* name );   // resolve the project's vtable; false = not loaded
    void ( *play  )( void );                      // GAME_STOPPED -> GAME_PLAYING; calls on_start
    void ( *stop  )( void );                      // any -> GAME_STOPPED; calls on_stop
    void ( *pause )( bool paused );               // GAME_PLAYING <-> GAME_PAUSED
    void ( *step  )( void );                      // paused only: advance exactly one sim tick
    i32  ( *state )( void );                      // current game_play_state_t

    /* the one per-frame host call -- fixed-step sim + frame + draw */

    void ( *tick )( f32 dt, const run_view_t* view );

} game_api_t;

#if defined( BUILD_STATIC ) || defined( GAME_STATIC )
MOD_GATEWAY_STATIC( game_api_t, game )
    #define MOD_USE_GAME    /* static build */
    #define MOD_FETCH_GAME  true
#else
MOD_GATEWAY_DYNAMIC( game_api_t, game )
    #define MOD_USE_GAME    MOD_DEFINE_API_PTR( game_api_t, game )
    #define MOD_FETCH_GAME  MOD_FETCH_API( game_api_t, game )
#endif

// clang-format on
/*============================================================================================*/
#endif    // GAME_API_H
