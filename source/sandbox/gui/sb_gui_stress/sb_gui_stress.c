/*==============================================================================================

    sandbox/gui/sb_gui_stress/sb_gui_stress.c -- gui load-limit stress bench.

    Five routines, each hammering a different axis of the pipeline, switched live with the
    number keys (fenced by want_capture_keyboard so typing in a field never switches):

      1  WINDOW FLOOD    -- dozens of live floaters with animated content: window records,
                            slot sort, hover contest, per-window segments.
      2  WIDGET WALL     -- one window, thousands of interactive widgets in columns: layout
                            engine, keyed state pool, id stack, nav item registration.
      3  TABLE AVALANCHE -- one striped scrolling table, thousands of rows: the table path
                            emits every row (no virtualization) -- the honest worst case.
      4  DRAW STORM      -- thousands of animated primitives on one canvas: tessellation,
                            vertex volume, the retained cache's dirty path every frame.
      5  STATE CHURN     -- hundreds of anim_f32 dampers on unique ids: tiny-class state
                            slots at/over capacity, probe chains, wants_redraw stepping.
      0  IDLE            -- control panel only; the clean-frame skip should engage.

    Tests 1/4/5 pin force_redraw (time-driven visuals must not idle-skip); 2/3 are static
    between interactions, so the retained replay path is part of what they measure.

    The perf overlay (F1-style debug hotkeys are live: debug_enable + P) is the intended
    readout -- emit ms, tess counts, wins retained, and the st tiny/small/big load rows.

==============================================================================================*/

#include <stdio.h>
#include <math.h>

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
    Bench state
==============================================================================================*/

/* This target compiles against the gui_stress lib variant (orb.targets), where GUI_STRESS_TEST
   raises the library's pool caps ~4x -- the slider ceilings scale to match.  The stock-cap
   ceilings remain so the bench still builds against the plain gui lib if ever repointed. */
#ifdef GUI_STRESS_TEST
#define STRESS_FLOOD_MAX   120     // test 1 window cap (window pool is 128 here)
#define STRESS_WALL_MAX    4000    // test 2 row cap
#define STRESS_TABLE_MAX   20000   // test 3 row cap
#define STRESS_STORM_MAX   80000   // test 4 primitive cap
#define STRESS_CHURN_MAX   2400    // test 5 damper cap
#define STRESS_TINY_SLOTS  2048    // mirrors GUI_DEFAULT_STATE_SLOTS in the gui_stress lib
#else
#define STRESS_FLOOD_MAX   80      // test 1 window cap
#define STRESS_WALL_MAX    1000    // test 2 row cap
#define STRESS_TABLE_MAX   5000    // test 3 row cap
#define STRESS_STORM_MAX   20000   // test 4 primitive cap
#define STRESS_CHURN_MAX   600     // test 5 damper cap
#define STRESS_TINY_SLOTS  512     // mirrors GUI_DEFAULT_STATE_SLOTS in the stock gui lib
#endif

static i32  s_test        = 0;     // active routine, 0 = idle
static i32  s_flood_count = 40;
static i32  s_wall_rows   = 250;
static i32  s_table_rows  = 1000;
static i32  s_storm_count = 4000;
static i32  s_churn_count = 400;

static f32  s_dt_avg      = 0.0f;  // exponential frame-time average for the readout

/* Per-widget backing for the flood / wall tests (interaction has to write somewhere). */
static bool s_flood_check[ STRESS_FLOOD_MAX ];
static bool s_wall_check [ STRESS_WALL_MAX ];
static f32  s_wall_value [ STRESS_WALL_MAX ];

/* Knuth multiplicative hash -- all per-item variety (position, color, phase) derives here. */
static u32
stress_hash( u32 i )
{
    return i * 2654435761u;
}

static u32
stress_color( u32 h )
{
    /* Bright-ish opaque ABGR from three hash bytes. */
    u32 r = 96 + ( ( h       ) & 127 );
    u32 g = 96 + ( ( h >> 8  ) & 127 );
    u32 b = 96 + ( ( h >> 16 ) & 127 );
    return 0xFF000000u | ( b << 16 ) | ( g << 8 ) | r;
}

/*==============================================================================================
    Test 1 -- WINDOW FLOOD: many live floaters, each with time-driven content
==============================================================================================*/

static void
stress_window_flood( void )
{
    f64 t = gui()->get_time();

    for ( i32 i = 0; i < s_flood_count; ++i )
    {
        char title[ 32 ];
        snprintf( title, sizeof( title ), "Flood %02d", i );

        u32 h = stress_hash( ( u32 )i );
        gui()->window_set_next_pos ( 20.0f + ( f32 )( i % 8 ) * 152.0f,
                                     70.0f + ( f32 )( i / 8 ) * 128.0f, GUI_COND_ONCE );
        gui()->window_set_next_size( 144.0f, 118.0f, GUI_COND_ONCE );
        if ( gui()->window_begin( title, GUI_WIN_NONE ) )
        {
            gui()->stack();
            f32 frac = ( f32 )fmod( t * 0.3 + ( f64 )( h % 100 ) * 0.01, 1.0 );
            gui()->progress_bar( frac, NULL );
            gui()->checkbox( "tick", &s_flood_check[ i ] );
            if ( gui()->small_button( "poke" ) )
                s_flood_check[ i ] = !s_flood_check[ i ];
        }
        gui()->window_end();
    }
}

/*==============================================================================================
    Test 2 -- WIDGET WALL: one window, thousands of interactive widgets in columns
==============================================================================================*/

static void
stress_widget_wall( void )
{
    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 560.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Widget Wall", GUI_WIN_NONE ) )
    {
        gui()->cols( ( f32[] ){ 70.0f, 60.0f, 1.0f, 60.0f, GUI_END } );
        for ( i32 i = 0; i < s_wall_rows; ++i )
        {
            gui()->push_id_int( i );
            gui()->textf( "row %d", i );
            gui()->checkbox( "##c", &s_wall_check[ i ] );
            gui()->slider_float( "##s", &s_wall_value[ i ], 0.0f, 1.0f );
            if ( gui()->small_button( "go" ) )
                s_wall_value[ i ] = 0.0f;
            gui()->pop_id();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Test 3 -- TABLE AVALANCHE: one striped scrolling table, thousands of emitted rows
==============================================================================================*/

static void
stress_table_avalanche( void )
{
    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 700.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Table Avalanche", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( gui()->table_begin( "##stress", 6,
                                 GUI_TABLE_BORDERS_OUTER | GUI_TABLE_BORDERS_V
                                 | GUI_TABLE_ROW_STRIPES | GUI_TABLE_SCROLL_Y, 0.0f ) )
        {
            gui()->table_setup_column( "Name",   GUI_TABLE_COL_STRETCH, 0.0f  );
            gui()->table_setup_column( "Id",     GUI_TABLE_COL_FIXED,   70.0f );
            gui()->table_setup_column( "Kind",   GUI_TABLE_COL_FIXED,   70.0f );
            gui()->table_setup_column( "Size",   GUI_TABLE_COL_FIXED,   80.0f );
            gui()->table_setup_column( "Crc",    GUI_TABLE_COL_FIXED,   90.0f );
            gui()->table_setup_column( "State",  GUI_TABLE_COL_FIXED,   70.0f );
            gui()->table_headers_row();

            static const char* k_kind [] = { "mesh", "tex", "sfx", "mat", "anim" };
            static const char* k_state[] = { "cold", "warm", "live" };

            for ( i32 i = 0; i < s_table_rows; ++i )
            {
                u32 h = stress_hash( ( u32 )i );
                gui()->table_next_row( 0.0f );
                gui()->table_next_column(); gui()->textf( "asset_%04d", i );
                gui()->table_next_column(); gui()->textf( "%u", h & 0xFFFF );
                gui()->table_next_column(); gui()->text ( k_kind[ h % 5 ] );
                gui()->table_next_column(); gui()->textf( "%u KB", ( h >> 8 ) % 4096 );
                gui()->table_next_column(); gui()->textf( "%08x", h );
                gui()->table_next_column(); gui()->text ( k_state[ ( h >> 16 ) % 3 ] );
            }
            gui()->table_end();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Test 4 -- DRAW STORM: thousands of animated primitives on one canvas
==============================================================================================*/

static void
stress_draw_storm( void )
{
    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 820.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Draw Storm", GUI_WIN_NONE ) )
    {
        gui()->stack();
        f32        avail = gui()->view_avail().y;
        gui_rect_t c     = gui()->canvas( avail > 40.0f ? avail : 40.0f );
        f32        t     = ( f32 )gui()->get_time();

        gui()->push_clip( c.x, c.y, c.w, c.h );
        for ( i32 i = 0; i < s_storm_count; ++i )
        {
            u32 h   = stress_hash( ( u32 )i );
            f32 x   = c.x + ( f32 )( h % 1024 )          * ( 1.0f / 1024.0f ) * c.w;
            f32 y   = c.y + ( f32 )( ( h >> 10 ) % 1024 ) * ( 1.0f / 1024.0f ) * c.h;
            f32 r   = 3.0f + ( f32 )( ( h >> 20 ) & 15 ) * 0.6f;
            f32 spin = t * ( 0.4f + ( f32 )( h & 7 ) * 0.25f );
            u32 col = stress_color( h );

            switch ( ( h >> 5 ) & 3 )
            {
                case 0: gui()->draw_circle( x, y, r, ( h & 1 ) != 0, 1.5f, col );          break;
                case 1: gui()->draw_ngon  ( x, y, r, 3 + ( ( h >> 7 ) & 3 ), spin,
                                            ( h & 2 ) != 0, 1.5f, col );                   break;
                case 2: gui()->draw_line  ( x - r, y, x + r * cosf( spin ),
                                            y + r * sinf( spin ), 1.5f, col );             break;
                default: gui()->draw_arc  ( x, y, r, spin, spin + 4.0f, 2.0f, col );       break;
            }
        }
        gui()->pop_clip();
    }
    gui()->window_end();
}

/*==============================================================================================
    Test 5 -- STATE CHURN: hundreds of anim_f32 dampers on unique ids (tiny-class pressure)
==============================================================================================*/

static void
stress_state_churn( void )
{
    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 820.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "State Churn", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "Each chip = one anim_f32 damper on a unique id.  Tiny state class" );
        gui()->textf( "here is %d slots -- push the count past it and watch 'st tiny' in",
                      STRESS_TINY_SLOTS );
        gui()->text ( "the perf overlay (debug hotkeys are live in this bench)." );

        f32        avail = gui()->view_avail().y;
        gui_rect_t c     = gui()->canvas( avail > 40.0f ? avail : 40.0f );
        f64        t     = gui()->get_time();

        gui()->push_clip( c.x, c.y, c.w, c.h );
        for ( i32 i = 0; i < s_churn_count; ++i )
        {
            u32 h      = stress_hash( ( u32 )i );
            f32 period = 0.6f + ( f32 )( h % 100 ) * 0.014f;
            f32 target = ( fmod( t / ( f64 )period + ( f64 )( ( h >> 8 ) & 7 ) * 0.25, 2.0 ) < 1.0 )
                             ? 0.0f : 1.0f;
            f32 f      = gui()->anim_f32( 0x53435231u + ( u32 )i, target, 5.0f );

            f32 lane_h = c.h / ( f32 )s_churn_count;
            f32 y      = c.y + ( f32 )i * lane_h;
            f32 x      = c.x + f * ( c.w - 12.0f );
            gui()->draw_rect( x, y, 12.0f, lane_h > 1.0f ? lane_h : 1.0f, stress_color( h ) );
        }
        gui()->pop_clip();
    }
    gui()->window_end();
}

/*==============================================================================================
    Control panel -- always shown; the only window in test 0 (idle-skip should engage there)
==============================================================================================*/

static const char* k_test_name[] = {
    "0  IDLE (clean-frame skip)",
    "1  WINDOW FLOOD",
    "2  WIDGET WALL",
    "3  TABLE AVALANCHE",
    "4  DRAW STORM",
    "5  STATE CHURN",
};

static void
show_control( void )
{
    gui()->window_set_next_pos ( 900.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 350.0f, 420.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Stress Control", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "test: %s", k_test_name[ s_test ] );
        f32 ms = s_dt_avg * 1000.0f;
        gui()->textf( "frame: %.2f ms  (%.0f fps)", ms, ms > 0.001f ? 1000.0f / ms : 0.0f );
        gui()->textf( "dirty: %s", gui()->frame_dirty() ? "yes" : "no (replaying)" );
        gui()->separator();

        gui()->text( "Keys 1-5 select a routine, 0 stops." );
        gui()->cols_n( 3 );
        for ( i32 i = 0; i <= 5; ++i )
        {
            char label[ 8 ];
            snprintf( label, sizeof( label ), "%d", i );
            gui()->push_id_int( i );
            if ( gui()->button( label ) )
                s_test = i;
            gui()->pop_id();
        }

        gui()->stack();
        gui()->separator_text( "load" );
        gui()->slider_int( "flood wins",  &s_flood_count, 1,   STRESS_FLOOD_MAX );
        gui()->slider_int( "wall rows",   &s_wall_rows,   10,  STRESS_WALL_MAX  );
        gui()->slider_int( "table rows",  &s_table_rows,  100, STRESS_TABLE_MAX );
        gui()->slider_int( "storm prims", &s_storm_count, 100, STRESS_STORM_MAX );
        gui()->slider_int( "churn chips", &s_churn_count, 50,  STRESS_CHURN_MAX );

        gui()->separator();
        gui()->text_wrapped( "Perf overlay has the real numbers: emit / tess / render ms, "
                             "windows retained, and state pool load.  Tests 1/4/5 pin "
                             "force_redraw; 2/3 go clean between interactions." );
    }
    gui()->window_end();
}

/*==============================================================================================
    Build -- key routing first, then the active routine, then the control panel
==============================================================================================*/

static void
build_frame( void )
{
    /* Number-key switching, fenced so typing digits into a field never flips the bench. */
    if ( !gui()->want_capture_keyboard() )
    {
        for ( i32 k = 0; k <= 5; ++k )
            if ( gui()->is_key_pressed( ( app_key_t )( APP_KEY_0 + k ) ) )
                s_test = k;
    }

    /* Time-driven visuals must not idle-skip; static tests measure the replay path instead. */
    gui()->set_force_redraw( s_test == 1 || s_test == 4 || s_test == 5 );

    switch ( s_test )
    {
        case 1: stress_window_flood();    break;
        case 2: stress_widget_wall();     break;
        case 3: stress_table_avalanche(); break;
        case 4: stress_draw_storm();      break;
        case 5: stress_state_churn();     break;
        default:                          break;
    }

    show_control();
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
        fprintf( stderr, "[sb_gui_stress] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui stress",
        .w     = 1280, .h = 720,
        .font  = GUI_FONT_JETBRAINS_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
        .debug = true,    // perf overlay + pipeline dashboard are the bench readout
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_stress] gui->boot failed\n" );
        goto shutdown;
    }

    gui()->debug_enable( true );   /* F1-style hotkeys are live in this bench. */
    gui()->print_mem_stats();

    f32 dt = 0.0f;
    while ( gui()->frame_poll( &dt ) )
    {
        /* Exponential frame-time average -- smooth enough to read, fast enough to react. */
        s_dt_avg += ( dt - s_dt_avg ) * 0.05f;

        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            build_frame();
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
