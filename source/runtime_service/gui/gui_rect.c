/*==============================================================================================

    runtime_service/gui/gui_rect.c -- GUI_RECT translation unit: the leaf rect library.

    The most basic building block in the whole GUI: a rectangle (x, y, width, height) and the
    handful of plain math operations every other unit does with one -- does this point fall
    inside it, split it into two pieces, shrink it by a margin, blend two colors together. No
    widget, no window, no on-screen drawing happens here; this file only does the arithmetic
    those things are built from.

    Because it depends on nothing but basic types, it sits at the very bottom of the gui stack
    (every other unit can use it; it uses none of them) and could just as easily be used by
    code outside the GUI entirely. The inline half -- the rect type itself and the small carve
    operations that are cheap enough to inline -- lives in rect/gui_rect.h; this file compiles
    the rest.

==============================================================================================*/

#include "orb.h"

#include "runtime_service/gui/rect/gui_rect.h"
#include "runtime_service/gui/rect/gui_rect_core.c"     // color blend + align placement

/*============================================================================================*/
