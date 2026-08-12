/*==============================================================================================

    runtime_service/gui/gui_style.c -- GUI_STYLE translation unit: style resolution.

    This is the theming layer: it turns a widget's current state -- idle, hovered, pressed,
    disabled -- into an actual color or size to paint with. Feed it "this is a button, and
    it's hovered" and it hands back the fill color a hovered button should use in the active
    theme. Like the interact server it sits above, this unit never draws a pixel itself; it
    only resolves values that something else (draw, stock) paints with afterward.

    It owns the theme itself (the full set of colors, sizes, and rounding a "look" is made
    of), tracks which theme is active and at what scale (so a 4K monitor gets bigger widgets
    than a 1080p one automatically), and provides the push/pop stacks that let a caller
    override a color or size temporarily -- "make just this one button red" -- without
    touching the base theme. Every color/metric macro other units use to ask "what should this
    look like" resolves through this unit, so a temporary override applies automatically no
    matter where in the code that macro is used.

    PURITY: this unit is deliberately kept independent of the interact server -- it takes a
    widget's state as a plain parameter (col_item_bg( state )) rather than asking the interact
    server for it, so it stays usable for theming a HUD or any UI that has no interact server
    running at all. The one exception is animated color blends (a hover fade, say): those key a
    small "how far along is this animation" value off the interact server's shared animation
    helper -- a deliberate, narrow dependency, not a hidden one.

    This unit never includes the render header and never calls a draw_* function: applying a
    resolved color or metric to an actual draw call is someone else's job
    (item_flags_resolve / item_flags_chrome_reset, stock/gui_adornment.c).

    Include order matters: each file can reference statics from files included above it.

    style/gui_bake.c        -- the bake: seven seeds and a five-number ramp -> the 32-cell colour
                               grid.  Pure, and depends on nothing above it, which is why it is
                               first: the theme registry bakes on the way in and the seed stack
                               re-bakes into the working run
    style/gui_theme.c       -- theme registry, base/active style state (s_style_base, s_style),
                               theme API, the grid lattice, metrics_compute (the em rescale)
    style/gui_style_core.c  -- the value store (one installed gui_style_t per set + the resolved
                               working run) and the stacks over it: push/pop/next resolution,
                               the item/chrome seam hooks, state -> color projections
    style/gui_stacks.c      -- the caller's bracketing vocabulary: push/pop id, item flags,
                               style color/var, scale ramp, disabled scope

==============================================================================================*/

#include <string.h>   /* strcmp -- theme name lookup */

#include "orb.h"

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Style resolves over the interact server -- no render, no draw. */
#include "runtime_service/gui/gui_host.h"       /* public gui types (-> gui.h -> rect)     */
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*==============================================================================================
    Unity build -- the pure bake first (theme load and the seed stack both call it), then theme
    state (the stacks re-seed from s_style), then the stack machinery over it, then the public
    bracketing vocabulary over both.
==============================================================================================*/

#include "runtime_service/gui/style/gui_bake.c"
#include "runtime_service/gui/style/gui_theme.c"
#include "runtime_service/gui/style/gui_style_core.c"
#include "runtime_service/gui/style/gui_stacks.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c): the base + active style, the theme table (.rdata), the installed store and
    its resolved working run, and the stack / override-pair tables.
==============================================================================================*/

u32
style_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_style_base ) + sizeof( s_style ) + sizeof( k_themes )
                + sizeof( s_store ) + sizeof( s_work )
                + sizeof( s_col_stack ) + sizeof( s_var_stack ) + sizeof( s_seed_stack )
                + sizeof( s_next ) + sizeof( s_item )
                + sizeof( s_set_stack ) + sizeof( s_set_source ) + sizeof( s_set_user ) );
}

/*============================================================================================*/
