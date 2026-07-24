/*==============================================================================================

    runtime_service/gui/gui.c -- THE GUI MODULE FACE: vtable + descriptor + DLL exports.

    The gui module's public identity and nothing else.  It carries no logic: the frame
    orchestration lives in the SEPARATE gui_frame.c unit, and every widget / layout / render
    path lives in its own carved unit (GUI_ARCHITECTURE.md).  This unit's whole job is to
    assemble the module vtable (g_gui_api_struct) that the module system hands out through the
    gui() accessor, and the mod_desc_t that registers it.

    Why it includes every unit header: the vtable binds a function pointer to every unit's
    implementation, and those functions are declared across the internal unit headers -- so the
    export TU sees the same full header set the frame orchestrator does, purely to resolve the
    vtable's names.

    Why the API pointer STORAGE lives here (MOD_USE_RHI / MOD_USE_APP): the module init that
    fetches the sibling APIs (gui_mod_init, in gui_api.c below) must sit in the same TU as the
    g_rhi_api_ptr / g_app_api_ptr definitions it assigns.  Every other unit -- gui_frame.c
    included -- only reads them through the extern accessors in rhi_api.h / app_api.h.

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
/* Sibling module APIs -- fetched once at module init (gui_api.c), 
   read everywhere via the extern accessors. THIS is the one TU that defines the pointer storage. */

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
