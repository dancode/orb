/*==============================================================================================

    runtime_service/ahi/ahi.c -- Unity build entry for the audio hardware interface.

    Mirrors rhi.c.  The build compiles only this file for ahi; the mixer, the platform
    backend, and the API wiring are #included here.  The ahi service is a STATIC service
    (like rhi/draw/gui): linked into the host and registered via mod_static, never a DLL.

    Inclusion order matters:
        1. orb.h + LOG_CH + engine headers
        2. Platform headers        (windows.h + WASAPI, gated by OS_WINDOWS)
        3. ahi_api.h               (types + vtable + gateway)
        4. ahi_backend.h           (backend seam decls)
        5. ahi_mixer.c             (voice pool + command ring + mix loop)
        6. backend                 (ahi_backend_wasapi.c or ahi_backend_null.c)
        7. ahi_api.c               (vtable wiring + mod descriptor -- must be last)

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "base/base.h"    /* f32_sqrt in the mix loop */

#define LOG_CH "ahi"

#include "engine/sys/sys_host.h"      /* threads, atomics */
#include "engine/core/core_api.h"     /* LOG_* */

MOD_USE_CORE;

/*==============================================================================================
    Platform Headers
==============================================================================================*/

#if OS_WINDOWS

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define COBJMACROS
    #include <windows.h>
    #include <mmdeviceapi.h>
    #include <audioclient.h>

#endif

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/ahi/ahi_api.h"
#include "runtime_service/ahi/ahi_backend.h"

#include "runtime_service/ahi/ahi_mixer.c"

#if OS_WINDOWS
    #include "runtime_service/ahi/ahi_backend_wasapi.c"
#else
    #include "runtime_service/ahi/ahi_backend_null.c"
#endif

#ifndef AHI_API_C_PRELUDE
    #include "runtime_service/ahi/ahi_api.c"
#endif

/*============================================================================================*/
