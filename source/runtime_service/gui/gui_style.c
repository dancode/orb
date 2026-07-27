/*==============================================================================================

    runtime_service/gui/gui_style.c -- GUI_STYLE translation unit: style resolution.

    The first library over the interact server: interact-state flags in,
    colors / metrics out; NEVER paints.  It owns the theme registry, the active scaled style,
    the push/pop/next style stacks, the grid lattice, and the state -> color projections the
    layers above paint with.  The COL_* / WIDGET_* vocabulary macros (style/gui_style.h)
    resolve through this unit, so every read site above honors a stack override for free.

    PURITY: resolution takes interact state as PARAMETERS -- col_item_bg( st ),
    col_frame_bg( st, idle ) -- never queried from the interact server, so this unit is usable
    for HUD theming with no interact server present.  The one exception rides core's anim
    utility EXPLICITLY: col_item_bg_anim( id, st ) keys a damper through gui_anim4 (a keyed-
    state tenant) -- a deliberate, documented core dependency, not a hidden query.

    This unit includes NO render header and calls NO draw_* routine (the acceptance
    criterion): applying a resolved value to the draw state (alpha, rounding) is the impure
    wrappers' job (item_flags_resolve / item_flags_chrome_reset, stock/gui_adornment.c).

    Documented upward seam (the strata bridge -- see style/gui_style.h):
      - gui_theme_reset calls gui_style_apply (frame/gui_frame_font.c): the rescale needs the
        active font's metrics (draw unit), which style itself must not touch.

    The style used to reach UP into the stock unit for the installed palette and its
    projection table.  It does not any more: gui_style_t IS the installed layout, so the whole
    schema lives here and stock reads it back down through style_col.

    Include order matters: each file can reference statics from files included above it.

    style/gui_theme.c       -- theme registry, base/active style state (s_style_base, s_style),
                               theme API, the grid lattice, metrics_compute (the em rescale)
    style/gui_style_block.c -- the value backend: the block registry and the store / work
                               arrays every style slot lives in, whatever its vocabulary
    style/gui_style_core.c  -- the stacks machinery over one registered block: push/pop/next
                               resolution, the item/chrome seam hooks, state -> color projections
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
    Unity build -- theme state first (the stacks re-seed from s_style), then the stack
    machinery over it, then the public bracketing vocabulary over both.
==============================================================================================*/

#include "runtime_service/gui/style/gui_theme.c"
#include "runtime_service/gui/style/gui_style_block.c"
#include "runtime_service/gui/style/gui_style_core.c"
#include "runtime_service/gui/style/gui_stacks.c"

/*==============================================================================================
    Decentralized memory accounting -- this unit's fixed statics, read by gui_ui_memory
    (gui_ui_mem.c): the base + active style, the theme table (.rdata), the block backend
    (registry + store + work set), and the stack / override-pair tables.
==============================================================================================*/

u32
style_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_style_base ) + sizeof( s_style ) + sizeof( k_themes )
                + sizeof( s_block ) + sizeof( s_store ) + sizeof( s_work )
                + sizeof( s_col_stack ) + sizeof( s_var_stack )
                + sizeof( s_next ) + sizeof( s_item )
                + sizeof( s_set_stack ) + sizeof( s_set_source ) + sizeof( s_set_user ) );
}

/*============================================================================================*/
