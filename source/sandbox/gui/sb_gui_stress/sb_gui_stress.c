/*==============================================================================================

    sandbox/gui/sb_gui_stress/sb_gui_stress.c -- gui load-limit stress bench.

    Ten routines, each hammering a different axis of the pipeline, switched live with the
    number keys (fenced by want_capture_keyboard so typing in a field never switches):

      1  WINDOW FLOOD    -- dozens of live floaters with animated content: window records,
                            slot sort, hover contest, per-window segments.
      2  WIDGET WALL     -- one window, thousands of interactive widgets in columns: layout
                            engine, keyed state pool, id stack, nav item registration.
      3  TABLE AVALANCHE -- one striped scrolling table, thousands of rows: emits every row
                            with the clip toggle off -- the honest worst case.
      4  DRAW STORM      -- thousands of animated primitives on one canvas: tessellation,
                            vertex volume, the retained cache's dirty path every frame.
      5  STATE CHURN     -- hundreds of anim_f32 dampers on unique ids: tiny-class state
                            slots at/over capacity, probe chains, wants_redraw stepping.
      6  DOCK CYCLONE    -- a dockspace whose tabs re-shuffle at random every tick and whose
                            whole tree is torn down + recarved on a timer: dock node churn,
                            tab strips, active-tab flips, undock/redock, emptied-leaf collapse.
      7  LAYOUT ROULETTE -- one window whose body layout re-randomizes each generation on a
                            FRESH id scope: keyed state slots created + orphaned in bulk
                            (headers/trees/regions), nav re-registration, layout switching.
      8  VOLATILE SWARM  -- dozens of volatile_cb blocks animating at once while the rest of
                            the UI goes clean: replay-by-retessellation with many sub-slots,
                            graceful degrade past GUI_MAX_VOLATILE (overflow blocks freeze).
      9  FULL SIEGE      -- routines 1-5 all at once at fractional load: mixed dirty windows,
                            slot sort + segment volume + state pressure in the same frame.
      0  IDLE            -- control panel only; the clean-frame skip should engage.

    Tests 1/4/5/6/7/9 pin force_redraw (time-driven or self-mutating -- they must not
    idle-skip); 2/3 are static between interactions, so the retained replay path is part of
    what they measure; 8 deliberately does NOT force redraw -- idle-frame volatile replay IS
    the thing it measures.

    The control panel's "clip offscreen rows" toggle (on by default) A/Bs the rows_clip /
    table_rows_clip virtualization on tests 2/3: on, only the visible span emits and the cost
    stops scaling with total row count; off is the emit-everything worst case above.

    The perf overlay (F1-style debug hotkeys are live: debug_enable + P) is the intended
    readout -- emit ms, tess counts, wins retained, and the st tiny/small/big load rows.

==============================================================================================*/

#include <stdio.h>
#include <math.h>

#include "orb.h"
#include "base/fmt.h"   // fmt_snprintf -- the bench's per-row formatting is part of what emit ms measures
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

/* The bench runs against the SHIPPING gui library, at the caps everything else in the tree gets.
   Several of the ceilings below sit deliberately past a pool's capacity -- overflowing one is a
   result, not a failure, and the sticky overflow flag plus the dashboard are how it reads. */

#define STRESS_FLOOD_MAX   80      // test 1 window cap
#define STRESS_WALL_MAX    1000    // test 2 row cap
#define STRESS_TABLE_MAX   5000    // test 3 row cap
#define STRESS_STORM_MAX   1200    // test 4 primitive cap (~30 verts/prim avg vs the 32K vert pool)
#define STRESS_CHURN_MAX   600     // test 5 damper cap
#define STRESS_TINY_SLOTS  512     // mirrors GUI_DEFAULT_STATE_SLOTS
#define STRESS_DOCK_MAX    12      // test 6 docked-window cap
#define STRESS_MUT_MAX     100     // test 7 row cap (big-class slots are 32 -- overflows them)
#define STRESS_SWARM_MAX   24      // test 8 block cap (GUI_MAX_VOLATILE is 16 -- deliberate overflow)

static i32  s_test        = 0;     // active routine, 0 = idle
static i32  s_flood_count = 40;
static i32  s_wall_rows   = 250;
static i32  s_table_rows  = 1000;
static i32  s_storm_count = 800;
static i32  s_churn_count = 400;
static i32  s_dock_count  = 10;
static i32  s_mut_rows    = 60;
static i32  s_swarm_count = 12;
static bool s_clip        = true;  // tests 2/3: virtualize offscreen rows via rows_clip

static f32  s_dt_avg      = 0.0f;  // exponential frame-time average for the readout

/* Per-widget backing for the interactive tests (interaction has to write somewhere). */
static bool s_flood_check[ STRESS_FLOOD_MAX ];
static bool s_wall_check [ STRESS_WALL_MAX ];
static f32  s_wall_value [ STRESS_WALL_MAX ];
static bool s_dock_check [ STRESS_DOCK_MAX ];
static bool s_mut_check  [ STRESS_MUT_MAX ];
static f32  s_mut_value  [ STRESS_MUT_MAX ];

/* Knuth multiplicative hash -- all per-item variety (position, color, phase) derives here. */
static u32
stress_hash( u32 i )
{
    return i * 2654435761u;
}

/* LCG for the decisions that must differ frame to frame (dock shuffles) -- stress_hash gives
   the same answer for the same index, which is exactly wrong for a randomized schedule. */
static u32
stress_rand( void )
{
    static u32 s_rng = 0x9E3779B9u;
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
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
stress_window_flood( i32 count )
{
    f64 t = gui()->get_time();

    for ( i32 i = 0; i < count; ++i )
    {
        char title[ 32 ];
        fmt_snprintf( title, sizeof( title ), "Flood %02d", i );

        u32 h = stress_hash( ( u32 )i );
        gui()->window_set_next_pos ( 20.0f + ( f32 )( i % 12 ) * 152.0f,
                                     70.0f + ( f32 )( i / 12 ) * 128.0f, GUI_COND_ONCE );
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
stress_widget_wall( i32 rows )
{
    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 560.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Widget Wall", GUI_WIN_NONE ) )
    {
        /* Fixed row pitch for BOTH paths (rows_clip needs one, and bare cols() auto rows would
           key off the first item's text height) -- so the toggle A/Bs cost, not geometry. */
        f32 rh = gui()->sz_rows_h( 1 ) - 2.0f * gui()->sz_row_gap();   /* one WIDGET_H row */
        gui()->row_cols( rh, ( f32[] ){ 70.0f, 60.0f, 1.0f, 60.0f, GUI_END } );

        i32 first = 0, last = rows;
        if ( s_clip )
        {
            gui_span_t s = gui()->rows_clip( rows, rh );
            first = s.first;  last = s.last;
        }

        for ( i32 i = first; i < last; ++i )
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
stress_table_avalanche( i32 rows )
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

            i32 first = 0, last = rows;
            if ( s_clip )
            {
                gui_span_t s = gui()->table_rows_clip( rows, 0.0f );
                first = s.first;  last = s.last;
            }

            for ( i32 i = first; i < last; ++i )
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
stress_draw_storm( i32 count )
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
        for ( i32 i = 0; i < count; ++i )
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
stress_state_churn( i32 count )
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
        for ( i32 i = 0; i < count; ++i )
        {
            /* The target must MOVE CONTINUOUSLY: anim_f32 self-evicts once settled and seeds a
               history-less channel AT its target, so a square-wave target just teleports with
               zero slots held.  A sine keeps every damper in flight -- one live tiny slot each. */
            u32 h      = stress_hash( ( u32 )i );
            f32 freq   = 0.5f + ( f32 )( h % 100 ) * 0.03f;
            f32 target = 0.5f + 0.5f * sinf( ( f32 )t * freq + ( f32 )( ( h >> 8 ) & 255 ) * 0.025f );
            f32 f      = gui()->anim_f32( 0x53435231u + ( u32 )i, target, 6.0f );

            f32 lane_h = c.h / ( f32 )count;
            f32 y      = c.y + ( f32 )i * lane_h;
            f32 x      = c.x + f * ( c.w - 12.0f );
            gui()->draw_rect( x, y, 12.0f, lane_h > 1.0f ? lane_h : 1.0f, stress_color( h ) );
        }
        gui()->pop_clip();
    }
    gui()->window_end();
}

/*==============================================================================================
    Test 6 -- DOCK CYCLONE: tabs re-shuffle at random every tick; the whole tree recarves on
    a timer.  dock_window() with a stale leaf id (the node collapsed when it emptied) is a
    safe no-op, so the shuffle never validates -- exercising exactly that path is the point.
==============================================================================================*/

static gui_dock_id_t s_dock_leaf[ 5 ];
static i32           s_dock_leaf_count = 0;
static f64           s_dock_rebuild_at = 0.0;   // 0 = rebuild immediately on entry
static f64           s_dock_shuffle_at = 0.0;

static void
stress_dock_cyclone( i32 count )
{
    f64 t = gui()->get_time();

    /* Timer-driven teardown: destroy the tree wholesale (safe point: top of build, before any
       docked window's begin).  The empty leaf list below triggers the recarve this frame. */
    if ( t >= s_dock_rebuild_at )
    {
        s_dock_rebuild_at = t + 6.0;
        gui()->dock_clear( 0 );
        s_dock_leaf_count = 0;
    }

    gui_dock_id_t root = gui()->dockspace_over_viewport( 0, GUI_DOCKSPACE_NONE );

    if ( s_dock_leaf_count == 0 && root != GUI_DOCK_NONE )
    {
        /* Fresh 5-leaf layout, then scatter every window across it at random. */
        gui_dock_id_t left   = gui()->dock_split( root, GUI_DIR_LEFT,  0.25f, &root );
        gui_dock_id_t right  = gui()->dock_split( root, GUI_DIR_RIGHT, 0.30f, &root );
        gui_dock_id_t bottom = gui()->dock_split( root, GUI_DIR_DOWN,  0.35f, &root );
        gui_dock_id_t lbot   = gui()->dock_split( left, GUI_DIR_DOWN,  0.50f, &left );

        gui_dock_id_t carve[] = { left, lbot, right, bottom, root };
        for ( u32 c = 0; c < 5; ++c )
            if ( carve[ c ] != GUI_DOCK_NONE )
                s_dock_leaf[ s_dock_leaf_count++ ] = carve[ c ];

        for ( i32 i = 0; i < count; ++i )
        {
            char title[ 32 ];
            fmt_snprintf( title, sizeof( title ), "Dock %02d", i );
            gui()->dock_window( title, s_dock_leaf[ stress_rand() % ( u32 )s_dock_leaf_count ] );
        }
        s_dock_shuffle_at = t + 0.35;
    }
    else if ( t >= s_dock_shuffle_at && s_dock_leaf_count > 0 )
    {
        /* Shuffle tick: move one random window to a random leaf (the add re-tabs it out of its
           old node and makes it the ACTIVE tab there -- the visible tab flip), or undock it to
           free-float until a later tick or the next recarve sweeps it back in. */
        s_dock_shuffle_at = t + 0.35;
        u32  w = stress_rand() % ( u32 )count;
        char title[ 32 ];
        fmt_snprintf( title, sizeof( title ), "Dock %02d", ( i32 )w );

        if ( ( stress_rand() & 3 ) == 0 )
            gui()->dock_undock( title );
        else
            gui()->dock_window( title, s_dock_leaf[ stress_rand() % ( u32 )s_dock_leaf_count ] );
    }

    for ( i32 i = 0; i < count; ++i )
    {
        char title[ 32 ];
        fmt_snprintf( title, sizeof( title ), "Dock %02d", i );

        u32 h = stress_hash( ( u32 )i );
        gui()->window_set_next_pos ( 60.0f + ( f32 )( i % 6 ) * 40.0f,
                                     90.0f + ( f32 )( i / 6 ) * 40.0f, GUI_COND_ONCE );
        gui()->window_set_next_size( 260.0f, 180.0f, GUI_COND_ONCE );
        if ( gui()->window_begin( title, GUI_WIN_NONE ) )
        {
            gui()->stack();
            f32 frac = ( f32 )fmod( t * 0.4 + ( f64 )( h % 100 ) * 0.01, 1.0 );
            gui()->progress_bar( frac, NULL );
            gui()->textf( "docked: %s", gui()->window_is_docked( title ) ? "yes" : "no (floating)" );
            gui()->checkbox( "tick", &s_dock_check[ i ] );
        }
        gui()->window_end();
    }
}

/*==============================================================================================
    Test 7 -- LAYOUT ROULETTE: the body re-randomizes each generation under a FRESH id scope,
    so every stateful widget it spawned last generation (headers, trees, nav entries) orphans
    its slots and the new generation allocates its own -- bulk create + abandon in the keyed
    state pool, the failure mode the churn test's stable ids never reach.
==============================================================================================*/

static void
stress_layout_roulette( i32 rows )
{
    static u32 s_mut_gen  = 0;
    static f64 s_mut_next = 0.0;

    f64 t = gui()->get_time();
    if ( t >= s_mut_next )
    {
        s_mut_next = t + 0.7;
        s_mut_gen++;
    }

    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 640.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Layout Roulette", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "generation %u -- every id below is scoped to it; watch the st rows in "
                      "the perf overlay saw-tooth as slots orphan and refill.", s_mut_gen );
        gui()->separator();

        gui()->push_id_int( ( i32 )s_mut_gen );
        for ( i32 i = 0; i < rows; ++i )
        {
            u32 h = stress_hash( s_mut_gen * 0x01000193u + ( u32 )i );

            /* Re-roll the column layout every few rows -- the layout engine flips between
               flow and 2/3/4-track grids mid-body, at generation-random seams. */
            if ( i % 7 == 0 )
            {
                i32 tracks = 1 + ( i32 )( ( h >> 9 ) % 4 );
                if ( tracks == 1 ) gui()->stack();
                else               gui()->cols_n( tracks );
            }

            gui()->push_id_int( i );
            switch ( h % 8 )
            {
                case 0: gui()->checkbox( "##c", &s_mut_check[ i ] );                    break;
                case 1: gui()->slider_float( "##s", &s_mut_value[ i ], 0.0f, 1.0f );    break;
                case 2: if ( gui()->small_button( "poke" ) )
                        {
                            s_mut_value[ i ] = 1.0f;
                        }                                                                break;
                case 3: gui()->textf( "item %04u", h & 0xFFF );                          break;
                case 4: gui()->progress_bar( ( f32 )( h % 100 ) * 0.01f, NULL );         break;
                case 5: if ( gui()->collapsing_header( "header" ) )
                        {
                            gui()->textf( "payload %08x", h );
                            gui()->textf( "payload %08x", ~h );
                        }                                                                break;
                case 6: if ( gui()->tree_node( "node" ) )
                        {
                            gui()->textf( "leaf %04u", h & 0xFFF );
                            gui()->tree_pop();
                        }                                                                break;
                default: gui()->bullet_text( "bullet" );                                 break;
            }
            gui()->pop_id();
        }
        gui()->pop_id();
    }
    gui()->window_end();
}

/*==============================================================================================
    Test 8 -- VOLATILE SWARM: many volatile_cb blocks animating while the rest of the UI goes
    clean.  One shared callback serves every block: on replay a callback gets no index, so all
    per-block variety derives from the canvas rect it is handed (position is identity here).
    Deliberately does NOT force redraw -- idle-frame replay of N sub-slots IS the measurement.
==============================================================================================*/

/* CONTRACT: fixed layout footprint -- the canvas height and command count never change;
   only pixel content (fill width, arc angle, color) animates.  All per-block variety derives
   from the block id (stable at real emit AND on replay) -- never from the canvas rect, which
   legitimately moves under resize/relayout and would re-roll color/phase with every pixel. */
static void
stress_swarm_block_cb( gui_id_t id, bool is_replay )
{
    UNUSED( is_replay );
    gui()->volatile_begin();

    gui_rect_t r = gui()->canvas( 26.0f );
    f32        t = ( f32 )sys_tick_seconds();
    u32        h = stress_hash( ( u32 )id );
    f32        ph = ( f32 )( h % 628 ) * 0.01f;
    f32        s  = 0.5f + 0.5f * sinf( t * ( 2.0f + ( f32 )( h & 3 ) ) + ph );

    f32 bar_max = r.w - 34.0f > 8.0f ? r.w - 34.0f : 8.0f;
    gui()->draw_rect( r.x, r.y + 4.0f, 4.0f + bar_max * s, 18.0f, stress_color( h ) );
    gui()->draw_arc ( r.x + r.w - 22.0f, r.y + 13.0f, 9.0f,
                      t * 3.0f + ph, t * 3.0f + ph + 4.0f, 2.0f, stress_color( h >> 3 ) );

    gui()->volatile_end();
}

static void
stress_volatile_swarm( i32 count )
{
    gui()->window_set_next_pos ( 40.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 640.0f, 620.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Volatile Swarm", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( "Leave the mouse still: the window goes clean, yet every block "
                             "keeps animating via idle-frame volatile replay.  Blocks past "
                             "GUI_MAX_VOLATILE degrade gracefully -- they freeze on idle "
                             "frames instead of replaying." );
        gui_render_stats_t rs = gui()->render_stats();
        gui()->textf( "volatile_patched last frame: %u", rs.volatile_patched );
        gui()->separator();

        gui()->cols_n( 4 );
        for ( i32 i = 0; i < count; ++i )
        {
            gui()->push_id_int( i );
            gui()->volatile_cb( "swarm_blk", stress_swarm_block_cb );
            gui()->pop_id();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Test 9 -- FULL SIEGE: routines 1-5 at once at fractional load.  No single axis peaks; the
    stress is the mix -- animated floaters dirtying every frame over static walls/tables that
    want to retain, all contesting slot sort, segments, and the state pool together.
==============================================================================================*/

static void
stress_full_siege( void )
{
    i32 flood = s_flood_count / 3;   if ( flood < 8   ) flood = 8;
    i32 wall  = s_wall_rows   / 4;   if ( wall  < 50  ) wall  = 50;
    i32 table = s_table_rows  / 4;   if ( table < 200 ) table = 200;
    i32 storm = s_storm_count / 2;   if ( storm < 200 ) storm = 200;
    i32 churn = s_churn_count / 2;   if ( churn < 100 ) churn = 100;

    stress_window_flood   ( flood );
    stress_widget_wall    ( wall  );
    stress_table_avalanche( table );
    stress_draw_storm     ( storm );
    stress_state_churn    ( churn );
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
    "6  DOCK CYCLONE",
    "7  LAYOUT ROULETTE",
    "8  VOLATILE SWARM",
    "9  FULL SIEGE",
};

static void
show_control( void )
{
    gui()->window_set_next_pos ( 900.0f, 70.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 350.0f, 520.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Stress Control", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "test: %s", k_test_name[ s_test ] );
        f32 ms = s_dt_avg * 1000.0f;
        gui()->textf( "frame: %.2f ms  (%.0f fps)", ms, ms > 0.001f ? 1000.0f / ms : 0.0f );
        gui()->textf( "dirty: %s", gui()->frame_dirty() ? "yes" : "no (replaying)" );
        gui()->separator();

        gui()->text( "Keys 1-9 select a routine, 0 stops." );
        gui()->cols_n( 5 );
        for ( i32 i = 0; i <= 9; ++i )
        {
            char label[ 8 ];
            fmt_snprintf( label, sizeof( label ), "%d", i );
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
        gui()->slider_int( "dock wins",   &s_dock_count,  4,   STRESS_DOCK_MAX  );
        gui()->slider_int( "mutate rows", &s_mut_rows,    20,  STRESS_MUT_MAX   );
        gui()->slider_int( "swarm blks",  &s_swarm_count, 4,   STRESS_SWARM_MAX );
        gui()->checkbox( "clip offscreen rows (2/3)", &s_clip );

        gui()->separator();
        gui()->text_wrapped( "Perf overlay has the real numbers: emit / tess / render ms, "
                             "windows retained, and state pool load.  Tests 1/4/5/6/7/9 pin "
                             "force_redraw; 2/3 go clean between interactions; 8 measures "
                             "idle-frame volatile replay." );
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
        for ( i32 k = 0; k <= 9; ++k )
            if ( gui()->is_key_pressed( ( app_key_t )( APP_KEY_0 + k ) ) )
                s_test = k;
    }

    /* Entering the cyclone re-arms an immediate recarve (its tree may be stale-dormant from a
       previous visit); leaving lets the dockspace go dormant on its own by not being emitted. */
    static i32 s_prev_test = 0;
    if ( s_test == 6 && s_prev_test != 6 )
        s_dock_rebuild_at = 0.0;
    s_prev_test = s_test;

    /* Time-driven / self-mutating tests must not idle-skip; static tests measure the replay
       path instead -- and the swarm (8) measures idle-frame volatile replay itself. */
    gui()->set_force_redraw( s_test == 1 || s_test == 4 || s_test == 5
                             || s_test == 6 || s_test == 7 || s_test == 9 );

    switch ( s_test )
    {
        case 1: stress_window_flood   ( s_flood_count ); break;
        case 2: stress_widget_wall    ( s_wall_rows   ); break;
        case 3: stress_table_avalanche( s_table_rows  ); break;
        case 4: stress_draw_storm     ( s_storm_count ); break;
        case 5: stress_state_churn    ( s_churn_count ); break;
        case 6: stress_dock_cyclone   ( s_dock_count  ); break;
        case 7: stress_layout_roulette( s_mut_rows    ); break;
        case 8: stress_volatile_swarm ( s_swarm_count ); break;
        case 9: stress_full_siege     (               ); break;
        default:                                         break;
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

    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui stress",
        .w     = 1280, .h = 720,
        .font  = GUI_FONT_JETBRAINS,
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
    while ( gui()->boot_poll( &dt ) )
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

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        gui()->boot_pace ( 0, 0 ); // ( 4, 16 );
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
