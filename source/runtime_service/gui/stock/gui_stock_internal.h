#ifndef GUI_STOCK_INTERNAL_H
#define GUI_STOCK_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/stock/gui_stock_internal.h -- the stock unit's cross-unit seams.

    The stock unit (root gui_stock.c) is the first layer astride both servers: styled paint
    over interact state.  Its PUBLIC surface (the stock_* renders, plus the el_ style stratum
    el_color / the role x state axis) stays in gui_element.h and the GUI_STOCK band of gui_api.h;
    this header holds only what crosses a unit boundary inside the system -- the per-item
    wrappers the emit/chrome seams call, the styled painters the widgets and chrome compose,
    and the unit's memory seam.

    ONE DECL, ONE HOME -- so most of what this unit defines is NOT declared here.  Anything a
    lower unit invokes across its documented upward seam is declared with that LOWEST consumer:
    item_flags_resolve / item_flags_chrome_reset and the child-box trio in flow/gui_flow.h,
    draw_nav_ring in core/gui_core.h, draw_drop_ring in interact/gui_interact.h.  The style
    bridge this unit rides (style_el_col) stays in style/gui_style.h.
    What is left below is what only chrome and the widget set above consume.

==============================================================================================*/

// clang-format off

/* The self-measure the button family shares (stock/gui_adornment.c): visible label span plus the
   standard inset.  Its caption sibling gui_field_row is public, declared in gui_host.h. */
f32 label_natural_w( const char* s );

/* The one adornment consumed from ABOVE (stock/gui_adornment.c): chrome's window ends paint it
   over their own border to mark the window holding keyboard focus. */
void draw_window_focus_border( gui_rect_t r );

/* The styled half of the symbol palette (stock/gui_symbol_style.c): emitters that resolve
   their own look (style-var picks, WIN_BORDER, ROUND_WIDGET) over the draw unit's pure ones.
   Their public wrappers (gui_draw_arrow / gui_draw_close / gui_draw_frame) are in gui_host.h. */
void draw_arrow          ( gui_rect_t box, gui_dir_t dir, u32 color );
void draw_collapse_arrow ( gui_rect_t box, bool collapsed, u32 color );
void draw_close_x        ( gui_rect_t box, u32 color );
void draw_check_indicator( gui_rect_t box, u32 col );
void draw_rule           ( f32 x, f32 yc, f32 w, f32 thickness, u32 col );

u32 stock_unit_mem_bytes( void );          /* the stock unit's fixed statics */

// clang-format on
/*============================================================================================*/
#endif    // GUI_STOCK_INTERNAL_H
