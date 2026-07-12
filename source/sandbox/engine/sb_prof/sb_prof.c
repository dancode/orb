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

/* INFO: How you test a lock-free profiler deterministically.

   Concurrency bugs are timing-dependent, so every suite is built to make its outcome
   exact rather than probabilistic:
     - Effects are verified through the full API round trip: emit, then drain, then assert
       on the drained bytes -- the same path a real consumer (overlay, dump) uses.
     - Every suite starts from drained-empty rings (sb_flush_all), so counts are absolute
       numbers, not "at least N".
     - Threaded suites assert only on states guaranteed after thread_join: join is a full
       synchronization point, so everything a worker wrote is visible after it returns.
     - Invariants are chosen so ANY legal interleaving must satisfy them (exact totals,
       well-formed pairing, zero drops) -- never assertions about timing.
   The cost readouts at the end are informational, not checks: perf numbers vary machine
   to machine and would make the suite flaky as assertions.                                */

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

/* INFO: This tiny walk is a working model of what every profiler viewer does: treat BEGIN
   as push and END as pop; the stream is valid iff the stack never underflows and ends
   empty. The depth at any point is the flame-graph nesting level, and END.tick - BEGIN.tick
   at matching depth is a zone's duration. Understand this loop and you understand how a
   timeline UI is derived from the raw ring bytes.                                         */

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

/* INFO: Identity is the contract everything else leans on: registering twice must yield
   the same id (call sites cache it in statics), the id must equal the raw hash (so ids
   survive restarts and hot-reloads, and can be precomputed offline), and lookup must
   return the interned string (viewers and dumps translate ids back to labels at the very
   end of the pipeline). In practice this is what lets you diff captures from different
   runs by zone id.                                                                        */

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

/* INFO: The core round trip: emit a known nested shape (frame > sim > physics, plus a
   sibling), drain it, and assert the stream replays exactly that shape. The check that
   END events carry id 0 pins down a deliberate design choice: closes are anonymous
   because pairing is positional -- which is why PROF_ZONE_END needs no argument and no
   per-site static, halving what the hot path has to know.                                 */

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

/* INFO: Verifies the macro machinery rather than the profiler core: the per-call-site
   static must mean one registration then reuse (all three loop passes carry the same id),
   and PROF_SCOPE must run its body EXACTLY once, because it secretly wraps the block in a
   for loop. A bug here would silently double-execute user code -- far worse than a wrong
   timing number, which is why body_runs is counted explicitly.                            */

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

/* INFO: Frame marks are the axis every per-frame view hangs off. Two halves are pinned:
   the RETURNED number is globally monotonic (fetch-add semantics -- no two marks can get
   the same number), and the RING event carries that number, which is how a consumer lines
   zones from many threads up against one shared frame timeline.                           */

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

/* INFO: Exercises last-write-wins (set) versus accumulate (add) on the shared slot table,
   including slot creation on first touch and the macro forms landing on the same slots as
   direct calls (the id is the same hash either way). In regular practice counters carry
   the per-frame quantities you would otherwise printf: draw calls, entity counts, bytes
   in flight -- set once per frame, or add from many sites and read the total.             */

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

/* INFO: Deliberately overflows one ring to prove the failure mode is exact accounting, not
   corruption: capacity + 500 writes must produce exactly 500 counted drops, and a chunked
   drain must recover exactly capacity events. The odd chunk size (300 into an 8192 ring)
   forces the drain's wrap-around two-memcpy path to actually execute. In practice the
   dropped counter is the operator's alarm that the ring is too small or the consumer is
   not keeping up -- data loss you can SEE beats data loss you cannot.                     */

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

/* INFO: The runtime gate must be airtight in both directions: disabled means zero ring
   traffic (this is what lets an instrumented dev build idle at one branch per macro), and
   re-enabling must work with no further ceremony. A toggle in the middle of an open zone
   can orphan a BEGIN or an END -- by contract the CONSUMER tolerates that, rather than the
   hot path paying to prevent it; sb_verify_pairing's tolerance mirrors the same rule.     */

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

/* INFO: The SPSC isolation proof. Four workers hammer zones concurrently; if storage were
   shared this would be a data race, but each worker's stream must come back in its own
   ring: exactly 2 * SB_WORK_ZONES events, perfectly paired at depth 1, zero drops. The
   thread_join before the asserts is what makes this deterministic -- join is a full
   synchronization point, so every worker write is visible when the checks run. This is
   the suite that would catch a broken publish order: a torn or reordered event shows up
   as a pairing or count failure.                                                          */

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

/* INFO: Ring slots are a fixed pool of 16, so transient threads (loaders, one-shot jobs)
   would exhaust the pool permanently without a release path. This suite walks the whole
   lifecycle and pins the one dangerous transition: releasing with PENDING events must
   RETIRE the slot, not free it -- proven by rc_x NOT reusing rc_a's slot -- because
   handing an undrained ring to a new thread would splice two histories into one stream.
   The emptying drain frees; reuse keeps thread_count stable; an empty release skips
   RETIRED entirely. Practical rule: fixed worker pools never bother releasing, short-
   lived threads call PROF_THREAD_RELEASE on the way out.                                  */

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

/* INFO: Treats the trace file as a black-box artifact: capture a little of everything
   (a frame mark, a macro zone pair, a counter), then assert the JSON grammar a viewer
   depends on actually appears -- B/E phases with the zone name, the frame instant, the
   counter sample, thread metadata, and a properly closed array. This is byte-for-byte the
   same file the host's `prof_dump` console command produces; drop one onto
   https://ui.perfetto.dev to see the timeline these tests are checking in text form.      */

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
    Suite: memory hooks -- exact accounting, CAS-max peak under contention, dump sampling
==============================================================================================*/

/* INFO: How the hooks are meant to be used (nothing in the engine calls them yet): an
   allocator wraps its own alloc/free with PROF_MEM_ALLOC/FREE and a scope name -- e.g. an
   arena reporting "arena/gui" -- and from then on live bytes, high-water peak, and churn
   counts are exact without anyone polling. The threaded check below is the reason the
   table exists at all: every worker's allocations are freed by join time, so a SAMPLED
   value would read zero and show nothing ever happened -- while the hook-maintained peak
   and churn counts still hold the true story of what the memory did in between.           */

typedef struct sb_mem_arg_s
{
    u32 scope_id;
    u32 iterations;
} sb_mem_arg_t;

static void
sb_mem_worker( void* arg )
{
    sb_mem_arg_t* ma = ( sb_mem_arg_t* )arg;
    for ( u32 i = 0; i < ma->iterations; ++i )
    {
        prof_mem_alloc( ma->scope_id, 16 );
        prof_mem_free( ma->scope_id, 16 );
    }
}

/* Snapshot all scopes and copy out the one matching id. */
static bool
sb_mem_find( u32 id, prof_mem_t* out )
{
    prof_mem_t snap[ PROF_MAX_MEM_SCOPES ];
    u32        n = prof_mem_stats( snap, PROF_MAX_MEM_SCOPES );
    for ( u32 i = 0; i < n; ++i )
    {
        if ( snap[ i ].id == id )
        {
            *out = snap[ i ];
            return true;
        }
    }
    return false;
}

static void
sb_test_mem( void )
{
#if ORB_PROFILE_MEM
    prof_mem_t m;

    /* Exact single-thread accounting: current follows the balance, peak holds the max. */
    u32 pool = prof_name_register( "mem/pool" );
    prof_mem_alloc( pool, 100 );
    prof_mem_alloc( pool, 50 );
    prof_mem_free( pool, 100 );
    prof_mem_alloc( pool, 25 );

    sb_check( sb_mem_find( pool, &m ), "scope appears in the stats snapshot" );
    sb_check( m.current == 75, "current tracks the alloc/free balance" );
    sb_check( m.peak == 150, "peak holds the high-water mark after frees" );
    sb_check( m.allocs == 3 && m.frees == 1, "alloc/free events are counted" );

    /* Macro form: same table through the gateway, per-site cached id. */
    PROF_MEM_ALLOC( "mem/macro", 64 );
    PROF_MEM_FREE( "mem/macro", 64 );
    sb_check( sb_mem_find( prof_name_register( "mem/macro" ), &m ), "macro form reaches the table" );
    sb_check( m.current == 0 && m.peak == 64, "macro pair balances; peak survives" );
    sb_check( m.allocs == 1 && m.frees == 1, "macro pair counted once" );

    /* Unbalanced hooks are reported truthfully, never clamped. */
    u32 stray = prof_name_register( "mem/stray" );
    prof_mem_free( stray, 10 );
    sb_check( sb_mem_find( stray, &m ) && m.current == -10, "unbalanced free goes negative, not clamped" );

    /* Contention: totals are exact under any interleaving, and by join time everything
       is freed -- exactly the history a sampled counter would erase. Peak is bounded by
       the max possible concurrent live bytes (one 16-byte alloc per worker). */
    enum { SB_MEM_WORKERS = 4, SB_MEM_ITERS = 4000 };
    u32          shared = prof_name_register( "mem/shared" );
    sb_mem_arg_t args[ SB_MEM_WORKERS ];
    thread_t     threads[ SB_MEM_WORKERS ];

    for ( int i = 0; i < SB_MEM_WORKERS; ++i )
    {
        args[ i ].scope_id   = shared;
        args[ i ].iterations = SB_MEM_ITERS;
        threads[ i ]         = thread_create( sb_mem_worker, &args[ i ], 0 );
        sb_check( thread_valid( threads[ i ] ), "mem worker created" );
    }
    for ( int i = 0; i < SB_MEM_WORKERS; ++i )
        thread_join( threads[ i ] );

    sb_check( sb_mem_find( shared, &m ), "shared scope present after workers" );
    sb_check( m.current == 0, "contended alloc/free balance is exact (fetch-add)" );
    sb_check( m.allocs == SB_MEM_WORKERS * SB_MEM_ITERS, "no alloc event lost under contention" );
    sb_check( m.frees == SB_MEM_WORKERS * SB_MEM_ITERS, "no free event lost under contention" );
    sb_check( m.peak >= 16 && m.peak <= SB_MEM_WORKERS * 16, "peak within the possible live range" );

    /* Dump sampling: scopes land as "used"/"peak" counter tracks in the trace. */
    sb_flush_all();
    const char* path = "sb_prof_mem.json";
    sb_check( prof_dump_begin( path ), "mem dump opens" );
    prof_dump_flush();
    prof_dump_end();

    static char buf[ 64 * 1024 ];
    FILE*       f = fopen( path, "rb" );
    sb_check( f != NULL, "mem trace file exists" );
    if ( f )
    {
        size_t len = fread( buf, 1, sizeof( buf ) - 1, f );
        fclose( f );
        buf[ len ] = 0;

        sb_check( strstr( buf, "mem/pool" ) != NULL, "scope sampled into the trace" );
        sb_check( strstr( buf, "\"used\":75" ) != NULL, "live bytes sampled as the used series" );
        sb_check( strstr( buf, "\"peak\":150" ) != NULL, "high-water sampled as the peak series" );
    }
    remove( path );
#else
    printf( "    (ORB_PROFILE_MEM disabled -- suite skipped)\n" );
#endif
}

/*==============================================================================================
    Suite: hitch capture -- rolling history, threshold trigger, cooldown, session reset
==============================================================================================*/

/* INFO: Hitch capture in practice: a shipped dev build arms the monitor once at boot
   ("prof_hitch 33.3" on the host console) and then nobody thinks about it -- when a frame
   stutters, a trace of the moments BEFORE and DURING the stutter appears on disk by
   itself, ready for perfetto. This suite plays both roles by hand: it is the "host loop"
   (calling update once per simulated frame with a made-up frame time) and the "spike"
   (passing a big frame_ms), so every trigger rule -- threshold, cooldown, dump priority,
   re-arm reset -- is checked deterministically without real stalls or real timing.        */

static void
sb_test_hitch( void )
{
#if ORB_PROFILE_HITCH
    sb_flush_all();

    /* Disarmed monitor is inert regardless of the frame time reported. */
    sb_check( !prof_hitch_armed(), "monitor starts disarmed" );
    sb_check( prof_hitch_update( 100.0 ) == 0, "disarmed update captures nothing" );
    sb_check( prof_hitch_count() == 0, "no captures while disarmed" );
    sb_check( prof_hitch_last_path() == NULL, "no capture path while disarmed" );

    /* Armed, under threshold: history accumulates, nothing fires. */
    prof_hitch_arm( 10.0, "sb_prof_hitch" );
    sb_check( prof_hitch_armed(), "monitor reports armed" );

    u32 fired = 0;
    for ( int f = 0; f < 3; ++f )
    {
        PROF_ZONE_BEGIN( "hitch/work" );
        PROF_ZONE_END();
        prof_frame_mark();
        fired += prof_hitch_update( 1.0 );
    }
    sb_check( fired == 0, "under-threshold frames never capture" );
    sb_check( prof_hitch_count() == 0, "capture count stays zero under threshold" );

    /* Over-threshold frame: the capture fires with the whole buffered history. */
    PROF_ZONE_BEGIN( "hitch/spike" );
    PROF_ZONE_END();
    prof_frame_mark();
    u32 written = prof_hitch_update( 50.0 );
    sb_check( written >= 12, "capture writes the buffered history (4 frames of events)" );
    sb_check( prof_hitch_count() == 1, "capture count increments" );

    char path[ 160 ] = { 0 };
    sb_check( prof_hitch_last_path() != NULL, "capture path is published" );
    if ( prof_hitch_last_path() )
        snprintf( path, sizeof( path ), "%s", prof_hitch_last_path() );
    sb_check( strstr( path, "sb_prof_hitch_" ) == path, "capture path uses the prefix" );

    /* Read back: pre-hitch history AND the hitch frame itself are in one trace. */
    static char buf[ 64 * 1024 ];
    FILE*       f = fopen( path, "rb" );
    sb_check( f != NULL, "hitch trace file exists" );
    if ( f )
    {
        size_t len = fread( buf, 1, sizeof( buf ) - 1, f );
        fclose( f );
        buf[ len ] = 0;

        sb_check( strstr( buf, "{\"traceEvents\":[" ) == buf, "hitch trace opens the traceEvents array" );
        sb_check( strstr( buf, "hitch/work" ) != NULL, "pre-hitch history survives into the trace" );
        sb_check( strstr( buf, "hitch/spike" ) != NULL, "the hitch frame itself is in the trace" );
        sb_check( strstr( buf, "]}" ) != NULL, "hitch trace closes cleanly" );
    }
    remove( path );

    /* Cooldown: an immediate second spike is suppressed (no file spam on a long stall). */
    PROF_ZONE_BEGIN( "hitch/work" );
    PROF_ZONE_END();
    prof_frame_mark();
    sb_check( prof_hitch_update( 50.0 ) == 0, "cooldown suppresses back-to-back captures" );
    sb_check( prof_hitch_count() == 1, "capture count unchanged during cooldown" );

    /* An explicit dump owns the drain: the armed monitor stands down entirely. */
    const char* dump_path = "sb_prof_hitch_dump.json";
    sb_check( prof_dump_begin( dump_path ), "explicit dump opens while armed" );
    sb_check( prof_hitch_update( 50.0 ) == 0, "monitor is inert while a dump is active" );
    prof_dump_end();
    remove( dump_path );

    /* Re-arm starts a fresh session: cooldown, count, and path all reset. */
    prof_hitch_arm( 10.0, "sb_prof_hitch2" );
    sb_check( prof_hitch_count() == 0, "re-arm resets the capture count" );
    sb_check( prof_hitch_last_path() == NULL, "re-arm clears the last path" );

    /* History wrap: buffer far more than PROF_HITCH_HISTORY_CAP events across frames --
       the capture must hold exactly the most recent CAP of them (plus the live-flush
       counter samples), proving the circular overwrite keeps the newest history. */
    for ( int frame = 0; frame < 3; ++frame )
    {
        for ( u32 i = 0; i < 3000; ++i )
        {
            PROF_ZONE_BEGIN( "hitch/wrap" );
            PROF_ZONE_END();
        }
        sb_check( prof_hitch_update( 1.0 ) == 0, "wrap-fill frame stays under threshold" );
    }
    written = prof_hitch_update( 50.0 );
    sb_check( written >= PROF_HITCH_HISTORY_CAP, "wrapped capture writes a full history window" );
    sb_check( written <= PROF_HITCH_HISTORY_CAP + PROF_MAX_COUNTERS + PROF_MAX_MEM_SCOPES,
              "wrapped capture writes at most one window" );
    if ( prof_hitch_last_path() )
        remove( prof_hitch_last_path() );

    /* Threshold <= 0 disarms. */
    prof_hitch_arm( 0.0, NULL );
    sb_check( !prof_hitch_armed(), "threshold <= 0 disarms" );
    sb_check( prof_hitch_update( 100.0 ) == 0, "disarmed again after the session" );
#else
    printf( "    (ORB_PROFILE_HITCH disabled -- suite skipped)\n" );
#endif
}

/*==============================================================================================
    Suite: wall-clock sanity -- a zone around a real sleep measures a real duration
==============================================================================================*/

/* INFO: Anchors tick_ns to reality: a zone wrapped around a real 5 ms sleep must measure
   roughly 5 ms. Every other suite checks STRUCTURE and would pass with a broken clock
   source or a ns/us unit mixup; only a wall-clock comparison catches those. The loose
   bounds (>= 4 ms, < 1 s) absorb OS sleep jitter without making the check flaky.          */

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

/* INFO: Microbenchmark hygiene in miniature: batches sized to FIT the ring so drops never
   distort the loop, a flush between batches so every run starts from empty cursors, and
   best-of-10 instead of an average -- the minimum is the cleanest estimate of intrinsic
   cost, while averages absorb scheduler noise, interrupts, and cache-cold first passes.
   Expect the QPC timestamp read to dominate both flavors; that floor is the number that
   tells you how densely you can afford to instrument.                                     */

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
    printf( " memory hooks\n" );
    printf( "========================================\n" );
    sb_test_mem();

    printf( "\n========================================\n" );
    printf( " hitch capture\n" );
    printf( "========================================\n" );
    sb_test_hitch();

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
