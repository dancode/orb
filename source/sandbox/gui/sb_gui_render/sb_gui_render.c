/*==============================================================================================

    sandbox/gui/sb_gui_render/sb_gui_render.c -- render feature matrix.

    A renderer test bed, not a GUI application: output comes straight from the draw_* primitives
    with no windows, panels or widgets in the way, so what lands on screen is exactly what the
    tessellator and the gui shader produced.  Each page is a categorical sweep -- one primitive
    family crossed with the style/fx state that modifies it -- laid out on a fixed grid so a
    malformed cell is visible by comparison with its neighbours.

    Number keys select a page; the page list is the extension point.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

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
    Shared ink
==============================================================================================*/

#define INK       GUI_COLOR( 0xE8, 0xE0, 0xD0, 0xFF )
#define INK_DIM   GUI_COLOR( 0x8A, 0x88, 0x80, 0xFF )
#define AMBER     GUI_COLOR( 0xFF, 0xA0, 0x20, 0xFF )
#define TEAL      GUI_COLOR( 0x20, 0xC0, 0xB0, 0xFF )
#define PLUM      GUI_COLOR( 0xB0, 0x60, 0xE0, 0xFF )

/*==============================================================================================
    Grid -- every page walks the same cell layout, so cells line up across pages and a
    misbehaving one stands out against its row.
==============================================================================================*/

#define GRID_X      40.0f    // left margin of the first column
#define GRID_Y      70.0f    // top of the first row (below the hint line)
#define CELL_W     180.0f    // cell box, label strip included
#define CELL_H     140.0f
#define CELL_GAP    16.0f
#define LABEL_H     16.0f    // strip at the bottom of a cell holding its caption

/* Cell n of a page: its drawing area, with the caption written under it. */
static gui_rect_t
cell( i32 index, i32 columns, const char* caption )
{
    i32 col = index % columns;
    i32 row = index / columns;

    gui_rect_t box = {
        GRID_X + ( f32 )col * ( CELL_W + CELL_GAP ),
        GRID_Y + ( f32 )row * ( CELL_H + CELL_GAP ),
        CELL_W, CELL_H,
    };

    gui_rect_t strip = box;
    strip.y += box.h - LABEL_H;
    strip.h  = LABEL_H;
    gui()->draw_text_in( strip, GUI_ALIGN_CENTER, INK_DIM, caption );

    box.h -= LABEL_H;
    return box;
}

/*==============================================================================================
    Page 1 -- fills: the seed sweep every other page is measured against.
==============================================================================================*/

static void
page_fills( void )
{
    gui_rect_t r;

    r = cell( 0, 6, "rect" );
    gui()->draw_rect( r.x, r.y, r.w, r.h, AMBER );

    r = cell( 1, 6, "round rect" );
    gui()->draw_round_rect( r, 14.0f, 14.0f, 14.0f, 14.0f, 0.0f, TEAL );

    r = cell( 2, 6, "round rect border" );
    gui()->draw_round_rect( r, 14.0f, 14.0f, 14.0f, 14.0f, 2.0f, TEAL );

    r = cell( 3, 6, "gradient v" );
    gui()->draw_gradient( r, AMBER, PLUM, true );

    r = cell( 4, 6, "gradient h" );
    gui()->draw_gradient( r, AMBER, PLUM, false );

    r = cell( 5, 6, "circle" );
    gui()->draw_circle( r.x + r.w * 0.5f, r.y + r.h * 0.5f,
                        ( r.h < r.w ? r.h : r.w ) * 0.5f - 4.0f, 0.0f, PLUM );
}

/*==============================================================================================
    Pages
==============================================================================================*/

typedef struct
{
    const char* name;          // shown in the hint line
    void ( *build )( void );   // draws the page's cells
} page_t;

static const page_t s_pages[] = {
    { "fills", page_fills },
};

#define PAGE_COUNT ( ( i32 )( sizeof s_pages / sizeof s_pages[ 0 ] ) )

static i32 s_page = 0;

static void
build_frame( void )
{
    /* number-key page switching; fenced so a page that ever takes text input keeps its keys */
    if ( !gui()->want_capture_keyboard() )
        for ( i32 k = 0; k < PAGE_COUNT; ++k )
            if ( gui()->is_key_pressed( ( app_key_t )( APP_KEY_1 + k ) ) )
                s_page = k;

    char hint[ 128 ];
    snprintf( hint, sizeof hint, "sb_gui_render -- page %d/%d: %s",
              s_page + 1, PAGE_COUNT, s_pages[ s_page ].name );
    gui()->draw_text( 12.0f, 8.0f, INK_DIM, hint );

    s_pages[ s_page ].build();
}

/*==============================================================================================
    main
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
        fprintf( stderr, "[sb_gui_render] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "ORB -- gui render matrix",
        .w         = 1280, .h = 720,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.10f, 0.10f, 0.13f, 1.00f },
        .debug = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_render] gui->boot failed\n" );
        goto shutdown;
    }

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame();
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

// clang-format on
