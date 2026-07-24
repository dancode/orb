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

    THE CALL SHAPE.  Every component opens ( id, rect, ... ): identity first, the rect it works
    over second, then whatever that widget needs.  A parameter-rich component adds an _ex twin
    taking a desc struct (comp_slider / comp_slider_ex), never replaces the positional form --
    so learning one component teaches the signature of all of them.

    THE RESULT SHAPE.  Every gui_comp_*_t opens with `gui_item_state_t state`, then any geometry
    a render paints, then only the outcomes state does not already carry (changed / enter -- never
    a second spelling of state.clicked).  Because state sits at offset 0 for all of them, one
    render idiom works over every component:

        u32 face = gui()->el_color( GUI_EL_BG, gui()->item_phase( x.state ) );

    THE COMPONENTS (each has a reference render gui_stock_* in stock/gui_element_core.c; all
    public via gui_host.h / the vtable; a user widget is their sibling over the same comp_* call):
      - comp_slider -- state + fraction + bar/handle rects.  Absolute-position mapping (handle
        center = cursor).  The parameter-rich one, hence the _ex desc form.
      - comp_button -- the simplest, which SETTLED the shared shape: its outcome IS state.clicked,
        so it adds no field at all.  A pure id; the label is the render's.
      - comp_check -- toggle over an inscribed box; returns { state, box, changed }.
      - comp_cycle -- a "< value >" stepper that COMPOSES comp_button for each cap (the shared
        press core stacks); takes count for wrap, not the strings.  Its own .state covers the
        whole stepper, with .prev / .next carrying the per-cap faces.
      - comp_selectable -- comp_button + an optional *selected toggle (composition again).
      - comp_input -- the richest: runs the interact/ edit engine (edit_field) and returns
        PAINTABLE geometry (content rect, run origin, selection + caret bars) so a render draws a
        text field with only public verbs, never touching gui_edit_state_t or measuring a glyph.

    NO component (deliberately): stock_panel / stock_label / stock_meter are inert paint -- no
    interaction, no logic to extract -- so they stay render-only.  Not every widget needs one.

    NEXT: chrome's own widgets may migrate onto these components when a chrome-only feature is
    needed in a user widget (the migrate-down direction); until then chrome stays bespoke.

==============================================================================================*/

// clang-format off

u32 gui_component_unit_mem_bytes( void );        /* the component unit's fixed statics (none yet) */

// clang-format on
/*============================================================================================*/
#endif    // GUI_COMPONENT_INTERNAL_H
