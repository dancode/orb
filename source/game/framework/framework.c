/*==============================================================================================

    game/framework/framework.c -- unity unit for the world framework.

    Include this ONCE from the owning target's unity:
      - the game runner DLL (game.c) compiles it in so the world lives in game module state
      - sandboxes (sb_world.c) compile it in directly

    Requirements on the including TU's build:
      - core and ref reachable through their gateways ( core() / ref() ).  In exe targets
        the build tool's CORE_STATIC / REF_STATIC defines make these direct calls; in a
        DLL the owner must MOD_USE / MOD_FETCH core and ref in its own init()/reload().

==============================================================================================*/

#include "orb.h"
#include <string.h>

#ifndef LOG_CH
    #define LOG_CH "world"    // owner unity may have set its own channel first
#endif
#include "engine/mod/mod_import.h"
#include "engine/core/core_api.h"
#include "engine/ref/ref_api.h"

#include "game/framework/world.h"
#include "game/framework/world_internal.h"

#include "game/framework/pool.c"
#include "game/framework/world.c"
#include "game/framework/comp.c"

/*============================================================================================*/
