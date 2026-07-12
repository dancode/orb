#ifndef GAME_H
#define GAME_H
/*==============================================================================================

    game/game.h -- Game framework types.

    The framework runs a play SESSION over a project DLL (runtime/run_project.h): the
    session state below is the whole lifecycle a host can observe or drive.

==============================================================================================*/

#include "orb.h"
#include "runtime/run_project.h"

/*============================================================================================*/

typedef enum game_play_state_e
{
    GAME_STOPPED = 0,   // no play session -- tick() is a no-op
    GAME_PLAYING,       // sim advances at the fixed step; frame/draw every tick
    GAME_PAUSED,        // sim frozen (step() advances one tick); frame/draw still run

} game_play_state_t;

/*============================================================================================*/
#endif    // GAME_H
