#ifndef GUI_STOCK_INTERNAL_H
#define GUI_STOCK_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/stock/gui_stock_internal.h -- the stock unit's cross-unit seams.

    The stock unit (root gui_stock.c) is the first layer astride both servers: styled paint
    over interact state.  Its PUBLIC surface (the stock_* renders) is the GUI_STOCK band of
    gui_api.h; the style grid they paint from is gui.h's, published as gui()->style_color;
    this header holds only what crosses a unit boundary inside the system -- the per-item
    wrappers the emit/chrome seams call, the styled painters the widgets and chrome compose,
    and the unit's memory seam.

    ONE DECL, ONE HOME -- so most of what this unit defines is NOT declared here.  Anything a
    lower unit invokes across its documented upward seam is declared with that LOWEST consumer:
    item_flags_resolve / item_flags_chrome_reset and the child-box trio in flow/gui_flow.h,
    draw_nav_ring in core/gui_core.h, draw_drop_ring in interact/gui_interact.h.  The style
    bridge this unit rides (style_col) stays in style/gui_style.h.
    What is left below is what only chrome and the widget set above consume.

==============================================================================================*/

// clang-format off

/* The self-measure the button family shares (stock/gui_adornment.c): visible label span plus the
   standard inset.  Its caption sibling gui_field_row is public, declared in gui_host.h. */
f32 label_natural_w( const char* s );

/* Paint the keyboard rings this window marked (stock/gui_adornment.c).  Called at the end of a
   window's body, before its chrome: a ring lies ON its item's rect, so painting it where it is
   decided would put it under the widget's own fill.  It is marked there instead (ring_mark_*,
   core/gui_core.h) and laid down here, each back under its own clip so a ring inside a scrolled
   child stays bounded by that child's view. */
void rings_paint( void );

/* The styled half of the symbol palette (stock/gui_symbol_style.c): emitters that resolve
   their own look (style-var picks, WIN_BORDER, ROUND_WIDGET) over the draw unit's pure ones.
   Their public wrappers (gui_draw_arrow / gui_draw_close / gui_draw_frame) are in gui_host.h. */
void draw_arrow          ( gui_rect_t box, gui_dir_t dir, u32 color );
void draw_collapse_arrow ( gui_rect_t box, bool collapsed, u32 color );
void draw_close_x        ( gui_rect_t box, u32 color );
void draw_check_indicator( gui_rect_t box, u32 col );
void draw_rule           ( f32 x, f32 yc, f32 w, f32 thickness, u32 col );

/* The FACE painters (stock/gui_face.c) -- fill a rect for a style CELL: the cell's brush when the
   theme authored one, its flat colour when it did not, and NO border over authored art.  Each
   mirrors one colour projection so a site converts by changing one call (see the file banner).
   Consumed by the stock renders here and by every chrome widget that paints a surface.

   Every ITEM painter takes an id and animates through style_mix; GUI_ID_NONE opts out with no
   damper slot touched.  A widget painting more than one row reads style_mix once itself and
   uses the _mix forms, so its parts move together off a single probe. */
void draw_face           ( gui_rect_t r, u8 role, u8 phase );
void draw_face_frame     ( gui_rect_t r, u8 role, u8 phase, u32 border_col, f32 border_w );

void draw_face_item      ( gui_rect_t r, gui_id_t id, gui_item_state_t st, bool selected );
void draw_face_item_frame( gui_rect_t r, gui_id_t id, gui_item_state_t st, bool selected,
                           u32 border_col, f32 border_w );
void draw_face_grab      ( gui_rect_t r, gui_id_t id, gui_item_state_t st,
                           u32 border_col, f32 border_w );
void draw_face_field     ( gui_rect_t r, gui_id_t id, gui_item_state_t st,
                           u8 idle_role, u8 idle_phase, u32 border_col, f32 border_w );

void draw_face_mix       ( gui_rect_t r, u8 role, gui_style_mix_t m );
void draw_face_mix_frame ( gui_rect_t r, u8 role, gui_style_mix_t m, u32 border_col, f32 border_w );
void draw_face_field_mix ( gui_rect_t r, gui_style_mix_t m, u8 idle_role, u8 idle_phase,
                           u32 border_col, f32 border_w );

u32 stock_unit_mem_bytes( void );          /* the stock unit's fixed statics */

// clang-format on
/*============================================================================================*/
#endif    // GUI_STOCK_INTERNAL_H
