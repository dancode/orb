/*==============================================================================================

    runtime_service/gui/gui_element.c -- GUI_ELEMENT translation unit: styled building blocks.

    THE FIRST LAYER ASTRIDE BOTH SERVERS: everything that combines
    interact state with styled paint over a caller-supplied rect.  Below it, style resolves
    and never emits, draw emits and never resolves; element is where the two meet -- the
    rect CONSUMER over the rects flow carves.

    Three constituents, three faces of the same role:

    element/gui_element_core.c  -- the public el_* cores: fill EXACTLY the rect they are
                                     handed, read ONLY the installed element style
                                     (gui_el_style_t; el_style_derive compiles the theme in)
    element/gui_adornment.c     -- per-item ambient application (item_flags_resolve /
                                     item_flags_chrome_reset), the label paint
                                     (field_row, label_natural_w), and the system
                                     adornments the units below invoke across their
                                     documented upward seams (nav ring, focus border, drop
                                     ring, child box, resize highlight)
    element/gui_symbol_style.c  -- the styled half of the symbol palette (arrow, check
                                     indicator, rule, close, frame): emitters that resolve
                                     their own look from the live style, over the draw
                                     unit's parameter-pure emitters

    Cross-unit declarations live in element/gui_element_internal.h (the umbrella slot
    between flow and chrome); the public el_* surface stays in the root gui_element.h.

==============================================================================================*/

#include <math.h>     /* floorf -- the symbol glyph metrics */
#include <string.h>

#include "orb.h"
#include "base/fmt.h"

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Astride both servers by definition: the render header (it paints) AND the interact stack
   (it reads state as parameters) -- the whole sub-stack below element, and no chrome. */

#include "runtime_service/gui/render/gui_render.h"    /* the render server's push primitives
                                                         (pulls gui_host.h + rhi/app APIs)  */
#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/element/gui_element_internal.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*============================================================================================*/

#include "runtime_service/gui/element/gui_element_core.c"
#include "runtime_service/gui/element/gui_adornment.c"
#include "runtime_service/gui/element/gui_symbol_style.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c).  The installed element style + the role x state slot map.
==============================================================================================*/

u32
gui_element_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_el_style ) + sizeof( g_gui_el_slot_map ) );
}

/*============================================================================================*/
