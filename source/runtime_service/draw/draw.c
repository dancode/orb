/*==============================================================================================

    runtime_service/draw/draw.c -- Unity build entry for the draw library.

    Include order matters: geo and batch helpers come before material and cmd so their
    static functions are visible.  draw_api.c is included last so the vtable initializer
    can reference all implementation functions defined in the preceding fragments.

==============================================================================================*/

#include "draw_api.h"
#include "runtime_service/rhi/rhi_api.h"

/* Declare the file-scope cached rhi API pointer used throughout this translation unit.
   In dynamic builds this expands to: static rhi_api_t* g_rhi_api_ptr = NULL;
   In static builds this is a no-op (the shared global struct is used directly). */
MOD_USE_RHI

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/draw/draw_geo.c"
#include "runtime_service/draw/draw_batch.c"
#include "runtime_service/draw/draw_material.c"
#include "runtime_service/draw/draw_cmd.c"
#include "runtime_service/draw/draw_helper.c"

#ifndef DRAW_API_C_PRELUDE
    #include "runtime_service/draw/draw_api.c"
#endif


/*============================================================================================*/
