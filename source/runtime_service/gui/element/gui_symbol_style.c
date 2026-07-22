/*==============================================================================================

    runtime_service/gui/element/gui_symbol_style.c -- The styled half of the symbol palette.

    The symbol emitters that resolve their OWN look -- a theme style-var pick
    (GUI_VAR_ARROW/CHECK/SEPARATOR_STYLE), a style metric (WIN_BORDER, checkmark_pad), or the
    ambient control rounding (ROUND_WIDGET) -- moved up from draw/gui_symbol.c at the R8 carve
    so the draw unit ends parameter-pure: a draw routine takes its colors and sizes as
    parameters; these read the live style, which makes them element material (the first layer
    astride style and draw).  Each composes the pure emitters it left behind through the
    public gui_draw_* surface (gui_host.h) or the render primitives directly.

    The public wrappers over the movers (gui_draw_arrow / gui_draw_close / gui_draw_frame)
    live at the foot -- they moved with their targets so the draw unit never calls upward.

==============================================================================================*/
// clang-format off

/* Directional arrow glyph: a filled triangle (default) or a stroked chevron pointing `dir`,
   centered in `box`.  The one arrow generator -- arrow_button, draw_collapse_arrow, the combo /
   submenu arrow, and the dock overlay all draw through it, so the shape is uniform and follows
   GUI_VAR_ARROW_STYLE (the chevron variant) exactly as check / bullet follow their style vars.
   The half-extent scales with the box so every arrow matches the others at any font size. */
void
draw_arrow( gui_rect_t box, gui_dir_t dir, u32 color )
{
    if ( style_var( GUI_VAR_ARROW_STYLE ) >= 0.5f )
    {
        f32 t = floorf( box.h * 0.13f );  if ( t < 1.5f ) t = 1.5f;
        gui_draw_chevron( box, dir, t, color );
        return;
    }

    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( box.h * 0.22f );   /* triangle half-extent */

    switch ( dir )
    {
        case GUI_DIR_LEFT:  draw_push_triangle( cx - s, cy,     cx + s, cy - s, cx + s, cy + s, 0, color ); break;
        case GUI_DIR_RIGHT: draw_push_triangle( cx + s, cy,     cx - s, cy - s, cx - s, cy + s, 0, color ); break;
        case GUI_DIR_UP:    draw_push_triangle( cx,     cy - s, cx - s, cy + s, cx + s, cy + s, 0, color ); break;
        case GUI_DIR_DOWN:  draw_push_triangle( cx,     cy + s, cx - s, cy - s, cx + s, cy - s, 0, color ); break;
    }
}

/* Collapse toggle glyph: points down when expanded, right when collapsed (the following label reads
   as the thing being toggled).  Exactly the DOWN / RIGHT case of draw_arrow, so the window title bar
   and collapsing_header fold with the identical glyph arrow_button draws.  Shared by both. */
void
draw_collapse_arrow( gui_rect_t box, bool collapsed, u32 color )
{
    draw_arrow( box, collapsed ? GUI_DIR_RIGHT : GUI_DIR_DOWN, color );
}

/* Close glyph: the two-diagonal 'X' centered in `box` (Dear ImGui's CloseButton cross).  Extracted
   so the native caption close button and any other caller stroke the identical mark.  Styled by
   the border metric: the stroke weight is WIN_BORDER so the cross tracks the frame weight. */
void
draw_close_x( gui_rect_t box, u32 color )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 m  = box.w < box.h ? box.w : box.h;
    f32 s  = floorf( m * 0.18f );   /* glyph half-extent -- matches the caption min/max glyphs */
    f32 t  = WIN_BORDER;

    gui_draw_line( cx - s, cy - s, cx + s, cy + s, t, color );
    gui_draw_line( cx - s, cy + s, cx + s, cy - s, t, color );
}

/* Checkbox / menu indicator: the mark drawn inside the `box` when checked, switched on
   GUI_VAR_CHECK_STYLE -- a 'v' tick (default), a filled disc, or an 'X' cross.  The one place the
   three-way style resolves, so checkbox and menu_item stay identical and a new style only adds here. */
void
draw_check_indicator( gui_rect_t box, u32 col )
{
    u32 style = (u32)( style_var( GUI_VAR_CHECK_STYLE ) + 0.5f );
    if ( style == GUI_CHECK_DISC )
        draw_push_circle_filled( box.x + box.w * 0.5f, box.y + box.h * 0.5f,
                                 box.w * 0.5f - (f32)s_style.checkmark_pad, 16, col );
    else if ( style == GUI_CHECK_CROSS )
        draw_close_x( box, col );
    else
        gui_draw_check_mark( box, col );
}

/* Horizontal rule centered on yc, honoring GUI_VAR_SEPARATOR_STYLE (solid fill or dashed line).
   The shared draw seam for separator() and the two rules of separator_text().  The dashed form
   goes straight to the backend stipple primitive (gui_draw_dashed_line, gui_emit_path.c). */
void
draw_rule( f32 x, f32 yc, f32 w, f32 thickness, u32 col )
{
    if ( w <= 0.0f )
        return;
    if ( style_var( GUI_VAR_SEPARATOR_STYLE ) >= 0.5f )
        gui_draw_dashed_line( x, yc, x + w, yc, 6.0f, 4.0f, thickness, col );
    else
        draw_push_rect_filled( x, yc - thickness * 0.5f, w, thickness, 0, 0, 1, 1, 0, col );
}

/* Frame / bezel (Dear ImGui RenderFrame): a filled rounded body with an optional border, the basis
   every widget frame shares.  Uses the control-frame rounding (ROUND_WIDGET) so a custom-drawn frame
   matches the built-in buttons / inputs; pass border <= 0 to skip the outline. */
static void
draw_frame( gui_rect_t r, u32 col_bg, u32 col_border, f32 border )
{
    f32 save = draw_rounding();
    draw_set_rounding( ROUND_WIDGET );
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0, 0, 1, 1, 0, col_bg );
    if ( border > 0.0f )
        draw_push_rect_outline( r.x, r.y, r.w, r.h, border, 0, col_border );
    draw_set_rounding( save );
}

/*==============================================================================================
    Public surface -- the wrappers whose targets live in this file (the rest of the gui_draw_*
    family stays at the foot of draw/gui_symbol.c with the pure emitters).
==============================================================================================*/

void gui_draw_arrow( gui_rect_t box, gui_dir_t dir, u32 col )                 { draw_arrow( box, dir, col ); }
void gui_draw_close( gui_rect_t box, u32 col )                                { draw_close_x( box, col ); }
void gui_draw_frame( gui_rect_t box, u32 col_bg, u32 col_border, f32 border ) { draw_frame( box, col_bg, col_border, border ); }

// clang-format on
/*============================================================================================*/
