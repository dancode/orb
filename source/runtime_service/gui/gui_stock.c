/*==============================================================================================

    runtime_service/gui/gui_stock.c -- GUI_STOCK translation unit: the reference widget set.

    This is where a widget finally becomes something you can see: component logic (is it
    hovered, what's its current value) plus a theme's colors and metrics, combined into an
    actual filled rectangle on screen. Everything below this unit is either pure logic
    (component, interact) or pure lookup (style resolves a color but never draws it, draw
    knows how to draw but never asks "what color should this be") -- stock is the one place
    those two halves meet.

    Where it sits in the stack (see GUI_ARCHITECTURE.md):

        state/interact  ->  component  ->  stock  ->  chrome
                            (logic)       (this)     (product)

    A stock widget is a component's logic plus one plain render of it. It exists to be READ and
    FORKED, not used blindly: a game that wants its own art writes "my_game_slider" as the same
    slider component with its own paint instead of this one, and neither is treated as more
    correct than the other. Most stock_* renders drive a matching component underneath; the
    three with no interaction of their own (a plain panel, a label, a progress meter) skip that
    step since there is no state to track.

    Naming: "stock_" names the widget set itself; "style_" names the look it paints with (a
    color/metric lookup keyed by role and phase -- idle, hovered, pressed, and so on). Chrome
    uses the exact same style_ vocabulary, so a theme applies uniformly everywhere.

    Four constituents, four faces of the same role:

    stock/gui_stock_widgets.c  -- the public stock_* renders: fill EXACTLY the rect they are
                                     handed, and resolve every look through style_col /
                                     style_var (the style unit owns the storage and the
                                     theme compile that fills it)
    stock/gui_adornment.c     -- per-item ambient application (item_flags_resolve /
                                     item_flags_chrome_reset), the label paint
                                     (gui_field_row, label_natural_w), and the system
                                     adornments the units below invoke across their
                                     documented upward seams (nav ring, focus border, drop
                                     ring, child box, resize highlight)
    stock/gui_symbol_style.c  -- the styled half of the symbol palette (arrow, check
                                     indicator, rule, close, frame): emitters that resolve
                                     their own look from the live style, over the draw
                                     unit's parameter-pure emitters
    stock/gui_face.c          -- the FACE painters: fill a rect for a style CELL, using the
                                     cell's brush when the theme authored one and its flat
                                     colour when it did not.  The paint half of the face
                                     plane -- style resolves a brush and never emits, this
                                     emits one; every widget's surface fill goes through it,
                                     which is what lets a theme install art and have the
                                     whole widget set pick it up untouched

    Cross-unit declarations live in stock/gui_stock_internal.h (the umbrella slot between flow
    and chrome); the public stock_* surface is the GUI_STOCK band of gui_api.h, and the style
    grid it paints from is gui.h's -- the same one chrome reads.

==============================================================================================*/

#include <math.h>     /* floorf -- the symbol glyph metrics */
#include <string.h>

#include "orb.h"
#include "base/fmt.h"

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Astride both servers by definition: the render header (it paints) AND the interact stack
   (it reads state as parameters) -- the whole sub-stack below stock, and no chrome. */

#include "runtime_service/gui/render/gui_render.h"    /* the render server's push primitives
                                                         (pulls gui_host.h + rhi/app APIs)  */
#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/stock/gui_stock_internal.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*============================================================================================*/

/* Symbol style first: gui_face.c paints through gui_draw_frame, and the widget renders paint
   through both. */
#include "runtime_service/gui/stock/gui_symbol_style.c"
#include "runtime_service/gui/stock/gui_face.c"
#include "runtime_service/gui/stock/gui_stock_widgets.c"
#include "runtime_service/gui/stock/gui_adornment.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  Nothing left to count: the style's storage lives in the style unit's block
    backend, and the renders themselves are stateless.
==============================================================================================*/

u32
stock_unit_mem_bytes( void )
{
    return 0u;
}

/*============================================================================================*/
