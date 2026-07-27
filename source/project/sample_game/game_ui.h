#ifndef SAMPLE_GAME_UI_H
#define SAMPLE_GAME_UI_H
/*==============================================================================================

    project/sample_game/game_ui.h -- the game KIT (S3): how THIS game shows things.

    The reference kit for a project DLL over gui's element tier (GUI_STACK_PLAN increment 6;
    sb_gui_diablo/ui.h is the richer standalone-loop sibling).  Everything here follows the
    kit shape the stack was built for:

      - RECTS ARE THE CURRENCY: the HUD bracket hands back the screen as a rect; the caller
        cuts and places with gui_rect_* math, then fills rects with stock_* renders.
      - THE KIT OWNS THE LOOK: game_ui_install compiles the project's palette into gui's
        style (gui()->style_edit), so the stock renders paint this game's accent.  The
        theme system re-derives the installed style at every theme / font landing, so
        install after those -- for this host shape, once at on_start is enough.
      - SOFT GUI: the project runs under gui-less hosts too (sb_host_game).  game_ui_wire
        fetches the gui api without failing init; every entry no-ops until it lands, and
        on_hud is only ever driven by hosts that HAVE a gui bracket anyway.

    Emission is legal ONLY inside on_hud (runtime/run_project.h) -- the one project phase
    the host calls from inside its gui frame bracket.

==============================================================================================*/

#include "engine/mod/mod_export.h"   /* get_api_fn */
#include "runtime_service/gui/gui_api.h"

// clang-format off

bool game_ui_wire ( get_api_fn get_api );   // soft gui fetch (init/reload); false = gui-less host, fine
bool game_ui_ready( void );                 // gui landed? every other entry no-ops when false

void game_ui_install( void );               // compile the kit palette into the style (S3 -> S1)

/* HUD surface bracket: a chrome-free, non-scrolling fullscreen region on gui_vp (below any
   caption band), returned as THE root rect all HUD math cuts from.  Always pair. */
gui_rect_t game_ui_hud_begin( i32 gui_vp );
void       game_ui_hud_end  ( void );

f32  game_ui_u( f32 n );                    // n basis units in px (basis = active font line height)

/* The game's readouts -- each fills exactly the rect it is given. */
void game_ui_score     ( gui_rect_t r, i32 score );   // framed score panel
void game_ui_tick_meter( gui_rect_t r, f32 frac );    // progress to the next score tick

// clang-format on
/*============================================================================================*/
#endif    // SAMPLE_GAME_UI_H
