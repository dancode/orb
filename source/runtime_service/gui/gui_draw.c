/*==============================================================================================

    runtime_service/gui/gui_draw.c -- The drawing-routine library.

    A toolbox of ready-made drawing routines built over the render server's raw push 
    primitives: fill a rect, draw a line of text so it fits inside a given width, draw one
    of a small built-in set of symbols (an arrow, a checkmark), and a general-purpose "canvas"
    for a caller who wants to draw custom 2d shapes of their own. This is also where fonts and
    icons actually load and get packed into the shared texture the render server draws from 
    
    -- the render server itself has no idea what a font or an icon IS, it only knows how to 
       draw from an atlas someone else filled in.

    This unit only calls DOWN into the render server, never up into anything that knows about
    widgets or user interaction. The one thing that goes the other direction is deliberate: 
    while the render server is turning text or icons into triangles, it asks this unit 
    (through a small documented callback) where in the shared texture a given glyph or icon 
    actually lives.

    Constituents (draw/), in include order:

        gui_glyph_internal.c            -- glyph atlas upload + UV dispatch (the metrics half
        gui_glyph.c                        of fonts is the font/ leaf, below this unit)

        gui_glyph_table.c               -- ID-indexed glyph UV table the vertex stage reads
        gui_icon.c / gui_icon_load.c    -- icon registry + PNG -> R8 loader
        gui_sprite.c                    -- sprite registry + nine-slice + PNG -> RGBA loader
                                           (shares gui_icon_load.c's stb_image + file slurp)
        gui_paint.c                     -- paint floor + fitted text painters
        gui_symbol.c                    -- symbol marks + shape palette + gui_draw_* surface
        gui_canvas.c                    -- custom-draw placement/metric/hit-test API

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"
#include "base/math.h"
#include "base/utf8.h"   // codepoint stepping on the fitted-text measure seam

/*==============================================================================================
    Draw : Dependencies
==============================================================================================*/

/* This unit's world, and nothing above it (the include list IS the dependency graph).
   Drawing routines over the render server's primitives -- parameter-pure, so no
   core, style, or policy header belongs here. */

/* the render server's primitive surface */
#include "runtime_service/gui/render/gui_render.h" 

/* the atlas push API fonts/icons write through */
#include "runtime_service/gui/render/resource/gui_res_atlas.h" 

/*==============================================================================================
    Draw : Defines
==============================================================================================*/

#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/debug/gui_debug.h"

// icons and sprites are read by resource name through fs
#include "runtime_service/gui/gui_res.h"    

/*==============================================================================================
    Draw : Unity build
==============================================================================================*/

#include "runtime_service/gui/draw/gui_glyph_internal.c"
#include "runtime_service/gui/draw/gui_glyph_table.c"
#include "runtime_service/gui/draw/gui_glyph.c"
#include "runtime_service/gui/draw/gui_sdf_bake.c"
#include "runtime_service/gui/draw/gui_icon.c"
#include "runtime_service/gui/draw/gui_icon_load.c"
#include "runtime_service/gui/draw/gui_shape.c"
#include "runtime_service/gui/draw/gui_sprite.c"

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

    /* Baked shapes are the same layer over the SDF atlas, and stand up with the icons for the same
       reason: registration has to be legal from the moment an app can call it. */
    shape_init();

    return true;
}

void
gui_draw_shutdown( void )
{
    sprite_registry_shutdown();
    shape_shutdown();
    icon_atlas_shutdown();
    font_shutdown();
}

/* The draw unit's fixed statics, for the decentralized memory accounting: the icon tables (the
   loaded-font registry + its resident pixels are the font/ resource's, counted there; the two
   ATLASES are the render server's and counted there). */

u32
draw_unit_mem_bytes( void )
{
    return (u32)( sizeof( s_icons ) + sizeof( s_builtin_icons ) + sizeof( s_sprites ) );
}

/*============================================================================================*/
