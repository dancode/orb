/*==============================================================================================

    sandbox/gui/sb_gui_diablo/ui.c -- rect-first UI layer implementation.

    Thin by design: rect math delegates to the gui_rect_* inlines (gui.h), widgets compose
    gui()->item() behavior with gui()->draw_* presentation.  Nothing here touches the flow
    layout engine -- that is the point.

==============================================================================================*/

#include "sandbox/gui/sb_gui_diablo/ui.h"

// clang-format off

/*==============================================================================================
    Style -- ember-and-gold defaults (the sb_gui_diablo palette).
==============================================================================================*/

static ui_style_t s_style = {
    .panel_bg         = GUI_COLOR( 0x14, 0x10, 0x0d, 0xf0 ),
    .panel_border     = GUI_COLOR( 0x6b, 0x4a, 0x1f, 0xff ),
    .btn_bg           = GUI_COLOR( 0x1c, 0x14, 0x0e, 0xe6 ),
    .btn_bg_hover     = GUI_COLOR( 0x33, 0x20, 0x10, 0xf0 ),
    .btn_bg_press     = GUI_COLOR( 0x4a, 0x28, 0x10, 0xf5 ),
    .btn_border       = GUI_COLOR( 0x5a, 0x40, 0x1c, 0xff ),
    .btn_border_hover = GUI_COLOR( 0xc8, 0x96, 0x3c, 0xff ),
    .text             = GUI_COLOR( 0xd8, 0xc8, 0xa0, 0xff ),
    .text_dim         = GUI_COLOR( 0x80, 0x74, 0x5c, 0xff ),
    .title            = GUI_COLOR( 0xe8, 0xc0, 0x50, 0xff ),
    .title_shadow     = GUI_COLOR( 0x40, 0x10, 0x08, 0xff ),
    .border_w         = 2.0f,
};

ui_style_t*
ui_style( void )
{
    return &s_style;
}

/*==============================================================================================
    Screen scope
==============================================================================================*/

gui_rect_t
ui_screen_begin( gui_vp_t vp, const char* id )
{
    i32 w = 0, h = 0;
    gui()->viewport_size( vp, &w, &h );
    f32 y0 = gui()->viewport_content_y( vp );    /* below the caption band on a shelled window */

    gui()->region_begin( id, 0.0f, y0, (f32)w, (f32)h - y0, GUI_REGION_MID, GUI_WIN_NOSCROLL );
    gui()->stack();    /* declare a mode so stray flow widgets are legal; ours never flow */

    return ( gui_rect_t ){ 0.0f, y0, (f32)w, (f32)h - y0 };
}

void
ui_screen_end( void )
{
    gui()->region_end();
}

/*==============================================================================================
    Rect math
==============================================================================================*/

gui_rect_t ui_cut_top    ( gui_rect_t* r, f32 h ) { return gui_rect_cut_top   ( r, h ); }
gui_rect_t ui_cut_bottom ( gui_rect_t* r, f32 h ) { return gui_rect_cut_bottom( r, h ); }
gui_rect_t ui_cut_left   ( gui_rect_t* r, f32 w ) { return gui_rect_cut_left  ( r, w ); }
gui_rect_t ui_cut_right  ( gui_rect_t* r, f32 w ) { return gui_rect_cut_right ( r, w ); }

gui_rect_t ui_inset ( gui_rect_t r, f32 px ) { return gui_rect_pad( r, px ); }

gui_rect_t
ui_place( gui_rect_t area, f32 w, f32 h, gui_align_t align )
{
    return gui_rect_align( area, w, h, align );
}

gui_rect_t
ui_row( gui_rect_t area, i32 i, i32 n, f32 gap )
{
    f32 slot = ( area.h - gap * (f32)( n - 1 ) ) / (f32)n;
    return ( gui_rect_t ){ area.x, area.y + (f32)i * ( slot + gap ), area.w, slot };
}

gui_rect_t
ui_col( gui_rect_t area, i32 i, i32 n, f32 gap )
{
    f32 slot = ( area.w - gap * (f32)( n - 1 ) ) / (f32)n;
    return ( gui_rect_t ){ area.x + (f32)i * ( slot + gap ), area.y, slot, area.h };
}

gui_rect_t
ui_cell( gui_rect_t area, i32 cx, i32 cy, i32 ncols, i32 nrows, f32 gap )
{
    gui_rect_t row = ui_row( area, cy, nrows, gap );
    return ui_col( row, cx, ncols, gap );
}

f32
ui_span( i32 n, f32 size, f32 gap )
{
    return ( n <= 0 ) ? 0.0f : (f32)n * size + (f32)( n - 1 ) * gap;
}

/*==============================================================================================
    Basis unit
==============================================================================================*/

f32
ui_line( void )
{
    return gui()->sz_line_h();
}

f32
ui_u( f32 n )
{
    f32 v = n * gui()->sz_line_h();
    return (f32)(i32)( v + 0.5f );    /* whole px keeps 1-2px frame lines crisp */
}

/*==============================================================================================
    Widgets
==============================================================================================*/

void
ui_panel( gui_rect_t r )
{
    gui()->draw_frame( r, s_style.panel_bg, s_style.panel_border, s_style.border_w );
}

void
ui_label( gui_rect_t r, gui_align_t align, const char* text )
{
    gui()->draw_text_in( r, align, s_style.text, text );
}

void
ui_label_c( gui_rect_t r, gui_align_t align, u32 abgr, const char* text )
{
    gui()->draw_text_in( r, align, abgr, text );
}

void
ui_title( gui_rect_t r, const char* text )
{
    gui_vec2_t ts  = gui()->text_size( text );
    gui_rect_t box = gui_rect_align( r, ts.x, ts.y, GUI_ALIGN_CENTER );
    gui()->draw_text_shadow( box.x, box.y, text, s_style.title, s_style.title_shadow, 2.0f, 2.0f );
}

bool
ui_button( gui_rect_t r, const char* label )
{
    gui_item_state_t st = gui()->item( label, r );

    u32 bg     = st.active ? s_style.btn_bg_press
               : st.hover  ? s_style.btn_bg_hover
               :             s_style.btn_bg;
    u32 border = ( st.hover || st.nav ) ? s_style.btn_border_hover : s_style.btn_border;

    gui()->draw_frame( r, bg, border, s_style.border_w );
    gui()->draw_text_in( r, GUI_ALIGN_CENTER, s_style.text, label );

    if ( st.clicked ) 
         gui()->wants_redraw();

    return st.clicked;
}

/*============================================================================================*/
// clang-format on
