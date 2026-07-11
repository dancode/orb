/*==============================================================================================

    audio.c -- Unity build entry for the audio module.

    Tier 2 of the audio stack: hot-reloadable name-based sound playback over the ahi
    service (Tier 1, runtime_service/ahi).

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>

#include "engine/mod/mod_export.h"
#include "runtime_modules/audio/audio_api.h"
#include "runtime_service/ahi/ahi_api.h"

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef AUDIO_API_C_PRELUDE
#include "runtime_modules/audio/audio_api.c"
#endif

/*============================================================================================*/
