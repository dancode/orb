/*==============================================================================================

    runtime_service/gui/compose/gui_flow.c -- Unity build entry for the GUI_FLOW unit.

    Composition: the only code that turns style spacing metrics (line_size / gap / pad /
    quantum / scales) into rects.  Track resolver + cell emitters, scroll regions, children,
    transient sub-layouts, split panels, the root-level region, and the public layout verbs +
    sz_ sizing family.  Consumes spacing metrics, produces rects; behavior and presentation
    are the sibling roles it never reaches into.

    The fifth translation unit (beside gui.c, gui_render.c, element/gui_element.c,
    debug/gui_debug.c).  The compiler enforces the flow library boundary: everything resolves
    through the public gui_* surface (gui_host.h), the backend draw/clip API (gui_render.h),
    and the seams declared in gui_internal.h's cross-unit sections -- the ambient records
    (g_ctx / s_io / s_style / s_scope / s_build), the core services it composes over, and the
    two upward seams (scrollbar_widget, the region gutter's one widget; gui_anim_f32, the
    size-animate ease).

    Include order inside the unit is the static-visibility dependency order, unchanged from
    the unity list gui.c carried.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"

#include "runtime_service/gui/gui_internal.h"   /* -> gui_host.h -> gui_api.h -> gui.h */
#include "runtime_service/gui/render/gui_render.h"    /* draw_* / clip API                   */

#include "runtime_service/gui/compose/gui_layout_core.c"
#include "runtime_service/gui/compose/gui_scroll.c"
#include "runtime_service/gui/compose/gui_layout_child.c"
#include "runtime_service/gui/compose/gui_sublayout.c"
#include "runtime_service/gui/compose/gui_split.c"
#include "runtime_service/gui/compose/gui_region.c"
#include "runtime_service/gui/compose/gui_layout.c"

/* MEMORY ACCOUNTING: this unit's fixed statics, reported to gui_ui_memory (gui_ui_mem.c)
   through the gui_internal.h seam.  Last so every static aggregate above is in scope.
   (The layout-frame stack itself stays a core static -- gui_ctx.c owns frame turnover.) */
u32
gui_flow_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_layout_state_stack ) + sizeof( s_split_stack )
                + sizeof( s_sublayout_sink ) );
}

/*============================================================================================*/
