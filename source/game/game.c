/*==============================================================================================

    game.c -- Unity build entry for the game framework module.

    The framework has two halves:
      - the RUNNER (game_api.c): the play session over a project DLL -- state machine,
        fixed-step clock, per-frame phase drive of the project contract
      - the WORLD (game/framework/): entities, components, queries -- joins this unity
        when the first project consumes it

    Layering
    --------
        project DLL (run_project.h)   <- implements on_start/sim/frame/draw/stop
            ^
            | proj->on_*()
            |
        game (this DLL)               <- session verbs + world data, hot-reloadable
            ^
            | game()->...
            |
        host on_ready / on_update     <- project_bind once, tick( dt, view ) per frame

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#define LOG_CH "game"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"
#include "game/game_api.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "game/framework/framework.c" -- the world, once a project consumes it */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef GAME_API_C_PRELUDE
#include "game/game_api.c"
#endif

/*============================================================================================*/
