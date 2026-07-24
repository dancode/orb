#ifndef GUI_COMPONENT_INTERNAL_H
#define GUI_COMPONENT_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/component/gui_component_internal.h -- the component unit's cross-unit seams.

    THE COMPONENT TIER (STAGING).  A component is a widget's LOGIC with no look: it consumes an
    (id, rect) and does the tedious part -- hit-testing, drag math, value snapping, focus /
    hover / active state -- then hands back clear outputs.  It never draws.  A component does
    NOT exist on screen; it is a utility front-end a widget composes onto.  In the engine's own
    vocabulary (source/game world/entity/component/actor) a widget is an actor and its
    components are logic aspects that do not independently exist.

        state/interact  ->  component  ->  stock  ->  chrome
                            (this)        (render)   (product)

    A stock widget (gui_stock.c) and a user widget (my_game_slider) are SIBLINGS: each is one
    render over the same component.  That is the whole point of the stack -- zero one-size-fits-
    all widgets, user-driven presentation over shared logic.

    DESIGN IS ITERATIVE.  The component API shape -- what each gui_comp_*() takes and the output
    struct it returns -- is being settled one widget at a time, starting from the slider (richest
    logic seam) and the button (simplest case).  The proven pattern lives in chrome today and
    migrates DOWN into components only once a real widget needs it.

    THE COMPONENTS (each has a reference render gui_stock_* in stock/gui_element_core.c that the
    matching gui_el_* now delegates to; all public via gui_host.h / the vtable; a user widget is
    their sibling over the same comp_* call):
      - comp_slider -- ONE desc (gui_comp_slider_desc_t) -> state + bar/handle rects; the only
        parameter-rich one, hence the desc.  Absolute-position mapping (handle center = cursor).
      - comp_button -- the simplest, which SETTLED the shared shape: a component returns a
        gui_item_state_t plus its semantic outcome, any geometry between.  Plain (id, rect); a
        pure id, the label is the render's.
      - comp_check -- toggle over an inscribed box; returns { state, box, changed }.
      - comp_cycle -- a "< value >" stepper that COMPOSES comp_button for each cap (the shared
        press core stacks); takes count for wrap, not the strings.
      - comp_selectable -- comp_button + an optional *selected toggle (composition again).
      - comp_input -- the richest: runs the interact/ edit engine (edit_field) and returns
        PAINTABLE geometry (content rect, run origin, selection + caret bars) so a render draws a
        text field with only public verbs, never touching gui_edit_state_t or measuring a glyph.

    NO component (deliberately): el_panel / el_label / el_meter are inert paint -- no interaction,
    no logic to extract -- so they stay stock-render-only.  Not every core needs a component.

    NEXT: chrome's own widgets may migrate onto these components when a chrome-only feature is
    needed in a user widget (the migrate-down direction); until then chrome stays bespoke.

==============================================================================================*/

// clang-format off

u32 gui_component_unit_mem_bytes( void );        /* the component unit's fixed statics (none yet) */

// clang-format on
/*============================================================================================*/
#endif    // GUI_COMPONENT_INTERNAL_H
