/*==============================================================================================

    sandbox/gui/sb_gui_diablo/sb_gui_diablo.c -- rect-first UI proof: a Diablo-style game shell.

    The test bed for the ui.h prototype layer: every screen is exact-fit layout -- panels,
    slot grids, and bars placed with rect arithmetic, never flow.  Built in increments:

      increment 1 (done) -- main menu: centered option column, title band, footer.  Screen
                            switching shell + stub screens the later increments fill in.
      increment 2 (done) -- in-game HUD: action bar (art-exact 64px slots), potion, health/
                            mana globes, XP strip, click-to-move world marker.
      increment 3 (done) -- skill tree (3x4 lattice + connectors), equipment (paper doll +
                            8x6 backpack of art-exact cells, cursor-carry), character stats
                            (cut-per-row list with steppers).
      increment 4 (done) -- options form: ui_slider / ui_check / ui_cycle rows; the FONT row
                            queues a live font swap the loop applies between frames.

    The point is the READ of the screen functions below: if the layout code is not obvious
    arithmetic, the ui layer failed.  ESC returns to the menu from any screen.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

#include "sandbox/gui/sb_gui_diablo/ui.h"

// clang-format off

/*==============================================================================================
    Screens
==============================================================================================*/

typedef enum screen_t
{
    SCR_MENU = 0,     // main menu (increment 1)
    SCR_GAME,         // gameplay view + HUD (increment 2)
    SCR_SKILLS,       // skill tree (increment 3)
    SCR_EQUIP,        // equipment slot grid (increment 3)
    SCR_STATS,        // character stats list (increment 3)
    SCR_OPTIONS,      // settings forms

} screen_t;

static screen_t s_screen = SCR_MENU;
static bool     s_quit   = false;

/*==============================================================================================
    Main menu -- title band up top, an exact 300px option column dead center, footer line.
==============================================================================================*/

static void
screen_menu( gui_vp_t vp )
{
    gui_rect_t screen = ui_screen_begin( vp, "menu" );

    /* footer first: a text band pinned to the bottom edge, half-unit inset */
    gui_rect_t footer = ui_inset( ui_cut_bottom( &screen, ui_u( 1.5f ) ), ui_u( 0.5f ) );
    ui_label_c( footer, GUI_ALIGN_LEFT  | GUI_ALIGN_VCENTER, ui_style()->text_dim, "EMBERFALL 0.1 -- sb_gui_diablo increment 1" );
    ui_label_c( footer, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->text_dim, "ORB engine" );
    
    f32 line_size = ui_line();
    UNUSED( line_size );

    /* title band: top third of what remains (proportional -- stays px math on purpose) */
    gui_rect_t band = ui_cut_top( &screen, screen.h * 0.34f );
    ui_title( band, "E M B E R F A L L" );

    /* subtitle: a dim line of text centered in the band, just above the option column */
    ui_label_c( ui_place( band, band.w, ui_line(), GUI_ALIGN_HCENTER | GUI_ALIGN_BOTTOM ),
                GUI_ALIGN_CENTER, ui_style()->text_dim, "a point-and-click descent" );
    
    /* option column: 14u wide x (6 rows of 2u, half-unit gaps), centered below the title */
    static const struct { const char* label; screen_t go; } opts[] = {
        { "ENTER SANCTUM", SCR_GAME    },
        { "SKILL TREE",    SCR_SKILLS  },
        { "EQUIPMENT",     SCR_EQUIP   },
        { "CHARACTER",     SCR_STATS   },
        { "OPTIONS",       SCR_OPTIONS },
        { "EXIT",          SCR_MENU    },
    };
    const i32 n    = (i32)( sizeof( opts ) / sizeof( opts[ 0 ] ) );
    const f32 gap  = ui_u( 0.5f );
    const f32 bt_h = ui_u( 2.0f );
    
    gui_rect_t col = ui_place( screen, ui_u( 14.0f ), ui_span( n, bt_h, gap ), GUI_ALIGN_CENTER );
    for ( i32 i = 0; i < n; ++i )
    {
        if ( ui_button( ui_row( col, i, n, gap ), opts[ i ].label ) )
        {
            if ( i == n - 1 ) s_quit = true;    /* EXIT is the last row */
            else              s_screen = opts[ i ].go;
        }
    }

    ui_screen_end();
}

/*==============================================================================================
    In-game HUD (increment 2) -- the exact-fit case: a fixed bottom band carved off the screen,
    art-exact 64px action slots dead center, resource globes at the band's outer thirds, an XP
    strip across the top of the band.  The "world" above is a click-to-move marker so the HUD
    proves itself against live input (slots and world share the screen without fighting).
==============================================================================================*/

static f32        s_health       = 0.72f;
static f32        s_mana         = 0.55f;
static i32        s_potions      = 3;
static i32        s_active_skill = 0;
static gui_vec2_t s_move_target  = { 0.0f, 0.0f };
static bool       s_has_target   = false;

static f32 clamp01( f32 v ) { return ( v < 0.0f ) ? 0.0f : ( v > 1.0f ) ? 1.0f : v; }

static void
screen_game( gui_vp_t vp )
{
    gui_rect_t screen = ui_screen_begin( vp, "game" );

    /* HUD band first -- carve the bottom 6u so the world knows its space */
    gui_rect_t hud   = ui_cut_bottom( &screen, ui_u( 6.0f ) );
    gui_rect_t world = screen;    /* everything left is the play field */

    /* ---- world: a dark floor wash + click-to-move marker ---- */
    gui()->draw_gradient( world, GUI_COLOR( 0x08, 0x06, 0x05, 0xff ),
                                 GUI_COLOR( 0x1a, 0x12, 0x0c, 0xff ), false );
    ui_label_c( ui_place( world, world.w, ui_line(), GUI_ALIGN_HCENTER | GUI_ALIGN_TOP ),
                GUI_ALIGN_CENTER, ui_style()->text_dim,
                "left-click: move    1-4: skills    Q: potion    ESC: menu" );

    if ( gui()->invisible_button( "##world", world ))
    {
        gui()->get_mouse_pos( &s_move_target.x, &s_move_target.y );
        s_has_target = true;
        gui()->request_redraw();
    }
    if ( s_has_target )
    {
        /* damper glide to the last click; anim keeps frames coming until it settles */
        gui_vec2_t pos = gui()->anim_vec2( 0xD1AB70u, s_move_target, 6.0f );
        gui()->draw_circle( pos.x, pos.y, 10.0f, true,  0.0f, GUI_COLOR( 0xc8, 0x50, 0x20, 0xff ) );
        gui()->draw_circle( pos.x, pos.y, 10.0f, false, 2.0f, GUI_COLOR( 0xe8, 0xc0, 0x50, 0xff ) );
    }

    /* ---- XP strip: a thin meter across the top of the band, trimmed 10u off each end ---- */
    gui_rect_t xp = ui_cut_top( &hud, ui_u( 0.4f ) );
    ui_cut_left ( &xp, ui_u( 10.0f ) );
    ui_cut_right( &xp, ui_u( 10.0f ) );
    ui_meter( xp, 0.35f, GUI_COLOR( 0x8a, 0x6a, 0x20, 0xff ) );

    /* ---- globes: centered in the outer 6u of the band ---- */
    char cap[ 16 ];
    f32  gd = ui_u( 4.5f );

    gui_rect_t lzone = ui_cut_left ( &hud, ui_u( 6.0f ) );
    snprintf( cap, sizeof( cap ), "%d", (i32)( s_health * 100.0f ) );
    ui_globe( ui_place( lzone, gd, gd, GUI_ALIGN_CENTER ), s_health,
              GUI_COLOR( 0x9a, 0x18, 0x10, 0xff ), cap );

    gui_rect_t rzone = ui_cut_right( &hud, ui_u( 6.0f ) );
    snprintf( cap, sizeof( cap ), "%d", (i32)( s_mana * 100.0f ) );
    ui_globe( ui_place( rzone, gd, gd, GUI_ALIGN_CENTER ), s_mana,
              GUI_COLOR( 0x18, 0x30, 0x9a, 0xff ), cap );

    /* ---- action bar: 6 art-exact 64px slots, 8px gaps, dead center of what remains ---- */
    static const struct { const char* glyph; const char* key; } slots[] = {
        { "I", "1" }, { "II", "2" }, { "III", "3" }, { "IV", "4" },   /* skills  */
        { "",  "Q" },                                                 /* potion  */
        { "TP", "E" },                                                /* to town */
    };
    const i32 ns = (i32)( sizeof( slots ) / sizeof( slots[ 0 ] ) );

    char pots[ 8 ];
    snprintf( pots, sizeof( pots ), "x%d", s_potions );

    gui_rect_t bar = ui_place( hud, ui_span( ns, 64.0f, 8.0f ), 64.0f, GUI_ALIGN_CENTER );
    for ( i32 i = 0; i < ns; ++i )
    {
        const char* glyph  = ( i == 4 ) ? pots : slots[ i ].glyph;
        bool        active = ( i < 4 ) && ( s_active_skill == i );
        bool        fire   = ui_slot( ui_col( bar, i, ns, 8.0f ), glyph, slots[ i ].key, active );

        if ( i < 4  && gui()->is_key_pressed( (app_key_t)( APP_KEY_1 + i ) ) ) fire = true;
        if ( i == 4 && gui()->is_key_pressed( APP_KEY_Q ) )                    fire = true;
        if ( i == 5 && gui()->is_key_pressed( APP_KEY_E ) )                    fire = true;
        if ( !fire )
            continue;

        gui()->request_redraw();    /* key-driven state change: next build shows it */
        if      ( i < 4 )  { s_active_skill = i;  s_mana = clamp01( s_mana - 0.06f ); }
        else if ( i == 4 ) { if ( s_potions > 0 ) { s_potions--; s_health = clamp01( s_health + 0.15f ); } }
        else               s_screen = SCR_MENU;
    }

    if ( gui()->is_key_pressed( APP_KEY_ESCAPE ) )
    {
        s_screen = SCR_MENU;
        gui()->request_redraw();
    }

    ui_screen_end();
}

/*==============================================================================================
    Shared screen chrome -- BACK button one unit below a panel + ESC, every sub-screen's exit.
==============================================================================================*/

static void
nav_back( gui_rect_t panel )
{
    gui_rect_t back = ui_place( panel, ui_u( 7.0f ), ui_u( 2.0f ), GUI_ALIGN_HCENTER | GUI_ALIGN_BOTTOM );
    back.y = panel.y + panel.h + ui_u( 1.0f );
    if ( ui_button( back, "BACK" ) || gui()->is_key_pressed( APP_KEY_ESCAPE ) )
    {
        s_screen = SCR_MENU;
        gui()->request_redraw();
    }
}

/*==============================================================================================
    Skill tree (increment 3) -- three branches x four tiers on a fixed lattice.  Connector
    lines run cell-center to cell-center (drawn first, nodes over them); a node is LOCKED
    until its prerequisite (the tier above, same branch) is taken -- locked nodes are plain
    panels, not widgets.  Click a lit node to spend a point; click a taken node to refund it
    (only if nothing below depends on it).
==============================================================================================*/

static i32  s_skill_points = 5;
static bool s_skill_taken[ 12 ];   /* [branch * 4 + tier] */

static void
screen_skills( gui_vp_t vp )
{
    static const char* branch[ 3 ] = { "EMBER", "GALE", "BONE" };
    static const char* tier  [ 4 ] = { "I", "II", "III", "IV" };

    gui_rect_t screen = ui_screen_begin( vp, "skills" );
    ui_title( ui_cut_top( &screen, ui_u( 4.0f ) ), "SKILL TREE" );

    /* panel sized from its contents: header + 3x4 lattice + footer line */
    const f32 pad    = ui_u( 0.75f );
    const f32 head_h = ui_u( 1.2f );
    const f32 lat_w  = ui_u( 24.0f );
    const f32 lat_h  = ui_u( 16.0f );

    gui_rect_t panel = ui_place( screen, lat_w + 2.0f * pad,
                                 head_h + lat_h + head_h + 2.0f * pad, GUI_ALIGN_CENTER );
    ui_panel( panel );
    nav_back( panel );

    gui_rect_t in   = ui_inset( panel, pad );
    gui_rect_t head = ui_cut_top( &in, head_h );
    gui_rect_t foot = ui_cut_bottom( &in, head_h );
    gui_rect_t lat  = in;

    for ( i32 b = 0; b < 3; ++b )
        ui_label_c( ui_col( head, b, 3, 0.0f ), GUI_ALIGN_CENTER, ui_style()->title, branch[ b ] );

    char buf[ 64 ];
    snprintf( buf, sizeof( buf ), "POINTS: %d    (click a taken node to refund)", s_skill_points );
    ui_label_c( foot, GUI_ALIGN_CENTER, ui_style()->text_dim, buf );

    /* connectors first, nodes on top; a link lights up once both ends are taken */
    for ( i32 b = 0; b < 3; ++b )
        for ( i32 t = 1; t < 4; ++t )
        {
            gui_vec2_t a = gui_rect_center( ui_cell( lat, b, t - 1, 3, 4, 0.0f ) );
            gui_vec2_t c = gui_rect_center( ui_cell( lat, b, t,     3, 4, 0.0f ) );
            bool lit = s_skill_taken[ b * 4 + t - 1 ] && s_skill_taken[ b * 4 + t ];
            gui()->draw_line( a.x, a.y, c.x, c.y, 2.0f,
                              lit ? ui_style()->btn_border_hover : ui_style()->btn_border );
        }

    for ( i32 b = 0; b < 3; ++b )
        for ( i32 t = 0; t < 4; ++t )
        {
            i32  i        = b * 4 + t;
            bool taken    = s_skill_taken[ i ];
            bool unlocked = ( t == 0 ) || s_skill_taken[ i - 1 ];
            gui_rect_t node = ui_place( ui_cell( lat, b, t, 3, 4, 0.0f ), 56.0f, 56.0f, GUI_ALIGN_CENTER );

            if ( !unlocked )    /* locked: inert chrome, not a widget */
            {
                ui_panel( node );
                ui_label_c( node, GUI_ALIGN_CENTER, ui_style()->text_dim, tier[ t ] );
                continue;
            }

            gui()->push_id_int( i );
            if ( ui_slot( node, tier[ t ], "", taken ) )
            {
                bool dependent = ( t < 3 ) && s_skill_taken[ i + 1 ];
                if      ( taken && !dependent )          { s_skill_taken[ i ] = false; s_skill_points++; }
                else if ( !taken && s_skill_points > 0 ) { s_skill_taken[ i ] = true;  s_skill_points--; }
            }
            gui()->pop_id();
        }

    ui_screen_end();
}

/*==============================================================================================
    Equipment (increment 3) -- paper doll (3x4 lattice of art-exact 64px slots, cross layout)
    beside an 8x6 backpack grid of 48px cells.  The panel's dimensions are COMPUTED from those
    art sizes with ui_span -- the container fits the slots, never the other way around.  Click
    any slot to pick its item up onto the cursor; click again to place / swap.
==============================================================================================*/

typedef struct doll_slot_t { i32 cx, cy; const char* tag; } doll_slot_t;

static const doll_slot_t s_doll_lay[ 9 ] = {
    { 1, 0, "HD" }, { 2, 0, "AM" },                    /* head, amulet          */
    { 0, 1, "WP" }, { 1, 1, "CH" }, { 2, 1, "SH" },    /* weapon, chest, shield */
    { 0, 2, "R1" }, { 1, 2, "LG" }, { 2, 2, "R2" },    /* rings, legs           */
    { 1, 3, "FT" },                                    /* feet                  */
};

static char s_doll[ 9 ]  = { 0, 0, 'S', 'A', 0, 0, 0, 0, 0 };    /* sword + armor equipped */
static char s_bag[ 48 ]  = { 'G', 0, '!', 0, 0, 'R', 0, 0, '!', 0, 0, 0, 'B' };
static char s_carry      = 0;

static void
screen_equip( gui_vp_t vp )
{
    gui_rect_t screen = ui_screen_begin( vp, "equip" );
    ui_title( ui_cut_top( &screen, ui_u( 4.0f ) ), "EQUIPMENT" );

    /* the two art blocks size the panel */
    const f32 pad    = ui_u( 0.75f );
    const f32 head_h = ui_line();
    const f32 doll_w = ui_span( 3, 64.0f, 8.0f ), doll_h = ui_span( 4, 64.0f, 8.0f );
    const f32 grid_w = ui_span( 8, 48.0f, 6.0f ), grid_h = ui_span( 6, 48.0f, 6.0f );
    const f32 body_h = ( doll_h > grid_h ) ? doll_h : grid_h;

    gui_rect_t panel = ui_place( screen, doll_w + pad + grid_w + 2.0f * pad,
                                 head_h + ui_u( 0.25f ) + body_h + 2.0f * pad, GUI_ALIGN_CENTER );
    ui_panel( panel );
    nav_back( panel );

    gui_rect_t in    = ui_inset( panel, pad );
    gui_rect_t left  = ui_cut_left( &in, doll_w );
    ui_cut_left( &in, pad );                          /* the gutter between the blocks */
    gui_rect_t right = in;

    ui_label_c( ui_cut_top( &left,  head_h ), GUI_ALIGN_LEFT, ui_style()->text_dim, "EQUIPPED" );
    ui_label_c( ui_cut_top( &right, head_h ), GUI_ALIGN_LEFT, ui_style()->text_dim, "BACKPACK" );
    ui_cut_top( &left,  ui_u( 0.25f ) );
    ui_cut_top( &right, ui_u( 0.25f ) );

    /* paper doll: 9 slots on a 3x4 lattice of exact 64px cells */
    gui_rect_t doll = ui_place( left, doll_w, doll_h, GUI_ALIGN_HCENTER | GUI_ALIGN_TOP );
    for ( i32 i = 0; i < 9; ++i )
    {
        gui_rect_t cell = ui_cell( doll, s_doll_lay[ i ].cx, s_doll_lay[ i ].cy, 3, 4, 8.0f );
        char glyph[ 2 ] = { s_doll[ i ], 0 };

        gui()->push_id_int( 100 + i );
        if ( ui_slot( cell, glyph, s_doll_lay[ i ].tag, s_doll[ i ] != 0 ) )
        {
            char t = s_doll[ i ]; s_doll[ i ] = s_carry; s_carry = t;
        }
        gui()->pop_id();
    }

    /* backpack: 8x6 grid of exact 48px cells */
    gui_rect_t bag = ui_place( right, grid_w, grid_h, GUI_ALIGN_LEFT | GUI_ALIGN_TOP );
    for ( i32 i = 0; i < 48; ++i )
    {
        gui_rect_t cell = ui_cell( bag, i % 8, i / 8, 8, 6, 6.0f );
        char glyph[ 2 ] = { s_bag[ i ], 0 };

        gui()->push_id_int( i );
        if ( ui_slot( cell, glyph, "", false ) )
        {
            char t = s_bag[ i ]; s_bag[ i ] = s_carry; s_carry = t;
        }
        gui()->pop_id();
    }

    /* the carried item rides the cursor (mouse motion keeps frames dirty on its own) */
    if ( s_carry )
    {
        f32 mx, my;
        char glyph[ 2 ] = { s_carry, 0 };
        gui()->get_mouse_pos( &mx, &my );
        gui()->draw_text_shadow( mx + 14.0f, my + 10.0f, glyph,
                                 ui_style()->title, ui_style()->title_shadow, 1.0f, 1.0f );
    }

    ui_screen_end();
}

/*==============================================================================================
    Character stats (increment 3) -- a cut-per-row list: four allocatable attributes with
    [-][+] steppers against a points pool, then the derived block computed from them.
==============================================================================================*/

static i32       s_stat_points   = 5;
static i32       s_stat[ 4 ]      = { 24, 18, 30, 12 };
static const i32 s_stat_base[ 4 ] = { 24, 18, 30, 12 };

static void
screen_stats( gui_vp_t vp )
{
    static const char* stat_name[ 4 ] = { "STRENGTH", "DEXTERITY", "VITALITY", "ENERGY" };

    gui_rect_t screen = ui_screen_begin( vp, "stats" );
    ui_title( ui_cut_top( &screen, ui_u( 4.0f ) ), "CHARACTER" );

    const f32 pad   = ui_u( 0.75f );
    const f32 row_h = ui_u( 1.6f );

    gui_rect_t panel = ui_place( screen, ui_u( 20.0f ),
                                 ui_u( 1.5f ) + 8.0f * row_h + ui_u( 0.75f ) + 2.0f * pad,
                                 GUI_ALIGN_CENTER );
    ui_panel( panel );
    nav_back( panel );

    gui_rect_t in   = ui_inset( panel, pad );
    gui_rect_t head = ui_cut_top( &in, ui_u( 1.5f ) );
    char buf[ 32 ];
    snprintf( buf, sizeof( buf ), "POINTS: %d", s_stat_points );
    ui_label_c( head, GUI_ALIGN_LEFT  | GUI_ALIGN_VCENTER, ui_style()->text,     "ATTRIBUTES" );
    ui_label_c( head, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->title,    buf );

    for ( i32 i = 0; i < 4; ++i )
    {
        gui_rect_t row  = ui_cut_top( &in, row_h );
        gui_rect_t plus = ui_place( ui_cut_right( &row, ui_u( 1.5f ) ), ui_u( 1.2f ), ui_u( 1.2f ), GUI_ALIGN_CENTER );
        gui_rect_t minus= ui_place( ui_cut_right( &row, ui_u( 1.5f ) ), ui_u( 1.2f ), ui_u( 1.2f ), GUI_ALIGN_CENTER );
        ui_cut_right( &row, ui_u( 0.5f ) );

        gui()->push_id_int( i );
        if ( ui_button( plus,  "+" ) && s_stat_points > 0 )              { s_stat[ i ]++; s_stat_points--; }
        if ( ui_button( minus, "-" ) && s_stat[ i ] > s_stat_base[ i ] ) { s_stat[ i ]--; s_stat_points++; }
        gui()->pop_id();

        snprintf( buf, sizeof( buf ), "%d", s_stat[ i ] );
        ui_label_c( row, GUI_ALIGN_LEFT  | GUI_ALIGN_VCENTER, ui_style()->text, stat_name[ i ] );
        ui_label_c( row, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->text, buf );
    }

    gui_rect_t rule = ui_cut_top( &in, ui_u( 0.75f ) );
    gui()->draw_line( rule.x, rule.y + rule.h * 0.5f, rule.x + rule.w, rule.y + rule.h * 0.5f,
                      1.0f, ui_style()->panel_border );

    static const char* dv_name[ 4 ] = { "ATTACK", "DEFENSE", "LIFE", "MANA" };
    const i32 dv[ 4 ] = { s_stat[ 0 ] * 2, s_stat[ 1 ] * 3,
                          60 + s_stat[ 2 ] * 4, 20 + s_stat[ 3 ] * 3 };
    for ( i32 i = 0; i < 4; ++i )
    {
        gui_rect_t row = ui_cut_top( &in, row_h );
        snprintf( buf, sizeof( buf ), "%d", dv[ i ] );
        ui_label_c( row, GUI_ALIGN_LEFT  | GUI_ALIGN_VCENTER, ui_style()->text_dim, dv_name[ i ] );
        ui_label_c( row, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->text_dim, buf );
    }

    ui_screen_end();
}

/*==============================================================================================
    Options (increment 4) -- the settings form: label left, control zone right, one cut per
    row.  Sliders, checks, and "< value >" cycles are the new ui form controls.  The FONT row
    is live: it queues a font swap the main loop applies between frames (fonts are frame-
    global state), and the whole form re-lays itself in the new basis -- the two-currency
    policy demonstrated by the very screen that changes the dial.
==============================================================================================*/

static f32  s_opt_master = 0.8f;
static f32  s_opt_music  = 0.6f;
static bool s_opt_vsync  = true;
static bool s_opt_dmgnum = true;
static i32  s_opt_res    = 1;
static i32  s_opt_diff   = 0;
static i32  s_opt_font   = 0;
static i32  s_font_req   = -1;    /* queued font choice; applied in main() between frames */

static void
screen_options( gui_vp_t vp )
{
    static const char* res_items [] = { "1280 x 720", "1600 x 900", "1920 x 1080" };
    static const char* diff_items[] = { "NORMAL", "NIGHTMARE", "HELL" };
    static const char* font_items[] = { "ROBOTO 16", "CASCADIA 20" };

    gui_rect_t screen = ui_screen_begin( vp, "options" );
    ui_title( ui_cut_top( &screen, ui_u( 4.0f ) ), "OPTIONS" );

    const f32 pad   = ui_u( 0.75f );
    const f32 row_h = ui_u( 1.8f );
    const f32 gap   = ui_u( 0.25f );

    gui_rect_t panel = ui_place( screen, ui_u( 26.0f ),
                                 ui_span( 7, row_h, gap ) + 2.0f * pad, GUI_ALIGN_CENTER );
    ui_panel( panel );
    nav_back( panel );

    gui_rect_t in  = ui_inset( panel, pad );
    char       buf[ 16 ];

    for ( i32 row = 0; row < 7; ++row )
    {
        gui_rect_t r    = ui_cut_top( &in, row_h );
        gui_rect_t ctrl = ui_cut_right( &r, ui_u( 12.0f ) );
        ui_cut_top( &in, gap );

        gui()->push_id_int( row );
        switch ( row )
        {
            case 0:
            {
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "MASTER VOLUME" );
                gui_rect_t val = ui_cut_right( &ctrl, ui_u( 2.5f ) );
                ui_slider( ctrl, &s_opt_master, 0.0f, 1.0f );
                snprintf( buf, sizeof( buf ), "%d%%", (i32)( s_opt_master * 100.0f + 0.5f ) );
                ui_label_c( val, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->text_dim, buf );
            } break;

            case 1:
            {
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "MUSIC VOLUME" );
                gui_rect_t val = ui_cut_right( &ctrl, ui_u( 2.5f ) );
                ui_slider( ctrl, &s_opt_music, 0.0f, 1.0f );
                snprintf( buf, sizeof( buf ), "%d%%", (i32)( s_opt_music * 100.0f + 0.5f ) );
                ui_label_c( val, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->text_dim, buf );
            } break;

            case 2:
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "RESOLUTION" );
                ui_cycle( ctrl, &s_opt_res, res_items, 3 );
                break;

            case 3:
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "DIFFICULTY" );
                ui_cycle( ctrl, &s_opt_diff, diff_items, 3 );
                break;

            case 4:
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "FONT" );
                if ( ui_cycle( ctrl, &s_opt_font, font_items, 2 ) )
                    s_font_req = s_opt_font;          /* applied between frames by the loop */
                break;

            case 5:
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "VSYNC" );
                ui_check( ui_place( ctrl, row_h, row_h, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER ), &s_opt_vsync );
                break;

            case 6:
                ui_label_c( r, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ui_style()->text, "DAMAGE NUMBERS" );
                ui_check( ui_place( ctrl, row_h, row_h, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER ), &s_opt_dmgnum );
                break;
        }
        gui()->pop_id();
    }

    ui_screen_end();
}

/*==============================================================================================
    Frame build
==============================================================================================*/

static void
build_frame( gui_vp_t vp )
{
    switch ( s_screen )
    {
        case SCR_MENU:    screen_menu( vp ); break;
        case SCR_GAME:    screen_game( vp ); break;
        case SCR_SKILLS:  screen_skills( vp ); break;
        case SCR_EQUIP:   screen_equip ( vp ); break;
        case SCR_STATS:   screen_stats ( vp ); break;
        case SCR_OPTIONS: screen_options( vp ); break;
    }
}

/*==============================================================================================
    Entry
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_diablo] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- sb_gui_diablo",
        .w     = 1600, .h = 900,
        .font  = GUI_FONT_ROBOTO_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.035f, 0.025f, 0.02f, 1.0f },    /* near-black ember wash */
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_diablo] gui->boot failed\n" );
        goto shutdown;
    }

    // gui()->set_force_redraw( true );

    /* Second font for the F1/F2 basis-unit test.  boot() put Roboto 16 in registry slot 0;
       font_load_builtin loads a preset into a NEW registry id and activates it -- switch back
       to slot 0 after.  0 on failure: F2 then just reselects slot 0. */
    u32 font_big = gui()->font_load_builtin( GUI_FONT_CASCADIA_MONO_20 );
    gui()->font_use( 0 );
    ui_kit_install();   /* the kit owns the element look -- install after every font landing */

    f32 dt = 0.0f;
    while ( !s_quit && gui()->boot_poll( &dt ) )
    {
        /* font selection is frame-global state: switch BETWEEN frames (pre frame_begin), and
           read the key from app()'s snapshot -- gui's io snapshot belongs to the frame scope.
           F1/F2 and the OPTIONS form drive the same choice: keys set s_font_req too, so the
           form's FONT row always shows the truth. */
        if ( app()->key_pressed( APP_KEY_F1 ) ) s_font_req = 0;
        if ( app()->key_pressed( APP_KEY_F2 ) ) s_font_req = 1;
        if ( s_font_req >= 0 )
        {
            gui()->font_use( s_font_req == 0 ? 0 : font_big );
            ui_kit_install();   /* font_use re-derived the style -- re-install the kit */
            s_opt_font = s_font_req;
            s_font_req = -1;
        }

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame( vp0 );
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        gui()->frame_pace( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    rhi()->shutdown();
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
// clang-format on
