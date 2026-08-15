/*==============================================================================================

    sandbox/gui/sb_gui_diablo/ui.c -- rect-first UI layer implementation.

    Thin by design: rect math delegates to the gui_rect_* inlines (gui.h), widgets compose
    gui()->item() behavior with gui()->draw_* presentation.  Nothing here touches the flow
    layout engine -- that is the point.

==============================================================================================*/

#include "sandbox/gui/sb_gui_diablo/ui.h"

#include "base/fmt.h"    /* fmt_snprintf -- slot id composition */

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
    .slot_bg          = GUI_COLOR( 0x0e, 0x0b, 0x08, 0xf0 ),
    .slot_border      = GUI_COLOR( 0x52, 0x3a, 0x1a, 0xff ),
    .slot_border_hot  = GUI_COLOR( 0xc8, 0x96, 0x3c, 0xff ),
    .globe_bg         = GUI_COLOR( 0x0a, 0x08, 0x06, 0xf5 ),
    .globe_ring       = GUI_COLOR( 0x6b, 0x4a, 0x1f, 0xff ),
    .meter_bg         = GUI_COLOR( 0x12, 0x0e, 0x0a, 0xf0 ),
    .border_w         = 2.0f,
};

ui_style_t*
ui_style( void )
{
    return &s_style;
}

/* The S3 -> S1 compile step: install the ember-gold palette into gui's style, so the
   promoted stock_* renders (which ui_button / ui_check / ui_slider / ui_cycle / ui_meter now
   delegate to) render this kit's look.  The theme system re-derives the installed style at
   every theme / font landing (gui_style_apply), so call this after boot AND after any
   font_use / theme_set -- the kit owns the element look by having the last word.

   This used to be 32 assignments -- every cell of the grid, written one at a time, with
   panel_bg spelled into three of them and text into another three.  It is now seven seeds, a
   five-number ramp, one bake, and the two cells this kit genuinely disagrees with the ramp
   about.  The ramp is where the kit's character actually lives: a 0.15 hover and a 0.30 press
   is what makes ember-gold smoulder instead of flashing the way chrome's 0.60 / 0.75 does. */
void
ui_kit_install( void )
{
    gui_style_t* e = gui()->style_edit();

    e->var[ GUI_VAR_BORDER ] = s_style.border_w;

    /* Seven source colours.  Note the alphas ride through the bake untouched, so the whole kit
       stays translucent over the game behind it without restating 0xf0 thirty-two times. */
    e->palette.seed[ GUI_SEED_SURFACE ] = s_style.panel_bg;          /* the leather backing   */
    e->palette.seed[ GUI_SEED_CONTROL ] = s_style.btn_bg;            /* the raised plate      */
    e->palette.seed[ GUI_SEED_INK     ] = s_style.text;              /* parchment             */
    e->palette.seed[ GUI_SEED_LINE    ] = s_style.btn_border;        /* the tooled edge       */
    e->palette.seed[ GUI_SEED_ACCENT  ] = s_style.btn_border_hover;  /* lit gold              */
    e->palette.seed[ GUI_SEED_MARK    ] = s_style.title;             /* bright gold           */
    e->palette.seed[ GUI_SEED_GRAB    ] = s_style.text;              /* the contrast anchor   */

    /* A smouldering ramp, not a flashing one -- the whole difference between this kit's feel and
       chrome's, and it is seven numbers rather than a repainted grid. */
    e->palette.ramp[ GUI_RAMP_HOVER  ] = 0.15f;
    e->palette.ramp[ GUI_RAMP_PRESS  ] = 0.30f;
    e->palette.ramp[ GUI_RAMP_FADE   ] = 0.45f;
    e->palette.ramp[ GUI_RAMP_RECESS ] = 0.30f;
    e->palette.ramp[ GUI_RAMP_NEST   ] = 0.30f;   /* leather sinks: this kit's depth is carved */
    e->palette.ramp[ GUI_RAMP_STEP   ] = 0.18f;
    e->palette.ramp[ GUI_RAMP_SELECT ] = 0.45f;   /* a dark kit: a deep wash swallows the gold */

    gui()->style_bake( e );

    /* The two the ramp cannot know: this kit's panels are painted scenery, not surfaces you can
       interact with, so they must not react at all.  Bake first, disagree after -- which is the
       whole reason the bake is a call and not a side effect.  A selected PANEL read still washes
       this flat colour toward the accent live -- there is no separate plane to disagree with. */
    e->col[ GUI_ROLE_PANEL ][ GUI_PHASE_HOT    ] = s_style.panel_bg;
    e->col[ GUI_ROLE_PANEL ][ GUI_PHASE_ACTIVE ] = s_style.panel_bg;
}

/*==============================================================================================
    Screen scope
==============================================================================================*/

gui_rect_t
ui_screen_begin( i32 vp, const char* id )
{
    i32 w = 0, h = 0;
    gui()->viewport_size( vp, &w, &h );
    f32 y0 = gui()->viewport_content_y( vp );    /* below the caption band on a shelled window */

    gui()->region_begin( id, 0.0f, y0, (f32)w, (f32)h - y0, GUI_REGION_MID, GUI_VP_MAIN,
                         GUI_WIN_NOSCROLL );
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

/* UNIFIED (layout-seam pass): no kit vocabulary -- the stock widget driven by an explicit rect.
   next_item_rect makes cell_next_w hand gui_button THIS rect verbatim (no flow pad / gap), so the
   button fills r exactly, the stock render's zero-padding property with one widget set.  The kit
   look still arrives through ui_kit_install's installed style. */
bool
ui_button( gui_rect_t r, const char* label )
{
    gui()->next_item_rect( r );
    return gui()->button( label );
}

bool
ui_slot( gui_rect_t r, const char* label, const char* hotkey, bool active )
{
    char id[ 64 ];
    fmt_snprintf( id, sizeof( id ), "%s##slot_%s", label, hotkey );
    gui_item_state_t st = gui()->item( id, r );

    u32 border = ( active || st.hover || st.nav ) ? s_style.slot_border_hot : s_style.slot_border;
    u32 bg     = st.active ? s_style.btn_bg_press : s_style.slot_bg;

    gui()->draw_frame( r, bg, border, s_style.border_w );
    if ( label  && label[ 0 ] )
        gui()->draw_text_in( r, GUI_ALIGN_CENTER, s_style.text, label );
    if ( hotkey && hotkey[ 0 ] )
        gui()->draw_text_in( ui_inset( r, 3.0f ), GUI_ALIGN_RIGHT | GUI_ALIGN_BOTTOM,
                             s_style.text_dim, hotkey );

    if ( st.clicked )
        gui()->request_redraw();
    return st.clicked;
}

void
ui_globe( gui_rect_t r, f32 frac, u32 fill_abgr, const char* caption )
{
    frac = ( frac < 0.0f ) ? 0.0f : ( frac > 1.0f ) ? 1.0f : frac;

    f32 rad = ( ( r.w < r.h ) ? r.w : r.h ) * 0.5f;
    f32 cx  = r.x + r.w * 0.5f;
    f32 cy  = r.y + r.h * 0.5f;

    gui()->draw_circle( cx, cy, rad, true, 0.0f, s_style.globe_bg );

    /* bottom-up liquid fill: clip the circle to the bottom `frac` of its box */
    if ( frac > 0.0f )
    {
        f32 lid = ( cy + rad ) - frac * ( 2.0f * rad );
        gui()->push_clip( cx - rad, lid, 2.0f * rad, ( cy + rad ) - lid );
        gui()->draw_circle( cx, cy, rad, true, 0.0f, fill_abgr );
        gui()->pop_clip();
    }

    gui()->draw_circle( cx, cy, rad, false, 3.0f, s_style.globe_ring );
    if ( caption && caption[ 0 ] )
        gui()->draw_text_in( r, GUI_ALIGN_CENTER, s_style.text, caption );
}

/* PROMOTED: gui()->stock_meter (track colors from the installed style; fill stays a
   call parameter -- the per-widget color rule). */
void
ui_meter( gui_rect_t r, f32 frac, u32 fill_abgr )
{
    gui()->stock_meter( r, frac, fill_abgr );
}

/*==============================================================================================
    Form controls
==============================================================================================*/

/* PROMOTED: the three form cores are gui()->stock_check / stock_slider / stock_cycle.  The kit keeps
   the old fixed-string ids so existing push_id_int row brackets behave identically. */
bool
ui_check( gui_rect_t r, bool* v )
{
    return gui()->stock_check( r, "##check", v );
}

bool
ui_slider( gui_rect_t r, f32* v, f32 lo, f32 hi )
{
    return gui()->stock_slider( r, "##slider", v, lo, hi );
}

bool
ui_cycle( gui_rect_t r, i32* idx, const char* const* items, i32 count )
{
    return gui()->stock_cycle( r, "##cyc", idx, items, count );
}

/*============================================================================================*/
// clang-format on
