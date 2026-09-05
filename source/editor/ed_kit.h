#ifndef ED_KIT_H
#define ED_KIT_H
/*==============================================================================================

    editor/ed_kit.h -- the editor KIT (S3): the editor's own idiom over the gui stack.

    Reference consumer of the kit tier, sibling to the game kits
    (project/sample_game/game_ui.h, sb_gui_diablo/ui.h).  Where a game kit is
    rect-first, the editor's idiom is the PROPERTY ROW: a label column on the left, the
    value editor filling the rest -- the shape every inspector, options pane, and tool
    window in the editor shares.  Asset pickers and friends land here as they appear.

    A prop row composes the whole stack in one bracket: flow_cell takes the row from the
    ambient window flow (rect PRODUCER), rect math cuts the label column, stock_label fills
    it (rect CONSUMER), and flow_begin re-opens the flow inside the value zone so any
    STOCK widget drops in unchanged:

        ed_prop_begin( "config" );
            gui()->combo( "##config", &idx, items, n );    // fills the value zone
        ed_prop_end();

        ed_prop_text( "project", name );                   // read-only row, one call

    Give embedded stock widgets "##id" labels -- the row label already names the property.

==============================================================================================*/

#include "runtime_service/gui/gui_api.h"

// clang-format off

void ed_prop_begin( const char* label );                   // label column + flow in the value zone
void ed_prop_end  ( void );                                // close the value-zone flow
void ed_prop_text ( const char* label, const char* value );// read-only row: dim label, bright value

// clang-format on
/*============================================================================================*/
#endif    // ED_KIT_H
