/*==============================================================================================

    audio.c -- Unity build entry for the audio module.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>

#include "engine/mod/mod_export.h"
#include "runtime_modules/audio/audio_api.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "runtime_modules/audio/audio_function.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef AUDIO_API_C_PRELUDE
#include "runtime_modules/audio/audio_api.c"
#endif

/*============================================================================================*/
