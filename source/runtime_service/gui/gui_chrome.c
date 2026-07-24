/*==============================================================================================

    runtime_service/gui/gui_chrome.c -- Unity build entry for the GUI_CHROME unit.

    The stock widget set and the host structures over the core tiers, all under chrome/:
    chrome/widgets/ (prefab emit clients composing the three sibling roles),
    chrome/table/, chrome/window/ (persisted record + chrome policy -- the stock RECIPE over
    the feat_* kit), chrome/dock/, chrome/popup/, and chrome/nav/ (core-classified as a peer
    focus service, but it reads/drives the popup stack, so it lives here).

    One of the carved translation units (beside gui.c, gui_render.c, gui_core.c, gui_style.c,
    gui_interact.c, gui_flow.c, gui_element.c, gui_draw.c, gui_debug.c).  The compiler enforces the boundary: everything
    resolves through the public gui_* surface (gui_host.h), the backend draw API
    (gui_render.h), and the unit headers below it -- the ambient records, the
    core service seams (item / id / io / state / style / paint / gesture services), and the
    flow unit's emit surface.  Chrome's few core-facing definitions (scrollbar_widget, the
    viewport request slot) are seams the other direction.

    Include order inside the unit is the static-visibility dependency order, unchanged from
    the unity list gui.c carried: widgets -> table -> window -> dock -> popup -> nav.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"
#include "base/math_ease.h"

/* This unit's world -- everything below it (the include list IS the dependency graph).
   Chrome is policy over the whole stack; the render header carries its documented server
   crossing (the text-selection run capture). */
#include "runtime_service/gui/render/gui_render.h"    /* pulls gui_host.h + rhi/app APIs */
MOD_USE_RHI;
MOD_USE_APP;

#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/stock/gui_element_internal.h"
#include "runtime_service/gui/chrome/gui_chrome.h"
#include "runtime_service/gui/debug/gui_debug.h"

// clang-format off

/* The stock widget set (a client of the tiers below; calls to the user/ vocabulary resolve
   through the public declarations in gui_host.h -- deliberate dogfooding). */
#include "runtime_service/gui/chrome/widgets/gui_text_edit.c"
#include "runtime_service/gui/chrome/widgets/gui_scrollbar.c"
#include "runtime_service/gui/chrome/widgets/gui_text.c"
#include "runtime_service/gui/chrome/widgets/gui_button.c"
#include "runtime_service/gui/chrome/widgets/gui_tree.c"
#include "runtime_service/gui/chrome/widgets/gui_input.c"
#include "runtime_service/gui/chrome/widgets/gui_text_edit_multi.c"
#include "runtime_service/gui/chrome/widgets/gui_volatile.c"
#include "runtime_service/gui/chrome/widgets/gui_widget_slider.c"
#include "runtime_service/gui/chrome/widgets/gui_widget_numeric.c"
#include "runtime_service/gui/chrome/widgets/gui_tab_bar.c"

/* Table -- independent optional feature (no window dependency). */
#include "runtime_service/gui/chrome/table/gui_table.c"

/* Window subsystem (first real optional boundary).  gui_select.c first: the text-selection
   controller (chrome -- it reads the render capture + font metrics) paints under the
   window begins and resolves in gui_window_end, all below. */
#include "runtime_service/gui/chrome/window/gui_select.c"
#include "runtime_service/gui/chrome/window/gui_window.c"
#include "runtime_service/gui/chrome/window/gui_window_native.c"
#include "runtime_service/gui/chrome/window/gui_window_docked.c"
#include "runtime_service/gui/chrome/window/gui_window_free.c"
#include "runtime_service/gui/chrome/window/gui_window_end.c"

/* Dock -- window-dependent, independent of popup/. */
#include "runtime_service/gui/chrome/dock/gui_dock_core.c"
#include "runtime_service/gui/chrome/dock/gui_dock_float.c"
#include "runtime_service/gui/chrome/dock/gui_dock_drag.c"
#include "runtime_service/gui/chrome/dock/gui_dock.c"
#include "runtime_service/gui/chrome/dock/gui_dock_serialize.c"
#include "runtime_service/gui/chrome/dock/gui_dock_route.c"

/* Popup -- window-dependent overlay stack; nav between popup and its clients (it reads/drives
   the popup stack gui_popup.c just opened). */
#include "runtime_service/gui/chrome/popup/gui_popup.c"
#include "runtime_service/gui/chrome/nav/gui_nav.c"
#include "runtime_service/gui/chrome/popup/gui_combo.c"
#include "runtime_service/gui/chrome/popup/gui_menu.c"
#include "runtime_service/gui/chrome/popup/gui_toolbar.c"

/* MEMORY ACCOUNTING: this unit's fixed statics, reported to gui_ui_memory (gui_ui_mem.c)
   through the chrome/gui_chrome.h seam.  Last so every static aggregate above is in scope. */
u32
gui_chrome_unit_mem_bytes( void )
{
    u32 b = 0;
    /* Both text-edit undo rings (single-line s_undo + multiline s_medit_undo) moved to the
       interact edit engines and are counted by gui_interact_unit_mem_bytes. */
    b += (u32)( sizeof( s_num_edit_buf ) + sizeof( s_tabbars ) );
    b += (u32)( sizeof( s_tab ) + sizeof( s_tab_scroll_dummy ) );
    b += (u32)sizeof( s_select );   /* window text selection (chrome/window/gui_select.c) */
    b += (u32)( sizeof( s_dock_drag ) + sizeof( s_dock_tab_drag ) + sizeof( s_dock_float_req ) );
    b += (u32)( sizeof( s_menubar_sink ) + sizeof( s_menubar_saved_clip )
              + sizeof( s_tooltip_save ) );
    return b;
}

// clang-format on
/*============================================================================================*/
