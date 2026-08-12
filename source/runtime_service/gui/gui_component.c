/*==============================================================================================

    runtime_service/gui/gui_component.c -- GUI_COMPONENT translation unit: widget logic (STAGING).

    A widget usually has two separable halves: the LOGIC (where's the mouse, did it click,
    what value should the slider now show) and the LOOK (what color, what shape). This unit is
    the logic half, with no look at all. A component takes an id and a rect and does the
    fiddly, easy-to-get-wrong work -- hit-testing, drag math, snapping a value to a step -- and
    reports back a plain result: hovered? clicked? new value? It never draws anything, which is
    why it sits below the drawing units in the dependency stack: it physically cannot call a
    paint function even if it wanted to.

    The point is that the same logic should be reusable under any number of different looks.
    The engine's own reference widgets (stock/) render one look over this logic, and a game can
    write its own renderer with completely different art over the exact same logic, with
    neither one favored over the other.

        state/interact  ->  component  ->  stock  ->  chrome
                            (this unit)   (render)   (product)

    See component/gui_component_internal.h for the shape every component follows: the
    parameters (id, rect, ...) it takes, and the result -- starting with a shared "what is the
    user doing" state -- it returns. Six components live here today (slider, button, checkbox,
    cycle button, selectable row, text input), and the engine's whole interactive stock_*
    widget set renders over them. chrome/'s widgets have not been migrated onto this yet and
    keep their own bespoke logic for now.

==============================================================================================*/

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
