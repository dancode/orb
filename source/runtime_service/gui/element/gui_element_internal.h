#ifndef GUI_ELEMENT_INTERNAL_H
#define GUI_ELEMENT_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/element/gui_element_internal.h -- the element unit's cross-unit seams.

    The element unit (root gui_element.c) is the first layer astride both servers:
    styled paint over interact state.  Its PUBLIC surface (the el_* cores, gui_el_style_t)
    stays in the root gui_element.h; this header holds only what crosses a unit boundary
    inside the system -- the per-item wrappers the emit/chrome seams call, the styled
    painters the widgets and chrome compose, and the unit's memory seam.

    Not here, deliberately: the adornments invoked from BELOW are declared in their
    consumer's documented upward-seam block -- draw_nav_ring (core/gui_core.h),
    draw_drop_ring (interact/gui_interact.h) -- and the element bridge the style unit rides
    (el_style_derive, g_gui_el_slot_map) stays in style/gui_style.h.  One decl, one home.

==============================================================================================*/

// clang-format off

/* Per-item ambient application (element/gui_adornment.c): item_flags_resolve at the cell emit
   seam; item_flags_chrome_reset at every chrome seam -- wrappers over the interact server's
   pure halves.  Declared in flow/gui_flow.h: flow is their lowest consumer (its
   emit / region seams drive them). */

/* Labeled-row paint half (element/gui_adornment.c): the geometry lives with the composer
   (cell_split_field, flow/gui_flow.h); the label paint + the WIDGET_PAD self-measure here. */
gui_rect_t draw_field_label( gui_rect_t row, const char* label, f32 min_control_w,
                             u32 label_color );
f32        label_natural_w ( const char* s );

/* System adornments (element/gui_adornment.c) invoked from below across documented upward
   seams are declared with their LOWEST consumer: draw_nav_ring (core/gui_core.h),
   draw_drop_ring (interact/gui_interact.h), the child box pair + resize highlight
   (flow/gui_flow.h's upward block).  Only the focus border -- consumed from chrome's
   window ends, ABOVE this unit -- is declared here. */
void draw_window_focus_border( gui_rect_t r );

/* The styled half of the symbol palette (element/gui_symbol_style.c): emitters that resolve
   their own look (style-var picks, WIN_BORDER, ROUND_WIDGET) over the draw unit's pure ones.
   Their public wrappers (gui_draw_arrow / gui_draw_close / gui_draw_frame) are in gui_host.h. */
void draw_arrow          ( gui_rect_t box, gui_dir_t dir, u32 color );
void draw_collapse_arrow ( gui_rect_t box, bool collapsed, u32 color );
void draw_close_x        ( gui_rect_t box, u32 color );
void draw_check_indicator( gui_rect_t box, u32 col );
void draw_rule           ( f32 x, f32 yc, f32 w, f32 thickness, u32 col );

u32 gui_element_unit_mem_bytes( void );          /* the element unit's fixed statics */

// clang-format on
/*============================================================================================*/
#endif    // GUI_ELEMENT_INTERNAL_H
