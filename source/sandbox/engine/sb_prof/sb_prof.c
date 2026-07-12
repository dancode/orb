/*==============================================================================================

    sandbox/engine/sb_prof/sb_prof.c -- Test sandbox for the engine/prof profiler library.

    Exercises the whole starter surface: name registry, zone capture + drain + nesting
    reconstruction, capture macros, frame marks, counters, ring overflow, runtime disable,
    multi-threaded capture into per-thread rings, ring release/recycle, the Chrome-trace
    dump, and a wall-clock sanity zone. Ends with informational hot-path cost readouts
    (ns per begin/end pair; direct calls and the macro form).

    Built with ORB_PROFILE_FAST so the ZONE macros run the inline TLS write path here --
    the direct prof_zone_* calls throughout still cover the classic path, so one binary
    exercises both. Flip the define to 0 to run the whole suite against the vtable macros.

==============================================================================================*/
#define ORB_PROFILE_FAST 1

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
    Suite: ring release + recycle -- PROF_RING_RECYCLE

    Workers claim a ring, emit, and PROF_THREAD_RELEASE before exiting. A release with
    pending events RETIRES the slot (drainable, not reusable) until the drain that empties
    it frees it; an empty release frees immediately. Reuse keeps thread_count stable.
==============================================================================================*/

typedef struct sb_release_arg_s
{
    char label[ PROF_THREAD_NAME_MAX ];
    u32  zone_id;
    u32  pairs;
} sb_release_arg_t;

static void
sb_release_worker( void* arg )
{
    sb_release_arg_t* ra = ( sb_release_arg_t* )arg;

    prof_thread_name( ra->label );
    for ( u32 i = 0; i < ra->pairs; ++i )
    {
        prof_zone_begin( ra->zone_id );
        prof_zone_end();
    }
    PROF_THREAD_RELEASE();
}

/* Spawn one release worker and join it -- everything after the join is deterministic. */
static void
sb_spawn_release_worker( const char* label, u32 zone_id, u32 pairs )
{
    sb_release_arg_t ra;
    snprintf( ra.label, sizeof( ra.label ), "%s", label );
    ra.zone_id = zone_id;
    ra.pairs   = pairs;

    thread_t th = thread_create( sb_release_worker, &ra, 0 );
    sb_check( thread_valid( th ), "release worker created" );
    thread_join( th );
}

static int
sb_find_ring( const char* label )
{
    for ( u32 t = 0; t < prof_thread_count(); ++t )
    {
        const char* l = prof_thread_label( t );
        if ( l && strcmp( l, label ) == 0 )
            return ( int )t;
    }
    return -1;
}

static void
sb_test_recycle( void )
{
#if PROF_RING_RECYCLE
    sb_flush_all();

    u32 zone_id = prof_name_register( "rc/work" );
    u32 base    = prof_thread_count();

    /* Release with pending events: the slot RETIRES -- still drainable, not reusable. */
    sb_spawn_release_worker( "rc_a", zone_id, 3 );
    sb_check( prof_thread_count() == base + 1, "first release worker claimed one new slot" );

    sb_spawn_release_worker( "rc_x", zone_id, 1 );
    sb_check( prof_thread_count() == base + 2, "retired slot with pending events is not reused" );

    /* The drain that empties a retired ring frees its slot. */
    int ta = sb_find_ring( "rc_a" );
    sb_check( ta >= 0, "retired ring still enumerable by label" );
    if ( ta >= 0 )
    {
        u32 n = sb_drain_thread( ( u32 )ta );
        sb_check( n == 6, "retired ring drains its pending events" );
        sb_check( sb_verify_pairing( s_events, n ) == 1, "retired stream is well paired" );
    }

    sb_spawn_release_worker( "rc_b", zone_id, 2 );
    sb_check( prof_thread_count() == base + 2, "freed slot is reused (thread count stable)" );
    int tb = sb_find_ring( "rc_b" );
    sb_check( tb == ta, "reuse lands on the freed slot" );
    if ( tb >= 0 )
        sb_check( sb_drain_thread( ( u32 )tb ) == 4, "reused ring carries only the new owner's events" );

    /* An empty release frees immediately -- the next claim reuses it with no drain. */
    sb_spawn_release_worker( "rc_c", zone_id, 0 );
    sb_spawn_release_worker( "rc_d", zone_id, 1 );
    sb_check( prof_thread_count() == base + 2, "empty release frees without a drain" );
    sb_check( sb_find_ring( "rc_d" ) == ta, "empty-released slot reused in place" );

    sb_flush_all();    /* empties + frees the remaining retired slots (rc_x, rc_d) */
#else
    printf( "    (PROF_RING_RECYCLE disabled -- suite skipped)\n" );
#endif
}

/*==============================================================================================
    Suite: Chrome-trace dump -- write, spot-check the trace grammar, clean up
==============================================================================================*/

static void
sb_test_dump( void )
{
    sb_flush_all();

    const char* path = "sb_prof_trace.json";

    sb_check( !prof_dump_active(), "dump starts inactive" );
    sb_check( prof_dump_begin( path ), "dump_begin opens the trace file" );
    sb_check( prof_dump_active(), "dump reports active" );
    sb_check( !prof_dump_begin( path ), "second dump_begin refuses while active" );

    prof_frame_mark();
    PROF_ZONE_BEGIN( "dump/zone" );
    PROF_ZONE_END();
    prof_counter_set( prof_name_register( "dump/counter" ), 99 );

    u32 written = prof_dump_flush();
    sb_check( written >= 3, "flush writes the frame mark, the pair, and counters" );

    prof_dump_end();
    sb_check( !prof_dump_active(), "dump inactive after end" );

    /* Read the file back and spot-check the trace grammar. */
    static char buf[ 64 * 1024 ];
    FILE*       f = fopen( path, "rb" );
    sb_check( f != NULL, "trace file exists" );
    if ( f )
    {
        size_t len = fread( buf, 1, sizeof( buf ) - 1, f );
        fclose( f );
        buf[ len ] = 0;

        sb_check( strstr( buf, "{\"traceEvents\":[" ) == buf, "trace opens with the traceEvents array" );
        sb_check( strstr( buf, "\"ph\":\"B\"" ) && strstr( buf, "dump/zone" ), "zone open recorded with its name" );
        sb_check( strstr( buf, "\"ph\":\"E\"" ) != NULL, "zone close recorded" );
        sb_check( strstr( buf, "\"ph\":\"i\"" ) && strstr( buf, "\"name\":\"frame\"" ), "frame mark recorded as a global instant" );
        sb_check( strstr( buf, "\"ph\":\"C\"" ) && strstr( buf, "dump/counter" ), "counter recorded as a C sample" );
        sb_check( strstr( buf, "\"ph\":\"M\"" ) && strstr( buf, "thread_name" ), "thread metadata recorded" );
        sb_check( strstr( buf, "]}" ) != NULL, "trace closes the array and object" );
    }
    remove( path );
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
    Hot-path cost readouts (informational, not checks)

    Emits batches that fit the ring, discards between batches, reports the best batch.
    Two flavors: the direct call (the classic path) and the ZONE macro form, which is the
    inline TLS write here (ORB_PROFILE_FAST) or the gateway call when built without it.
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

    i64 best_macro = 0;
    for ( int batch = 0; batch < 10; ++batch )
    {
        sb_flush_all();
        i64 t0 = sys_tick_nanoseconds();
        for ( u32 i = 0; i < pairs; ++i )
        {
            PROF_ZONE_BEGIN( "perf/pair" );
            PROF_ZONE_END();
        }
        i64 elapsed = sys_tick_nanoseconds() - t0;
        if ( best_macro == 0 || elapsed < best_macro )
            best_macro = elapsed;
    }
    sb_flush_all();

#if ORB_PROFILE_FAST
    const char* macro_kind = "fast TLS inline";
#else
    const char* macro_kind = "vtable";
#endif
    printf( "    hot path: %.1f ns per direct-call pair (%u pairs, best of 10)\n",
            ( f64 )best / ( f64 )pairs, pairs );
    printf( "    hot path: %.1f ns per macro pair, %s\n",
            ( f64 )best_macro / ( f64 )pairs, macro_kind );
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
    printf( " ring release + recycle\n" );
    printf( "========================================\n" );
    sb_test_recycle();

    printf( "\n========================================\n" );
    printf( " chrome-trace dump\n" );
    printf( "========================================\n" );
    sb_test_dump();

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
