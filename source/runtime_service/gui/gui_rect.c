/*==============================================================================================

    runtime_service/gui/gui_rect.c -- GUI_RECT translation unit: the leaf rect library.

    The bottom of the gui stack (GUI_SERVER_PLAN.md): pure geometry + color primitives with
    no dependency beyond base types.  No draw, no ids, no ambient state -- usable by any 2d
    utility, inside gui or out.  The inline kit (types + carve algebra) is rect/gui_rect.h;
    this unit compiles the non-inline half.

    Constituents (rect/):
        gui_rect_core.c   -- color blend + alignment placement primitives

==============================================================================================*/

#include "orb.h"

#include "runtime_service/gui/rect/gui_rect.h"

#include "runtime_service/gui/rect/gui_rect_core.c"

/*============================================================================================*/
