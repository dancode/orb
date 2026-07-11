#ifndef AUDIO_H
#define AUDIO_H
/*==============================================================================================

    runtime_modules/audio/audio.h -- Audio module types.

    The game-facing sound layer (Tier 2): a hot-reloadable module that plays sounds BY
    NAME from a registered bank.  All device and mixing work happens in the ahi service
    (Tier 1) -- this module only sends it commands, which is what makes it safe to
    hot-reload while audio is playing.

==============================================================================================*/

#include "orb.h"
#include "runtime_service/ahi/ahi.h"    /* ahi_sound_t -- the PCM description shared with Tier 1 */

/* Opaque play handle; wraps an ahi voice id.  0 is never a live handle. */
typedef u32 audio_handle_t;

#define AUDIO_HANDLE_INVALID 0

#define AUDIO_MAX_SOUNDS     32
#define AUDIO_NAME_MAX       32

/*============================================================================================*/
#endif    // AUDIO_H
