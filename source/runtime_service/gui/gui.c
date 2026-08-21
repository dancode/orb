/*==============================================================================================

    runtime_service/gui/gui.c -- THE GUI MODULE FACE: vtable + descriptor + DLL exports.

    The GUI module's front door -- the one the rest of the engine actually links against. 
    When other code calls gui()->draw_rect(...) or any other function on the module,
    it is going through a table of function pointers this file assembles. This file carries
    no logic of its own: the frame orchestration lives in the separate gui_frame.c unit, and
    every widget / layout / render path lives in its own carved unit (GUI_ARCHITECTURE.md). 
    
    This unit's whole job is to build that function table (g_gui_api_struct) and the small
    descriptor that tells the module system how to load, hot-reload, and unload this module.

    It includes every other unit's header since building the function table means naming 
    every function in it. This file owns the storage for the pointers to the sibling app/rhi
    module APIs this module depends on -- fetched once when the module loads, and read 
    everywhere else in the GUI through a shared accessor rather than refetched per unit.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/
/* The whole stack's decls -- purely so gui_api.c's vtable can name every unit's implementation
   function (their prototypes live across these internal unit headers). */

#include "runtime_service/gui/render/gui_render.h"
#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/stock/gui_stock_internal.h"
#include "runtime_service/gui/chrome/gui_chrome.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*============================================================================================*/
/* Sibling module APIs -- fetched once at module init (gui_api.c), read everywhere via the
   extern accessors. THIS is the one TU that defines the pointer storage. */

#include "engine/app/app_api.h"
#include "runtime_service/rhi/rhi_api.h"

MOD_USE_APP;
MOD_USE_RHI;

/*============================================================================================*/

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
