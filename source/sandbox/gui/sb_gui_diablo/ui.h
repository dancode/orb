#ifndef SB_UI_H
#define SB_UI_H
/*==============================================================================================

    sandbox/gui/sb_gui_diablo/ui.h -- the rect-first UI layer (prototype).

    A proving ground for a simpler user-facing GUI API, built over the existing gui library.
    Once proven by sb_gui_diablo it graduates into gui proper.  The whole design is three rules:

      1. RECTS ARE THE ONLY LAYOUT CURRENCY.  You are handed the screen as a rect.  You cut
         it, split it, and place boxes in it with pure math.  There is no pen, no flow, no
         layout mode, no per-region template.  Every function either transforms rects or
         fills one with a widget.

      2. WHAT YOU GIVE IS WHAT YOU GET.  A widget fills EXACTLY the rect you pass -- no
         hidden padding, no border carved off behind your back, no "outer vs interior"
         distinction.  All spacing is spacing YOU added with ui_inset / gap arguments,
         so every pixel is accounted for at the call site.

      3. PIXELS ONLY.  Every size argument is pixels.  No overloaded f32 where >1 means px,
         (0,1) means fraction, 1 means fill.  A fraction is plain C: r.w * 0.25f.

    The result: exact-fit screens (HUD bars, slot grids, fixed panels) read as arithmetic
    you can check by eye, and there is nothing to decipher -- ui_span( 5, 48, 12 ) IS the
    height of five 48px rows with 12px gaps, nothing more.

        gui_rect_t screen = ui_screen_begin( vp, "menu" );
        gui_rect_t band   = ui_cut_top( &screen, 120 );                       // title band
        ui_title( band, "EMBERFALL" );
        gui_rect_t col = ui_place( screen, 300, ui_span( 5, 44, 10 ), GUI_ALIGN_CENTER );
        for ( i = 0; i < 5; ++i )
            if ( ui_button( ui_row( col, i, 5, 10 ), label[ i ] ) ) ...
        ui_screen_end();

==============================================================================================*/

#include "runtime_service/gui/gui_api.h"

// clang-format off

/*==============================================================================================
    Screen scope -- one full-viewport input surface per logical screen (menu, HUD, ...).
    Opens a chrome-free fullscreen region on viewport vp (below any caption band) and returns
    its rect: THE root rect all layout math starts from.  Always pair with ui_screen_end.
==============================================================================================*/

gui_rect_t ui_screen_begin( gui_vp_t vp, const char* id );
void       ui_screen_end  ( void );

/*==============================================================================================
    Rect math -- pure functions, no state.  Cuts consume from the source rect (it shrinks);
    everything else returns a new rect and leaves the input untouched.
==============================================================================================*/

gui_rect_t ui_cut_top    ( gui_rect_t* r, f32 h );   // slice h px off the top; r shrinks
gui_rect_t ui_cut_bottom ( gui_rect_t* r, f32 h );
gui_rect_t ui_cut_left   ( gui_rect_t* r, f32 w );
gui_rect_t ui_cut_right  ( gui_rect_t* r, f32 w );

gui_rect_t ui_inset ( gui_rect_t r, f32 px );        // shrink uniformly by px on every side

/* Seat an exact w x h box inside `area` per the align flags (GUI_ALIGN_CENTER, LEFT|BOTTOM...).
   The one placement verb: nothing is resized, nothing is padded -- the box is w x h, period. */
gui_rect_t ui_place ( gui_rect_t area, f32 w, f32 h, gui_align_t align );

/* Equal partitions of an area with an explicit gap between slots (gap only BETWEEN, never at
   the edges -- rule 2).  ui_row/ui_col return slot i of n stacked vertically / horizontally;
   ui_cell returns cell (cx, cy) of an ncols x nrows matrix. */
gui_rect_t ui_row  ( gui_rect_t area, i32 i, i32 n, f32 gap );
gui_rect_t ui_col  ( gui_rect_t area, i32 i, i32 n, f32 gap );
gui_rect_t ui_cell ( gui_rect_t area, i32 cx, i32 cy, i32 ncols, i32 nrows, f32 gap );

/* Exact extent of n fixed-size slots with gaps between them: n * size + (n - 1) * gap.
   The sizing primitive: compute a container's dimension FROM its contents, then ui_place it. */
f32 ui_span ( i32 n, f32 size, f32 gap );

/*==============================================================================================
    Widgets -- every widget fills exactly the rect it is given.  Colors come from the shared
    style block below; tweak it directly (it is a plain struct, live every frame).
==============================================================================================*/

void ui_panel  ( gui_rect_t r );                                   // framed backdrop panel
void ui_label  ( gui_rect_t r, gui_align_t align, const char* text );
void ui_label_c( gui_rect_t r, gui_align_t align, u32 abgr, const char* text );
void ui_title  ( gui_rect_t r, const char* text );                 // centered display text
bool ui_button ( gui_rect_t r, const char* label );                // true on click

/*==============================================================================================
    Style -- one flat struct, mutable at any time.  No stacks, no push/pop: a screen that wants
    different colors sets them before its widgets and (optionally) restores after.
==============================================================================================*/

typedef struct ui_style_t
{
    u32 panel_bg;           // panel fill
    u32 panel_border;       // panel frame line
    u32 btn_bg;             // button fill, idle
    u32 btn_bg_hover;       // button fill, cursor over
    u32 btn_bg_press;       // button fill, held
    u32 btn_border;         // button frame line, idle
    u32 btn_border_hover;   // button frame line, cursor over / keyboard nav
    u32 text;               // primary text
    u32 text_dim;           // secondary text
    u32 title;              // display text
    u32 title_shadow;       // display text drop shadow
    f32 border_w;           // frame line width (panels + buttons)

} ui_style_t;

ui_style_t* ui_style( void );

// clang-format on
/*============================================================================================*/
#endif    // SB_UI_H
