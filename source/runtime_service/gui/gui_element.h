#ifndef GUI_ELEMENT_H
#define GUI_ELEMENT_H
/*==============================================================================================

    runtime_service/gui/gui_element.h -- the element axis: what a color is FOR, and when.

    THE color vocabulary of the whole GUI.  There is no second one: chrome, the stock widgets,
    and a kit's own renders all name a color as a (role, state) pair, and all of them resolve
    through the same instanced style (gui_style_t, gui.h) -- so "the editor look" and "the game
    look" are two instances of one schema rather than two schemas.

    Five roles x four states = the 20 cells a theme authors and a style source overwrites.
    Deliberately NO per-widget slots (btn_bg_hover, slot_border_hot, ...): per-widget color is
    either a call parameter (stock_meter's fill) or a token in the kit above.

    Two doors, and the difference matters: gui()->el_color( role, state ) is the RESOLVED read
    (push_style_color / next_style_color overrides win) -- use it in any render, stock or your
    own.  gui()->style_edit() is the raw installed struct of the CURRENT set, for a kit
    INSTALLING a look; reading ->col[][] through it at paint time bypasses the style stack.

    Naming: el_ is this axis; the widget set that paints from it is stock_ (gui_api.h,
    GUI_STOCK).  A user widget is a stock render's sibling and reads the same cells.

==============================================================================================*/

#include "runtime_service/gui/rect/gui_rect.h"

// clang-format off

/* What the color is FOR.  Five roles cover every surface the GUI paints.

   PANEL vs BG is the container / control split, and it is the one distinction that has to exist:
   a window body, a child region, and a title bar are surfaces the layout CARVES, while a button
   face, an input field, and a check box are surfaces a widget FILLS.  They recede and advance in
   opposite directions (a panel sits under its contents, a control sits over its panel), so one
   shared "background" cannot serve both. */
typedef enum
{
    GUI_EL_PANEL = 0,     // container surface: window body, child region, title bar
    GUI_EL_BG,            // control surface: button face, input field, check box, cycle end caps
    GUI_EL_BORDER,        // frame line, focus ring, resize edge
    GUI_EL_TEXT,          // glyphs, caret
    GUI_EL_ACCENT,        // emphasis: marks, value fills, nav highlight
    GUI_EL_ROLE_COUNT

} gui_el_role_t;

/* Which interaction state selects the color.  The same four steps mean the analogous thing for
   every role, which is what lets one 5x4 grid replace the old flat palette plus its token
   residue:

     role     IDLE              HOT                  ACTIVE                 DIM
     -------  ----------------  -------------------  ---------------------  ------------------
     PANEL    window body       title bar            focused title bar      child / recessed
     BG       control face      hovered face         pressed / focused      inert face
     BORDER   frame line        hovered / resize     focused window ring    subdued frame
     TEXT     body text, caret  text on a hot face   text on a pressed one  secondary text
     ACCENT   value fill        nav highlight        mark, captured nav     empty track

   DIM doubles as the inert variant throughout: a recessed panel is PANEL[DIM], an empty value
   track is ACCENT[DIM], secondary text is TEXT[DIM]. */
typedef enum
{
    GUI_EL_IDLE = 0,      // at rest
    GUI_EL_HOT,           // cursor over / keyboard nav on the item
    GUI_EL_ACTIVE,        // pressed / captured / focused
    GUI_EL_DIM,           // inert, disabled, de-emphasized, recessed
    GUI_EL_STATE_COUNT,

    /* Not a cell -- the "whole row" selector push_style_color takes, so recoloring TEXT or BORDER
       is one balanced push instead of four.  Only the push / next verbs accept it; a read names
       one state. */
    GUI_EL_ALL = GUI_EL_STATE_COUNT

} gui_el_state_t;

// clang-format on
/*============================================================================================*/
#endif    // GUI_ELEMENT_H
