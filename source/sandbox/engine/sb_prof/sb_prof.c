/*==============================================================================================

    sandbox/engine/sb_prof/sb_prof.c -- Test sandbox for the engine/prof profiler library.

    Exercises the whole starter surface: name registry, zone capture + drain + nesting
    reconstruction, capture macros, frame marks, counters, ring overflow, runtime disable,
    multi-threaded capture into per-thread rings, and a wall-clock sanity zone. Ends with
    an informational hot-path cost readout (ns per begin/end pair).

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/prof/prof_host.h"

/*==============================================================================================
    Check helper
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
sb_check( bool ok, const char* what )
{
    s_checks++;
    if ( !ok )
    {
        s_fails++;
        printf( "    FAIL: %s\n", what );
    }
}

/*==============================================================================================
    Drain helpers

    s_events is a full-ring scratch copy; sb_flush_all discards every pending event on
    every ring so each suite starts from empty cursors.
==============================================================================================*/

static prof_event_t s_events[ PROF_RING_CAP ];

static void
sb_flush_all( void )
{
    for ( u32 t = 0; t < prof_thread_count(); ++t )
        while ( prof_drain( t, NULL, PROF_RING_CAP ) )
        {
        }
}

/* Drain one ring completely into s_events. */
static u32
sb_drain_thread( u32 thread_index )
{
    return prof_drain( thread_index, s_events, PROF_RING_CAP );
}

/* Walk a drained event batch: verify BEGIN/END events pair up like a well-nested stack
   and that timestamps never go backwards. Returns the deepest nesting level seen, or -1
   on a malformed stream. */
static int
sb_verify_pairing( const prof_event_t* ev, u32 count )
{
    int depth     = 0;
    int max_depth = 0;
    i64 last_tick = 0;

    for ( u32 i = 0; i < count; ++i )
    {
        if ( ev[ i ].tick_ns < last_tick )
            return -1;
        last_tick = ev[ i ].tick_ns;

        if ( ev[ i ].type == PROF_EV_BEGIN )
        {
            depth++;
            if ( depth > max_depth )
                max_depth = depth;
        }
        else if ( ev[ i ].type == PROF_EV_END )
        {
            depth--;
            if ( depth < 0 )
                return -1;
        }
    }
    return depth == 0 ? max_depth : -1;
}

/*==============================================================================================
    Suite: name registry
==============================================================================================*/

static void
sb_test_names( void )
{
    u32 a  = prof_name_register( "sim/update" );
    u32 a2 = prof_name_register( "sim/update" );
    u32 b  = prof_name_register( "render/draw" );

    sb_check( a != 0, "registered id is nonzero" );
    sb_check( a == a2, "same name registers to the same id" );
    sb_check( a != b, "different names get different ids" );
    sb_check( a == prof_hash_str( "sim/update" ), "id IS the name hash" );

    const char* name = prof_name_lookup( a );
    sb_check( name && strcmp( name, "sim/update" ) == 0, "lookup returns the interned string" );
    sb_check( prof_name_lookup( 0xDEADBEEF ) == NULL, "unknown id looks up as NULL" );
    sb_check( prof_name_lookup( 0 ) == NULL, "id 0 looks up as NULL" );
}

/*==============================================================================================
    Suite: zones -- direct calls, nesting, drain
==============================================================================================*/

static void
sb_test_zones( void )
{
    sb_flush_all();

    u32 frame_id  = prof_name_register( "zt/frame" );
    u32 sim_id    = prof_name_register( "zt/sim" );
    u32 physics_id = prof_name_register( "zt/physics" );

    /* frame > sim > physics, then a sibling sim -- 8 events, max depth 3. */
    prof_zone_begin( frame_id );
    prof_zone_begin( sim_id );
    prof_zone_begin( physics_id );
    prof_zone_end();
    prof_zone_end();
    prof_zone_begin( sim_id );
    prof_zone_end();
    prof_zone_end();

    u32 n = sb_drain_thread( 0 );
    sb_check( n == 8, "nested zones emit 8 events" );
    sb_check( sb_verify_pairing( s_events, n ) == 3, "stream is well nested, depth 3" );
    sb_check( s_events[ 0 ].type == PROF_EV_BEGIN && s_events[ 0 ].id == frame_id, "first event opens the frame zone" );
    sb_check( s_events[ 2 ].id == physics_id, "innermost begin carries the physics id" );
    sb_check( s_events[ 3 ].type == PROF_EV_END && s_events[ 3 ].id == 0, "END events carry id 0" );

    sb_check( sb_drain_thread( 0 ) == 0, "ring is empty after a full drain" );
    sb_check( prof_thread_count() >= 1, "main thread claimed a ring" );
}

/*==============================================================================================
    Suite: capture macros
==============================================================================================*/

static void
sb_test_macros( void )
{
    sb_flush_all();

    /* Static-cache macro: three passes must reuse one id. */
    for ( int i = 0; i < 3; ++i )
    {
        PROF_ZONE_BEGIN( "macro/zone" );
        PROF_ZONE_END();
    }

    u32 n = sb_drain_thread( 0 );
    sb_check( n == 6, "3 macro zone pairs emit 6 events" );
    bool same_id = n == 6 && s_events[ 0 ].id == s_events[ 2 ].id && s_events[ 2 ].id == s_events[ 4 ].id;
    sb_check( same_id, "macro static cache reuses one id" );
    sb_check( n == 6 && s_events[ 0 ].id == prof_hash_str( "macro/zone" ), "macro id matches the name hash" );

    /* Scope macro: body runs exactly once, wrapped in one pair. */
    int body_runs = 0;
    PROF_SCOPE( "macro/scope" )
    {
        body_runs++;
    }
    sb_check( body_runs == 1, "PROF_SCOPE runs its body exactly once" );

    n = sb_drain_thread( 0 );
    sb_check( n == 2, "PROF_SCOPE emits one begin/end pair" );
    sb_check( n == 2 && s_events[ 0 ].id == prof_hash_str( "macro/scope" ), "scope zone id matches the name hash" );
}

/*==============================================================================================
    Suite: frame marks
==============================================================================================*/

static void
sb_test_frames( void )
{
    sb_flush_all();

    u64 base = prof_frame_number();
    u64 f1   = prof_frame_mark();
    u64 f2   = prof_frame_mark();

    sb_check( f1 == base + 1 && f2 == base + 2, "frame_mark increments the global frame number" );
    sb_check( prof_frame_number() == f2, "frame_number reads the latest mark" );

    u32 n = sb_drain_thread( 0 );
    sb_check( n == 2, "two frame marks emit two events" );
    sb_check( n == 2 && s_events[ 0 ].type == PROF_EV_FRAME, "frame event carries PROF_EV_FRAME" );
    sb_check( n == 2 && s_events[ 1 ].id == ( u32 )f2, "frame event id is the frame number" );
}

/*==============================================================================================
    Suite: counters
==============================================================================================*/

static void
sb_test_counters( void )
{
    u32 draws_id = prof_name_register( "ct/draw_calls" );
    u32 allocs_id = prof_name_register( "ct/allocs" );

    prof_counter_set( draws_id, 42 );
    prof_counter_add( draws_id, 8 );
    prof_counter_add( allocs_id, 3 );
    prof_counter_set( allocs_id, 7 );

    prof_counter_t snap[ PROF_MAX_COUNTERS ];
    u32            n = prof_counters( snap, PROF_MAX_COUNTERS );
    sb_check( n >= 2, "snapshot returns both counters" );

    i64 draws = -1, allocs = -1;
    for ( u32 i = 0; i < n; ++i )
    {
        if ( snap[ i ].id == draws_id )
            draws = snap[ i ].value;
        if ( snap[ i ].id == allocs_id )
            allocs = snap[ i ].value;
    }
    sb_check( draws == 50, "set 42 + add 8 == 50" );
    sb_check( allocs == 7, "set after add overwrites to 7" );

    /* Macro forms fold onto the same table. */
    PROF_COUNTER_SET( "ct/macro", 5 );
    PROF_COUNTER_ADD( "ct/macro", -2 );
    n = prof_counters( snap, PROF_MAX_COUNTERS );
    i64 macro_val = -1;
    for ( u32 i = 0; i < n; ++i )
        if ( snap[ i ].id == prof_hash_str( "ct/macro" ) )
            macro_val = snap[ i ].value;
    sb_check( macro_val == 3, "counter macros set 5, add -2 == 3" );
}

/*==============================================================================================
    Suite: overflow -- fill the ring past capacity, drops counted, drain chunks recover all
==============================================================================================*/

static void
sb_test_overflow( void )
{
    sb_flush_all();

    u32 id      = prof_name_register( "of/spam" );
    u32 before  = prof_thread_dropped( 0 );
    u32 total   = PROF_RING_CAP + 500;

    for ( u32 i = 0; i < total; ++i )
        prof_zone_begin( id );

    sb_check( prof_thread_dropped( 0 ) - before == 500, "events past capacity are dropped and counted" );

    /* Drain in odd-sized chunks: the sum must be exactly one full ring. */
    u32  drained    = 0;
    u32  chunk;
    bool chunks_ok  = true;
    while ( ( chunk = prof_drain( 0, s_events, 300 ) ) != 0 )
    {
        chunks_ok = chunks_ok && chunk <= 300;
        drained  += chunk;
    }
    sb_check( chunks_ok, "drain honors the max argument" );
    sb_check( drained == PROF_RING_CAP, "chunked drain recovers exactly the ring capacity" );
}

/*==============================================================================================
    Suite: runtime disable
==============================================================================================*/

static void
sb_test_disable( void )
{
    sb_flush_all();

    prof_set_enabled( false );
    sb_check( !prof_is_enabled(), "is_enabled reflects the switch" );

    PROF_ZONE_BEGIN( "dis/zone" );
    PROF_ZONE_END();
    prof_frame_mark();    /* frame numbers still advance; the ring event is suppressed */

    sb_check( sb_drain_thread( 0 ) == 0, "disabled capture emits no events" );

    prof_set_enabled( true );
    PROF_ZONE_BEGIN( "dis/zone" );
    PROF_ZONE_END();
    sb_check( sb_drain_thread( 0 ) == 2, "re-enabled capture emits again" );
}

/*==============================================================================================
    Suite: multi-threaded capture -- one SPSC ring per worker
==============================================================================================*/

#define SB_WORKERS     4
#define SB_WORK_ZONES  1000

typedef struct sb_worker_arg_s
{
    char label[ PROF_THREAD_NAME_MAX ];
    u32  zone_id;
} sb_worker_arg_t;

static void
sb_worker_main( void* arg )
{
    sb_worker_arg_t* wa = ( sb_worker_arg_t* )arg;

    prof_thread_name( wa->label );
    for ( u32 i = 0; i < SB_WORK_ZONES; ++i )
    {
        prof_zone_begin( wa->zone_id );
        prof_zone_end();
    }
}

static void
sb_test_threads( void )
{
    sb_flush_all();

    u32             zone_id = prof_name_register( "mt/work" );
    sb_worker_arg_t args[ SB_WORKERS ];
    thread_t        threads[ SB_WORKERS ];

    for ( int i = 0; i < SB_WORKERS; ++i )
    {
        snprintf( args[ i ].label, sizeof( args[ i ].label ), "worker_%d", i );
        args[ i ].zone_id = zone_id;
        threads[ i ]      = thread_create( sb_worker_main, &args[ i ], 0 );
        sb_check( thread_valid( threads[ i ] ), "worker thread created" );
    }
    for ( int i = 0; i < SB_WORKERS; ++i )
        thread_join( threads[ i ] );

    sb_check( prof_thread_count() >= 1 + SB_WORKERS, "each worker claimed its own ring" );

    /* Find each worker ring by label and validate its private stream. */
    int  rings_found = 0;
    bool counts_ok   = true;
    bool pairing_ok  = true;
    bool no_drops    = true;
    for ( u32 t = 0; t < prof_thread_count(); ++t )
    {
        const char* label = prof_thread_label( t );
        if ( !label || strncmp( label, "worker_", 7 ) != 0 )
            continue;

        rings_found++;
        u32 n      = sb_drain_thread( t );
        counts_ok  = counts_ok && n == 2 * SB_WORK_ZONES;
        pairing_ok = pairing_ok && sb_verify_pairing( s_events, n ) == 1;
        no_drops   = no_drops && prof_thread_dropped( t ) == 0;
    }
    sb_check( rings_found == SB_WORKERS, "all worker rings located by label" );
    sb_check( counts_ok, "each worker ring holds exactly its own events" );
    sb_check( pairing_ok, "worker streams are well paired, depth 1" );
    sb_check( no_drops, "worker rings dropped nothing" );
}

/*==============================================================================================
    Suite: wall-clock sanity -- a zone around a real sleep measures a real duration
==============================================================================================*/

static void
sb_test_timing( void )
{
    sb_flush_all();

    prof_zone_begin( prof_name_register( "tm/sleep" ) );
    sys_sleep_milliseconds( 5 );
    prof_zone_end();

    u32 n = sb_drain_thread( 0 );
    sb_check( n == 2, "sleep zone emits one pair" );
    if ( n == 2 )
    {
        i64 dur_ns = s_events[ 1 ].tick_ns - s_events[ 0 ].tick_ns;
        sb_check( dur_ns >= 4 * 1000 * 1000, "5 ms sleep zone measures >= 4 ms" );
        sb_check( dur_ns < 1000 * 1000 * 1000, "5 ms sleep zone measures < 1 s" );
    }
}

/*==============================================================================================
    Hot-path cost readout (informational, not a check)

    Emits batches that fit the ring, discards between batches, reports the best batch.
==============================================================================================*/

static void
sb_report_cost( void )
{
    u32 id    = prof_name_register( "perf/pair" );
    u32 pairs = PROF_RING_CAP / 2 - 64;
    i64 best  = 0;

    for ( int batch = 0; batch < 10; ++batch )
    {
        sb_flush_all();
        i64 t0 = sys_tick_nanoseconds();
        for ( u32 i = 0; i < pairs; ++i )
        {
            prof_zone_begin( id );
            prof_zone_end();
        }
        i64 elapsed = sys_tick_nanoseconds() - t0;
        if ( best == 0 || elapsed < best )
            best = elapsed;
    }
    sb_flush_all();

    printf( "    hot path: %.1f ns per begin/end pair (%u pairs, best of 10)\n",
            ( f64 )best / ( f64 )pairs, pairs );
}

/*==============================================================================================
   main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    sys_tick_init();
    prof_init();

    printf( "========================================\n" );
    printf( " name registry\n" );
    printf( "========================================\n" );
    sb_test_names();

    printf( "\n========================================\n" );
    printf( " zones + drain + nesting\n" );
    printf( "========================================\n" );
    sb_test_zones();
    sb_test_macros();

    printf( "\n========================================\n" );
    printf( " frame marks + counters\n" );
    printf( "========================================\n" );
    sb_test_frames();
    sb_test_counters();

    printf( "\n========================================\n" );
    printf( " overflow + disable\n" );
    printf( "========================================\n" );
    sb_test_overflow();
    sb_test_disable();

    printf( "\n========================================\n" );
    printf( " multi-threaded capture\n" );
    printf( "========================================\n" );
    sb_test_threads();

    printf( "\n========================================\n" );
    printf( " timing sanity + hot-path cost\n" );
    printf( "========================================\n" );
    sb_test_timing();
    sb_report_cost();

    prof_exit();
    sys_tick_exit();

    printf( "\n%d checks, %d failures\n", s_checks, s_fails );
    printf( s_fails == 0 ? "ALL PASS\n" : "FAILED\n" );
    return s_fails == 0 ? 0 : 1;
}

/*============================================================================================*/
