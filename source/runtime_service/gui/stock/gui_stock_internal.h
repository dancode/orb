#ifndef GUI_STOCK_INTERNAL_H
#define GUI_STOCK_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/stock/gui_stock_internal.h -- the stock unit's cross-unit seams.

    The stock unit (root gui_stock.c) is the first layer astride both servers: styled paint
    over interact state.  Its PUBLIC surface (the stock_* renders, plus the el_ style stratum
    gui_el_style_t / el_color) stays in gui_element.h and the GUI_STOCK band of gui_api.h;
    this header holds only what crosses a unit boundary inside the system -- the per-item
    wrappers the emit/chrome seams call, the styled painters the widgets and chrome compose,
    and the unit's memory seam.

    Not here, deliberately: the adornments invoked from BELOW are declared in their
    consumer's documented upward-seam block -- draw_nav_ring (core/gui_core.h),
    draw_drop_ring (interact/gui_interact.h) -- and the style bridge this unit rides
    (el_style_derive, g_gui_el_slot_map) stays in style/gui_style.h.  One decl, one home.

==============================================================================================*/

// clang-format off

/* Per-item ambient application (stock/gui_adornment.c): item_flags_resolve at the cell emit
   seam; item_flags_chrome_reset at every chrome seam -- wrappers over the interact server's
   pure halves.  Declared in flow/gui_flow.h: flow is their lowest consumer (its
   emit / region seams drive them). */

/* Label paint (stock/gui_adornment.c): gui_field_row draws a labeled widget's own label per the
   ambient gui_field_t (geometry = field_geom_split, flow/gui_flow.h; declared in gui_host.h).
   label_natural_w is the self-measure the button family shares. */
f32        label_natural_w ( const char* s );

/* System adornments (stock/gui_adornment.c) invoked from below across documented upward
   seams are declared with their LOWEST consumer: draw_nav_ring (core/gui_core.h),
   draw_drop_ring (interact/gui_interact.h), the child box pair + resize highlight
   (flow/gui_flow.h's upward block).  Only the focus border -- consumed from chrome's
   window ends, ABOVE this unit -- is declared here. */
void draw_window_focus_border( gui_rect_t r );

/* The styled half of the symbol palette (stock/gui_symbol_style.c): emitters that resolve
   their own look (style-var picks, WIN_BORDER, ROUND_WIDGET) over the draw unit's pure ones.
   Their public wrappers (gui_draw_arrow / gui_draw_close / gui_draw_frame) are in gui_host.h. */
void draw_arrow          ( gui_rect_t box, gui_dir_t dir, u32 color );
void draw_collapse_arrow ( gui_rect_t box, bool collapsed, u32 color );
void draw_close_x        ( gui_rect_t box, u32 color );
void draw_check_indicator( gui_rect_t box, u32 col );
void draw_rule           ( f32 x, f32 yc, f32 w, f32 thickness, u32 col );

u32 gui_stock_unit_mem_bytes( void );          /* the stock unit's fixed statics */

// clang-format on
/*============================================================================================*/
#endif    // GUI_STOCK_INTERNAL_H
