/*==============================================================================================

    sandbox/gui/sb_gui_custom/sb_gui_custom.c -- custom non-chrome UI test bed.

    A chrome-free screen: no gui_window, no dockspace, just a pane and widgets built directly
    from rect + item() + draw_*, with a from-scratch palette installed via style_edit().  The
    point is proving out custom-look UI on top of the same backing gui() calls chrome itself
    uses -- style_edit / push_style_seed / item() / draw_* -- so a project that wants its own
    chrome-like set (not just tuning vars over the stock look) has a working example to grow.

    Grown in increments; each one adds to the same screen rather than replacing it.

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

// clang-format off

/*==============================================================================================
    Palette -- a from-scratch look, installed straight into the style block rather than tuned
    off the chrome defaults.  Seven seeds and a ramp is the entire kit.
==============================================================================================*/

static void
custom_kit_install( void )
{
    gui_style_t* e = gui()->style_edit();

    e->palette.seed[ GUI_SEED_SURFACE ] = GUI_COLOR( 0x10, 0x14, 0x18, 0xff );   // slate panel
    e->palette.seed[ GUI_SEED_CONTROL ] = GUI_COLOR( 0x18, 0x20, 0x26, 0xff );   // control face
    e->palette.seed[ GUI_SEED_INK     ] = GUI_COLOR( 0xd8, 0xe4, 0xea, 0xff );   // ink
    e->palette.seed[ GUI_SEED_LINE    ] = GUI_COLOR( 0x2c, 0x38, 0x40, 0xff );   // frame lines
    e->palette.seed[ GUI_SEED_ACCENT  ] = GUI_COLOR( 0x30, 0xc8, 0xb0, 0xff );   // teal accent
    e->palette.seed[ GUI_SEED_MARK    ] = GUI_COLOR( 0x30, 0xc8, 0xb0, 0xff );
    e->palette.seed[ GUI_SEED_GRAB    ] = GUI_COLOR( 0xd8, 0xe4, 0xea, 0xff );

    e->palette.ramp[ GUI_RAMP_HOVER  ] = 0.25f;
    e->palette.ramp[ GUI_RAMP_PRESS  ] = 0.45f;
    e->palette.ramp[ GUI_RAMP_FADE   ] = 0.35f;

    e->var[ GUI_VAR_BORDER ] = 1.0f;
}

/*==============================================================================================
    Custom widget -- a rect + item() + draw_*, no stock_* call in it.  Demonstrates that a
    custom-look control needs nothing chrome does not already expose.
==============================================================================================*/

static bool
custom_button( const char* id, gui_rect_t r, const char* label )
{
    gui_item_state_t st = gui()->item( id, r );

    static const u32 FACE[ GUI_PHASE_COUNT ] = {
        [ GUI_PHASE_IDLE   ] = GUI_COLOR( 0x18, 0x20, 0x26, 0xff ),
        [ GUI_PHASE_HOT    ] = GUI_COLOR( 0x20, 0x2c, 0x32, 0xff ),
        [ GUI_PHASE_ACTIVE ] = GUI_COLOR( 0x18, 0x40, 0x3a, 0xff ),
        [ GUI_PHASE_INERT  ] = GUI_COLOR( 0x14, 0x18, 0x1c, 0xff ),
    };
    u32 line = st.hover ? GUI_COLOR( 0x30, 0xc8, 0xb0, 0xff ) : GUI_COLOR( 0x2c, 0x38, 0x40, 0xff );

    gui()->draw_frame( r, FACE[ gui()->item_phase( st ) ], line, 1.0f );
    gui()->draw_text_in( r, GUI_ALIGN_CENTER, GUI_COLOR( 0xd8, 0xe4, 0xea, 0xff ), label );

    return st.clicked;
}

/*==============================================================================================
    Frame build
==============================================================================================*/

static i32 s_clicks = 0;

static void
build_frame( i32 vp )
{
    gui_pane_t p = gui()->pane_begin( "custom_pane", ( gui_rect_t ){ 60.0f, 60.0f, 420.0f, 220.0f },
                                      GUI_REGION_MID, vp, GUI_WIN_NONE );
    gui_rect_t r = p.rect;

    gui()->draw_frame( r, GUI_COLOR( 0x10, 0x14, 0x18, 0xff ), GUI_COLOR( 0x2c, 0x38, 0x40, 0xff ), 1.0f );

    gui_rect_t body = gui_rect_pad( r, 16.0f );
    gui_rect_t head = gui_rect_cut_top( &body, 24.0f );
    gui()->draw_text( head.x, head.y, GUI_COLOR( 0x30, 0xc8, 0xb0, 0xff ), "sb_gui_custom" );

    gui_rect_cut_top( &body, 8.0f );
    gui_rect_t btn = gui_rect_cut_top( &body, 32.0f );
    if ( custom_button( "##go", btn, "CLICK ME" ) )
        s_clicks++;

    char buf[ 32 ];
    snprintf( buf, sizeof( buf ), "clicks: %d", s_clicks );
    gui_rect_cut_top( &body, 8.0f );
    gui_rect_t out = gui_rect_cut_top( &body, 20.0f );
    gui()->draw_text( out.x, out.y, GUI_COLOR( 0xd8, 0xe4, 0xea, 0xff ), buf );

    gui()->pane_end();
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
        fprintf( stderr, "[sb_gui_custom] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- sb_gui_custom",
        .w     = 1280, .h = 720,
        .font  = GUI_FONT_ROBOTO,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.04f, 0.05f, 0.06f, 1.0f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_custom] gui->boot failed\n" );
        goto shutdown;
    }

    custom_kit_install();

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame( vp0 );
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        gui()->boot_pace( 4, 16 );
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
