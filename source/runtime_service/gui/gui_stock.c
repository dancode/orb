/*==============================================================================================

    runtime_service/gui/gui_stock.c -- GUI_STOCK translation unit: the reference widget set.

    THE STOCK TIER: the example widgets that combine interact state with styled paint over a
    caller-supplied rect.  This is the layer astride both servers -- below it, style resolves and
    never emits, draw emits and never resolves; stock is where the two meet, the rect CONSUMER
    over the rects flow carves.

    Where it sits in the stack (see GUI_ARCHITECTURE.md):

        state/interact  ->  component  ->  stock  ->  chrome
                            (logic)       (this)     (product)

    A stock widget = a component's logic + one plain render.  It is the reference a user forks
    to build their own look (my_game_slider = the same component + their art); it is NOT a
    privileged default.  Every interactive stock_* render drives a gui_comp_* logic core
    (source/runtime_service/gui/component/); the three inert ones (panel / label / meter) have
    no component because they have no interaction to extract.

    Naming: stock_ is the WIDGET SET; el_ is the STYLE STRATUM it paints from (gui_el_style_t,
    gui_el_color, GUI_EL_BG).  Two vocabularies on purpose.

    Three constituents, three faces of the same role:

    stock/gui_stock_widgets.c  -- the public stock_* renders: fill EXACTLY the rect they are
                                     handed, read ONLY the installed element style
                                     (gui_el_style_t; el_style_derive compiles the theme in)
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

    Cross-unit declarations live in stock/gui_stock_internal.h (the umbrella slot between flow
    and chrome); the public stock_* surface is the GUI_STOCK band of gui_api.h, and the el_
    style stratum it paints from stays in gui_element.h.

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

#include "runtime_service/gui/stock/gui_stock_widgets.c"
#include "runtime_service/gui/stock/gui_adornment.c"
#include "runtime_service/gui/stock/gui_symbol_style.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  The installed element style + the role x state slot map.
==============================================================================================*/

u32
stock_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_el_style ) + sizeof( g_el_slot_map ) );
}

/*============================================================================================*/
