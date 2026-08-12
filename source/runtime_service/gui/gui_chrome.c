/*==============================================================================================

    runtime_service/gui/gui_chrome.c -- Unity build entry for the GUI_CHROME unit.

    This is the "product" layer: the actual widgets and window framework the editor and other
    engine tools use day to day -- buttons, text fields, tables, whole draggable/resizable
    windows, docking, popups and context menus, and keyboard navigation between all of it. It
    is built entirely on top of the lower units (interact, style, draw, flow) through their
    public surface, the same way any other piece of code using the GUI would.

    chrome/ is organized by feature: chrome/widgets/ is the widget set built directly on the
    lower units, chrome/table/ handles multi-column tables, chrome/window/ is a window's
    persisted state (position, size) plus the policy for how it behaves, chrome/dock/ lets
    windows snap together into tabbed docking layouts, chrome/popup/ is floating menus and
    context menus, and chrome/nav/ resolves keyboard/gamepad navigation between everything on
    screen (it lives here rather than in core because it needs to know about the popup stack).

    Include order inside the unit is the dependency order between those pieces: widgets, then
    table, then window, then dock, then popup, then nav -- each one can use anything defined by
    a piece listed before it.

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
#include "base/utf8.h"        // codepoint stepping on the wrap-walk + selection measure seams

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
#include "runtime_service/gui/stock/gui_stock_internal.h"
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
#include "runtime_service/gui/chrome/widgets/gui_plot.c"
#include "runtime_service/gui/chrome/widgets/gui_widget_color.c"
#include "runtime_service/gui/chrome/widgets/gui_tab_bar.c"
#include "runtime_service/gui/chrome/widgets/gui_box.c"

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
chrome_unit_mem_bytes( void )
{
    u32 b = 0;
    /* Both text-edit undo rings (single-line s_undo + multiline s_medit_undo) moved to the
       interact edit engines and are counted by interact_unit_mem_bytes. */
    b += (u32)( sizeof( s_num_edit_buf ) + sizeof( s_tabbars ) + sizeof( s_hex_buf ) );
    b += (u32)sizeof( s_tab_stack );
    b += (u32)sizeof( s_select );   /* window text selection (chrome/window/gui_select.c) */
    b += (u32)( sizeof( s_dock_drag ) + sizeof( s_dock_tab_drag ) + sizeof( s_dock_float_req ) );
    b += (u32)( sizeof( s_menubar_sink ) + sizeof( s_menubar_saved_clip )
              + sizeof( s_tooltip_save ) );
    b += (u32)( sizeof( s_box ) + sizeof( s_box_sp ) );   /* the box decorator's nesting stack */
    return b;
}

// clang-format on
/*============================================================================================*/
