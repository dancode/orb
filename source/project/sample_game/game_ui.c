/*==============================================================================================

    project/sample_game/game_ui.c -- the game kit implementation.

    Thin by design, like every kit: the renders are gui's stock_* widgets; this file owns only
    the palette (teal, matching the orbiting square), the HUD surface bracket, and the
    game-flavored composites.  See game_ui.h for the kit rules.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"

#include "project/sample_game/game_ui.h"

MOD_USE_GUI;

// clang-format off

/* The scene accent as a gui color: the square's teal (0.20, 0.80, 0.70).  One constant, not two
   -- the dim variant used to be hand-mixed here and is now the theme's own fade of this. */
#define GAME_UI_TEAL      GUI_COLOR( 0x33, 0xCC, 0xB3, 0xFF )

static bool s_has_gui = false;

/*==============================================================================================
    Wiring -- soft fetch: a gui-less host is a supported shape, not an error.
==============================================================================================*/

bool
game_ui_wire( get_api_fn get_api )
{
    UNUSED( get_api );                /* static builds: the fetch macro compiles to (1) */
    s_has_gui = MOD_FETCH_GUI;
    return s_has_gui;
}

bool
game_ui_ready( void )
{
    return s_has_gui;
}

/*==============================================================================================
    The S3 -> S1 compile step -- install the game's accent into the style.
==============================================================================================*/

void
game_ui_install( void )
{
    if ( !s_has_gui )
        return;

    /* Two seeds: the game's hue, and nothing else.  The rest of the theme -- surface, control,
       ink, line, the whole metric block -- is inherited as it stands, which is what "the
       smallest install that shows the dial" now costs.

       This was eight assignments, and six of them wrote ONE colour into the three reacting cells
       of two rows: the accent stopped lifting on hover and the mark stopped lighting on nav,
       because a flat row is a dead row.  Seeding and baking keeps both ramps alive and is
       shorter. */
    gui_style_t* e = gui()->style_edit();

    e->palette.seed[ GUI_SEED_ACCENT ] = GAME_UI_TEAL;
    e->palette.seed[ GUI_SEED_MARK   ] = GAME_UI_TEAL;

    gui()->style_bake( e );
}

/*==============================================================================================
    HUD surface bracket
==============================================================================================*/

gui_rect_t
game_ui_hud_begin( i32 gui_vp )
{
    i32 vp = ( i32 )gui_vp;

    i32 w = 0, h = 0;
    gui()->viewport_size( vp, &w, &h );
    f32 y0 = gui()->viewport_content_y( vp );    /* below the caption band on a shelled window */

    gui()->region_begin( "sg_hud", 0.0f, y0, ( f32 )w, ( f32 )h - y0,
                         GUI_REGION_MID, GUI_WIN_NOSCROLL );
    gui()->stack();    /* declare a mode so stray flow widgets are legal; the HUD never flows */

    return ( gui_rect_t ){ 0.0f, y0, ( f32 )w, ( f32 )h - y0 };
}

void
game_ui_hud_end( void )
{
    gui()->region_end();
}

f32
game_ui_u( f32 n )
{
    f32 v = n * gui()->sz_line_h();
    return ( f32 )( i32 )( v + 0.5f );    /* whole px keeps 1-2px frame lines crisp */
}

/*==============================================================================================
    Readouts -- every one fills exactly its rect.
==============================================================================================*/

void
game_ui_score( gui_rect_t r, i32 score )
{
    char text[ 32 ];
    snprintf( text, sizeof( text ), "score  %d", score );

    gui()->stock_panel( r );
    gui()->stock_label( gui_rect_pad( r, game_ui_u( 0.25f ) ),
                     GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, text );
}

void
game_ui_tick_meter( gui_rect_t r, f32 frac )
{
    gui()->stock_meter( r, frac, GAME_UI_TEAL );
}

// clang-format on
/*============================================================================================*/
