/*==============================================================================================

    sandbox/gui/sb_gui_diablo/sb_gui_diablo.c -- rect-first UI proof: a Diablo-style game shell.

    The test bed for the ui.h prototype layer: every screen is exact-fit layout -- panels,
    slot grids, and bars placed with rect arithmetic, never flow.  Built in increments:

      increment 1 (this) -- main menu: centered option column, title band, footer.  Screen
                            switching shell + stub screens the later increments fill in.
      increment 2        -- in-game HUD: action bar, potion slots, health/mana globes.
      increment 3+       -- skill tree, equipment (256px slot grid), character stats.

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

    /* footer first: version line pinned to the bottom edge, inset 12px */
    gui_rect_t footer = ui_inset( ui_cut_bottom( &screen, 32.0f ), 12.0f );
    ui_label_c( footer, GUI_ALIGN_LEFT  | GUI_ALIGN_VCENTER, ui_style()->text_dim, "EMBERFALL 0.1 -- sb_gui_diablo increment 1" );
    ui_label_c( footer, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ui_style()->text_dim, "ORB engine" );

    /* title band: top third of what remains */
    gui_rect_t band = ui_cut_top( &screen, screen.h * 0.34f );
    ui_title( band, "E M B E R F A L L" );
    ui_label_c( ui_place( band, band.w, 24.0f, GUI_ALIGN_HCENTER | GUI_ALIGN_BOTTOM ),
                GUI_ALIGN_CENTER, ui_style()->text_dim, "a point-and-click descent" );

    /* option column: 300 x (6 rows of 44, 10 gaps), centered in the space below the title */
    static const struct { const char* label; screen_t go; } opts[] = {
        { "ENTER SANCTUM", SCR_GAME    },
        { "SKILL TREE",    SCR_SKILLS  },
        { "EQUIPMENT",     SCR_EQUIP   },
        { "CHARACTER",     SCR_STATS   },
        { "OPTIONS",       SCR_OPTIONS },
        { "EXIT",          SCR_MENU    },
    };
    const i32 n   = (i32)( sizeof( opts ) / sizeof( opts[ 0 ] ) );
    const f32 gap = 10.0f;

    gui_rect_t col = ui_place( screen, 300.0f, ui_span( n, 44.0f, gap ), GUI_ALIGN_CENTER );
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
    Stub screens -- placeholders the later increments replace.  Same skeleton on purpose:
    a titled screen with a centered panel and a BACK button proves the layer reuses cleanly.
==============================================================================================*/

static void
screen_stub( gui_vp_t vp, const char* id, const char* title, const char* note )
{
    gui_rect_t screen = ui_screen_begin( vp, id );

    ui_title( ui_cut_top( &screen, 96.0f ), title );

    /* centered 480 x 200 panel with the increment note inside */
    gui_rect_t panel = ui_place( screen, 480.0f, 200.0f, GUI_ALIGN_CENTER );
    ui_panel( panel );
    ui_label_c( ui_inset( panel, 16.0f ), GUI_ALIGN_CENTER, ui_style()->text_dim, note );

    /* BACK: a 160 x 40 button 24px below the panel, horizontally centered on it */
    gui_rect_t back = ui_place( panel, 160.0f, 40.0f, GUI_ALIGN_HCENTER | GUI_ALIGN_BOTTOM );
    back.y = panel.y + panel.h + 24.0f;
    if ( ui_button( back, "BACK" ) || gui()->is_key_pressed( APP_KEY_ESCAPE ) )
        s_screen = SCR_MENU;

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
        case SCR_GAME:    screen_stub( vp, "game",    "SANCTUM",    "increment 2: HUD -- action bar, potions, globes" ); break;
        case SCR_SKILLS:  screen_stub( vp, "skills",  "SKILL TREE", "increment 3: node web on a fixed lattice"        ); break;
        case SCR_EQUIP:   screen_stub( vp, "equip",   "EQUIPMENT",  "increment 3: paper doll + slot grid"             ); break;
        case SCR_STATS:   screen_stub( vp, "stats",   "CHARACTER",  "increment 3: attribute list"                     ); break;
        case SCR_OPTIONS: screen_stub( vp, "options", "OPTIONS",    "later: settings forms"                           ); break;
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

    f32 dt = 0.0f;
    while ( !s_quit && gui()->frame_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame( vp0 );
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->present_begin( NULL );
        gui()->present_end();
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
