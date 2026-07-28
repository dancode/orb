/*==============================================================================================

    runtime_service/gui/gui_component.c -- GUI_COMPONENT translation unit: widget logic (STAGING).

    The component tier: a widget's LOGIC with no look.  A component consumes an (id, rect) and
    does the tedious work -- hit-testing, drag math, value snapping, focus / hover / active
    state -- then reports clear outputs a render consumes.  It never paints, so its include
    list stops BELOW the draw/render servers: it reads the interact server's services and
    nothing that emits.

        state/interact  ->  component  ->  stock  ->  chrome
                            (this unit)   (render)   (product)

    See component/gui_component_internal.h for the tier's charter -- the call shape
    ( id, rect, ... ) and the result shape (gui_item_state_t state first) every component
    follows.  Six components live here (slider, button, check, cycle, selectable, input): the
    whole interactive stock_* set now renders over them, and a user widget is a stock render's
    sibling over the same call.  chrome/ has NOT migrated down and keeps its bespoke widgets.

==============================================================================================*/

#include <stdio.h>    /* GUI_WARN_ONCE (rect/gui_rect.h) -- every unit root provides it */
#include <math.h>     /* floorf -- the slider snap grid */

#include "orb.h"

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Logic only -- the public gui types, the interact server + its storage + the gesture
   mechanisms; NOT draw/render (a component never paints) and NOT chrome.  The style header
   rides along only for the geometry vocabulary interact reads (WIN_BORDER); no paint. */

#include "runtime_service/gui/gui_host.h"       /* public gui types (-> gui.h -> rect)     */
#include "runtime_service/rhi/rhi_api.h"         /* rhi handles the viewport record stores  */
#include "engine/app/app_api.h"                  /* APP_WIN_MAX -- the per-surface fan-out   */

#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/font/gui_font.h"       /* font_char_h -- the text field measures (no paint) */
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/component/gui_component_internal.h"

/*============================================================================================*/

#include "runtime_service/gui/component/gui_comp_button.c"
#include "runtime_service/gui/component/gui_comp_slider.c"
#include "runtime_service/gui/component/gui_comp_check.c"
#include "runtime_service/gui/component/gui_comp_cycle.c"
#include "runtime_service/gui/component/gui_comp_selectable.c"
#include "runtime_service/gui/component/gui_comp_input.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  None yet (the components are stubs); the seam exists so the unit is a
    first-class member of the accounting contract as it grows.
==============================================================================================*/

u32
component_unit_mem_bytes( void )
{
    return 0;
}

/*============================================================================================*/
