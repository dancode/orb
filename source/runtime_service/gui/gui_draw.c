/*==============================================================================================

    runtime_service/gui/gui_draw.c -- GUI_DRAW translation unit: the drawing-routine library.

    Drawing and shapes over the RENDER SERVER's push primitives: the
    rect-taking paint floor, the fitted text painters, the symbol/shape palette, the canvas
    (the user door to 2d drawing), and the FONT + ICON resources -- glyph metrics and baking
    live here, one level above the server, writing into the shared atlas they hand down
    (the server renders from a pushed atlas; it does not know what a font is).

    Downward only: this unit calls the render server (draw_push_*, res_atlas_*) and reads the
    style vocabulary for the few styled sites.  The server
    calls BACK only through the glyph/sprite source contract (render/gui_render.h): the
    tessellator resolves glyph UVs and the emit layer resolves icon UVs against the tables
    this unit installs and manages.

    Constituents (draw/), in include order:
        gui_glyph_internal.c / gui_glyph.c   -- glyph atlas upload + UV dispatch (the metrics half
                                                 of fonts is the font/ leaf, below this unit)
        gui_icon.c / gui_icon_load.c         -- icon registry + PNG -> R8 loader
        gui_paint.c                          -- paint floor + fitted text painters
        gui_symbol.c                         -- symbol marks + shape palette + gui_draw_* surface
        gui_canvas.c                         -- custom-draw placement/metric/hit-test API

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Drawing routines over the render server's primitives -- parameter-pure, so no
   core, style, or policy header belongs here. */
#include "runtime_service/gui/render/gui_render.h" /* the render server's primitive surface
                                                      (pulls gui_host.h + rhi/app APIs)     */
#include "runtime_service/gui/render/resource/gui_res_atlas.h" /* the atlas push API fonts/icons write through */
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*==============================================================================================
    Unity build -- resources first (the palette and canvas draw with the active font's
    metrics), paint floor before symbol (symbol composes fill/outline).
==============================================================================================*/

#include "runtime_service/gui/draw/gui_glyph_internal.c"
#include "runtime_service/gui/draw/gui_glyph.c"
#include "runtime_service/gui/draw/gui_icon.c"
#include "runtime_service/gui/draw/gui_icon_load.c"

#include "runtime_service/gui/draw/gui_paint.c"
#include "runtime_service/gui/draw/gui_symbol.c"
#include "runtime_service/gui/draw/gui_canvas.c"

/*==============================================================================================
    Unit lifecycle -- called by the frame orchestrator (frame/gui_frame_loop.c) AFTER the render
    server stands up (fonts and icons register into the server's shared atlas) and torn down
    before it.
==============================================================================================*/

bool
gui_draw_boot( void )
{
    if ( !font_init() )
        return false;

    /* Icons are a layer over the shared atlas -- registration API stood up here so it can
       register into the atlas the render server just created.  The built-in icon set loads
       from disk in one pass; non-fatal: a missing file leaves that name unregistered. */
    if ( !icon_atlas_init() )
    {
        font_shutdown();
        return false;
    }
    icon_load_builtins();

    return true;
}

void
gui_draw_shutdown( void )
{
    icon_atlas_shutdown();
    font_shutdown();
}

/* The draw unit's fixed statics, for the decentralized memory accounting: the icon tables (the
   loaded-font registry + its resident pixels are the font/ resource's, counted there). */
u32
draw_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_icons ) + sizeof( s_builtin_icons ) );
}

/*============================================================================================*/
