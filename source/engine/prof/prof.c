/*==============================================================================================

    engine/prof/prof.c - Profiler implementation: per-thread SPSC event rings, the name
    registry, counters, and the drain surface.

    Hot-path contract: one enabled-flag check, one TLS load, one timestamp, one 16-byte
    store, one publishing cursor write. Everything else (name interning, ring claiming,
    counter slot creation) happens on cold paths guarded by a mutex.

    Threading model:
      - Each ring is SPSC: the owning thread writes, the single drain consumer reads.
        The writer publishes with an atomic (barriered) write_pos store after filling the
        slot; the consumer reads write_pos with a barriered load, copies, then publishes
        read_pos. Cursors are free-running modular u32 counters (w - r is the pending
        count, correct across wrap).
      - A stale read_pos on the writer side only makes overflow drop early -- never
        corrupts. Drops are counted, never blocked on.
      - Rings are claimed one per thread on first emit. With PROF_RING_RECYCLE, claims go
        through a CAS on the slot's owner word and prof_thread_release returns the slot:
        immediately when empty, else RETIRED until the drain consumer empties it (an
        undrained retired slot can also be adopted directly once empty). Without recycle,
        slots are claimed forever by a bump allocator. Threads past PROF_MAX_THREADS share
        a discard ring that only counts drops.
      - The ring struct and the TLS ring pointer are exported (prof.h / prof_api.h) for
        the ORB_PROFILE_FAST inline write path in statically-linked consumers; the
        publish ordering there mirrors prof_ring_push exactly.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "engine/sys/sys_host.h"
#include "engine/prof/prof_host.h"

// clang-format off

_Static_assert( sizeof( prof_event_t ) == 16,                        "prof event must stay 16 bytes" );
_Static_assert( ( PROF_RING_CAP & ( PROF_RING_CAP - 1 ) ) == 0,      "ring cap must be a power of two" );
_Static_assert( ( PROF_NAME_TABLE_SIZE & ( PROF_NAME_TABLE_SIZE - 1 ) ) == 0, "name table must be a power of two" );
_Static_assert( PROF_NAME_TABLE_SIZE >= 2 * PROF_MAX_NAMES,          "name table needs <= 0.5 load factor" );

/*==============================================================================================
    Storage

    prof_ring_t itself lives in prof.h (exported for the ORB_PROFILE_FAST inline path).
==============================================================================================*/

/* Name registry slot: id published LAST (atomic write) so lock-free readers that see a
   nonzero id are guaranteed to see the offset it guards. Slots are never removed. */
typedef struct prof_name_slot_s
{
    volatile i32 id;                            // zone/counter name hash; 0 = empty
    u32          offset;                        // byte offset into the name pool

} prof_name_slot_t;

typedef struct prof_counter_slot_s
{
    volatile i32 id;                            // counter name hash; 0 = empty; inserts are contiguous
    volatile i64 value;

} prof_counter_slot_t;

static prof_ring_t         g_prof_rings[ PROF_MAX_THREADS ];
static prof_ring_t         g_prof_discard_ring = { .discard = true, .label = "overflow" };

static volatile i32        g_prof_ring_count;   // recycle: high-water slot count; legacy: claims (may overshoot MAX)
volatile i32               g_prof_enabled = 1;  // runtime capture switch (extern: fast inline path reads it)
static volatile i64        g_prof_frame;        // global frame counter

static prof_name_slot_t    g_prof_names[ PROF_NAME_TABLE_SIZE ];
static char                g_prof_name_pool[ PROF_NAME_POOL_SIZE ];
static u32                 g_prof_name_pool_top;
static u32                 g_prof_name_count;

static prof_counter_slot_t g_prof_counters[ PROF_MAX_COUNTERS ];

static mutex_t             g_prof_lock;         // guards all cold-path inserts
static volatile i32        g_prof_boot;         // 0 = cold, 1 = initializing, 2 = ready

// this thread's claimed ring (extern: the fast inline path caches through it)
ORB_THREAD_LOCAL prof_ring_t* g_prof_tls_ring;

/* INFO: Thread-local storage is the whole trick.

   "Which ring do I write to?" must be answered in about a nanosecond, with no lock and no
   search, from any thread -- including ones the profiler has never seen. ORB_THREAD_LOCAL
   gives every thread its own private copy of this pointer: NULL until that thread's first
   event (the claim happens lazily right there), then a plain load forever after. No
   thread ever reads another thread's copy, so the variable itself needs no synchronization
   at all -- all cross-thread traffic goes through the ring it points at. This is the
   standard pattern in every instrumenting profiler (Tracy and Optick do exactly this).    */

/*==============================================================================================
    Lifecycle
==============================================================================================*/

/* INFO: Lazy boot via a tiny state machine.

   The profiler cannot demand "call prof_init before anything else" -- the first event may
   arrive from any thread at any time; that is the point of a leaf library. So boot happens
   on first touch, guarded by one atomic state: 0 = cold, 1 = someone is booting, 2 = ready.
   compare_exchange( 0 -> 1 ) elects exactly one booting thread even if ten race here at
   once: CAS is atomic, so exactly one caller sees the old value 0 (the "I won" signal);
   the rest see 1 or 2 and spin-yield for the few nanoseconds boot takes. This is the C
   equivalent of a C++ magic static / std::call_once.                                      */

/* Lazy one-time boot: first thread through the CAS creates the mutex, everyone else spins
   the handful of cycles until it is published. Called only on cold paths. */
static void
prof_ensure_init( void )
{
    if ( sys_atomic_read( &g_prof_boot ) == 2 )
        return;

    if ( sys_atomic_compare_exchange( &g_prof_boot, 1, 0 ) == 0 )
    {
        mutex_init( &g_prof_lock );
        sys_atomic_write( &g_prof_boot, 2 );
    }
    else
    {
        while ( sys_atomic_read( &g_prof_boot ) != 2 )
            thread_yield();
    }
}

void
prof_init( void )
{
    prof_ensure_init();
}

/* Full teardown for shutdown / test re-init. Caller must guarantee no other thread is
   still emitting: other threads' cached TLS ring pointers are not reachable from here,
   so a concurrent writer would land in a ring the next session hands to someone else. */
void
prof_exit( void )
{
    prof_dump_end();    /* closes + finalizes an abandoned capture; no-op when idle */

    if ( sys_atomic_read( &g_prof_boot ) == 2 )
        mutex_destroy( &g_prof_lock );

    memset( g_prof_rings,    0, sizeof( g_prof_rings ) );
    memset( g_prof_names,    0, sizeof( g_prof_names ) );
    memset( g_prof_counters, 0, sizeof( g_prof_counters ) );
    memset( g_prof_name_pool, 0, sizeof( g_prof_name_pool ) );

    g_prof_name_pool_top = 0;
    g_prof_name_count    = 0;
    g_prof_ring_count    = 0;
    g_prof_frame         = 0;
    g_prof_enabled       = 1;
    g_prof_tls_ring      = NULL;

    sys_atomic_write( &g_prof_boot, 0 );
}

/*==============================================================================================
    Rings
==============================================================================================*/

/* INFO: Claiming a ring with CAS instead of a lock.

   Several threads can arrive here at once, each needing a distinct slot. The claim is
   compare_exchange( owner: FREE -> OWNED ): if two threads target the same slot, the
   hardware guarantees exactly one sees FREE as the return value (it won) and the other
   sees OWNED and moves on to the next slot. The CAS itself is the arbitration -- no lock
   needed. This runs once per thread LIFETIME, so scanning 16 slots is performance-
   irrelevant; on cold paths simplicity beats cleverness every time.

   The high-water CAS-max loop below the claim handles a second classic race: threads
   claiming slots 3 and 5 concurrently both try to raise ring_count. The loop re-reads and
   retries until the count is at least its own idx+1, so a smaller concurrent value can
   never overwrite a larger one -- which a plain store could do.                           */

static prof_ring_t*
prof_ring_acquire( void )
{
    prof_ensure_init();

    prof_ring_t* r = NULL;

#if PROF_RING_RECYCLE

    /* Pass 1: claim the lowest free slot (never used, or returned to the pool). The CAS
       is the claim -- racing threads land on distinct slots. */
    for ( i32 i = 0; i < PROF_MAX_THREADS && !r; ++i )
    {
        if ( sys_atomic_compare_exchange( &g_prof_rings[ i ].owner,
                                          PROF_RING_OWNED, PROF_RING_FREE ) == PROF_RING_FREE )
            r = &g_prof_rings[ i ];
    }

    /* Pass 2: adopt a retired slot that is already fully drained -- covers hosts that
       never run a drain consumer, so releases still recycle once rings empty out. The
       cursors cannot move under us: a retired ring has no writer, and an empty ring
       gives the drain side nothing to advance. */
    for ( i32 i = 0; i < PROF_MAX_THREADS && !r; ++i )
    {
        prof_ring_t* cand = &g_prof_rings[ i ];
        if ( sys_atomic_read( &cand->owner ) != PROF_RING_RETIRED )
            continue;
        u32 w  = ( u32 )sys_atomic_read( &cand->write_pos );
        u32 rd = ( u32 )sys_atomic_read( &cand->read_pos );
        if ( w == rd && sys_atomic_compare_exchange( &cand->owner,
                                                     PROF_RING_OWNED, PROF_RING_RETIRED ) == PROF_RING_RETIRED )
            r = cand;
    }

    if ( r )
    {
        /* Fresh owner: reset the drop count and label. Cursors stay free-running -- the
           slot was empty at claim, so the pending count is already zero. */
        i32 idx = ( i32 )( r - g_prof_rings );
        sys_atomic_write( &r->dropped, 0 );
        snprintf( r->label, sizeof( r->label ), "thread_%d", idx );

        /* Raise the high-water count so drain enumeration reaches this slot. */
        i32 c;
        while ( ( c = sys_atomic_read( &g_prof_ring_count ) ) < idx + 1 )
            if ( sys_atomic_compare_exchange( &g_prof_ring_count, idx + 1, c ) == c )
                break;
    }

#else

    /* Legacy bump allocator: slots are claimed forever. */
    i32 idx = sys_atomic_increment( &g_prof_ring_count ) - 1;
    if ( idx < PROF_MAX_THREADS )
    {
        r = &g_prof_rings[ idx ];
        sys_atomic_write( &r->owner, PROF_RING_OWNED );
        if ( !r->label[ 0 ] )
            snprintf( r->label, sizeof( r->label ), "thread_%d", idx );
    }

#endif    /* PROF_RING_RECYCLE */

    if ( !r )
        r = &g_prof_discard_ring;

    g_prof_tls_ring = r;
    return r;
}

/* INFO: Why release is explicit, and why RETIRED exists.

   C gives the library no portable "this thread is exiting" hook, so a thread that wants
   its slot back must say so (workers call PROF_THREAD_RELEASE on the way out). Forgetting
   is not a leak in the scary sense: the slot just stays claimed, which is the pre-recycle
   behavior and perfectly fine for long-lived threads like main or a fixed worker pool.

   The FREE/RETIRED two-step solves an ordering problem: the dying thread may leave events
   nobody has drained yet. Handing that slot straight to a new thread would splice two
   threads' histories into one stream and break BEGIN/END pairing. RETIRED means "no
   writer, data pending": the drain may still read it, nobody may claim it, and whichever
   drain empties it flips it FREE. All transitions go through atomics on the owner word,
   so every observer sees a coherent lifecycle no matter how the threads interleave.       */

/* Return the calling thread's ring to the pool -- the counterpart of the implicit claim
   on first emit; call it before a transient thread exits. Empty rings free immediately;
   rings with pending events RETIRE so the drain consumer can still collect them (it
   frees the slot after the emptying drain). Without PROF_RING_RECYCLE this is a no-op:
   the slot stays claimed, exactly the old fixed-pool behavior. */
void
prof_thread_release( void )
{
#if PROF_RING_RECYCLE
    prof_ring_t* r  = g_prof_tls_ring;
    g_prof_tls_ring = NULL;

    if ( !r || r->discard )
        return;

    /* A stale read_pos here only demotes a free to a retire -- the drain side then
       frees it, so nothing is lost either way. */
    u32 w  = ( u32 )r->write_pos;
    u32 rd = ( u32 )sys_atomic_read( &r->read_pos );
    sys_atomic_write( &r->owner, ( w == rd ) ? PROF_RING_FREE : PROF_RING_RETIRED );
#endif
}

/* INFO: The publish protocol -- the one memory-ordering rule that truly matters here.

   The consumer discovers new events by reading write_pos, so the payload stores MUST be
   visible to another core before the new cursor value is. Both the compiler and the CPU
   are free to reorder plain stores; the barriered sys_atomic_write (a release-semantics
   store) is what forbids that: everything stored above it is guaranteed visible before
   the cursor bump. The consumer's matching half is its barriered read of write_pos
   (acquire semantics) in prof_drain. This store-release / load-acquire pair is THE
   canonical lock-free handoff -- understand it once and every SPSC queue you ever read
   becomes the same code.

   Two more subtleties worth internalizing:
     - read_pos may be STALE here (the consumer just advanced it). Worst case, w - rd
       looks bigger than reality and we drop an event we could have kept -- wrong in the
       harmless direction. The "fix" would fence every push to slightly improve behavior
       at the full-ring edge, which is already a failure state. Not worth it.
     - Drop-NEWEST (refuse the write) instead of overwrite-oldest: overwriting would let
       the writer stomp the very slot the consumer is mid-memcpy on. Refusing keeps the
       writer out of the consumer's territory entirely, and the dropped counter still
       tells the truth about what was lost.                                                */

static void
prof_ring_push( prof_ring_t* r, u16 type, u32 id, i64 tick_ns )
{
    if ( r->discard )
    {
        sys_atomic_increment( &r->dropped );
        return;
    }

    u32 w  = ( u32 )r->write_pos;    /* own cursor: plain volatile read           */
    u32 rd = ( u32 )r->read_pos;     /* consumer cursor: stale only over-drops    */

    if ( w - rd >= PROF_RING_CAP )
    {
        sys_atomic_increment( &r->dropped );
        return;
    }

    prof_event_t* e = &r->events[ w & ( PROF_RING_CAP - 1 ) ];
    e->tick_ns      = tick_ns;
    e->id           = id;
    e->type         = type;
    e->_pad         = 0;

    /* Publish after the payload -- barriered store orders the slot fill before the
       cursor bump the consumer keys off. */
    sys_atomic_write( &r->write_pos, ( i32 )( w + 1 ) );
}

/* Fetch (or claim) the calling thread's ring. */
static ORB_INLINE prof_ring_t*
prof_ring_mine( void )
{
    prof_ring_t* r = g_prof_tls_ring;
    return r ? r : prof_ring_acquire();
}

/*==============================================================================================
    Zones
==============================================================================================*/

void
prof_zone_begin( u32 id )
{
    if ( !g_prof_enabled )
        return;
    prof_ring_push( prof_ring_mine(), PROF_EV_BEGIN, id, sys_tick_nanoseconds() );
}

void
prof_zone_begin_name( const char* name )
{
    if ( !g_prof_enabled )
        return;
    prof_zone_begin( prof_name_register( name ) );
}

void
prof_zone_end( void )
{
    if ( !g_prof_enabled )
        return;
    prof_ring_push( prof_ring_mine(), PROF_EV_END, 0, sys_tick_nanoseconds() );
}

/*==============================================================================================
    Name Registry

    Open-addressing table keyed by the name hash itself. Reads are lock-free (probe until
    matching id or empty slot); inserts re-probe under the mutex, copy the string into the
    pool, then publish the id last. Two distinct names hashing to the same id silently
    merge into one zone -- with 32-bit FNV over <= 256 names the odds are negligible.
==============================================================================================*/

/* INFO: The classic "lock-free read, locked write" registry.

   Reads probe with no lock, touching slot ids only through atomic loads. Writes take a
   real mutex: inserts happen a handful of times per run, so contention is irrelevant and
   the mutex makes the multi-field update (copy string, set offset, bump pool top)
   trivially correct. Splitting the workload this way -- optimize the frequent operation,
   simplify the rare one -- is the pragmatic middle ground before reaching for a fully
   lock-free table.

   The one crossing point is publish order: a lock-free reader must never see a nonzero id
   on a half-initialized slot. So the insert fills EVERYTHING else first and stores the id
   LAST through a barriered write; the reader's atomic load of the id is the acquire side,
   proving the offset and the pooled string bytes are already visible. Same release/acquire
   pattern as the ring cursor, applied to a hash-table slot.                               */

u32
prof_name_register( const char* name )
{
    prof_ensure_init();

    u32 id  = prof_hash_str( name );
    u32 idx = id & ( PROF_NAME_TABLE_SIZE - 1 );

    /* Lock-free fast path: already registered. */
    for ( u32 probe = 0; probe < PROF_NAME_TABLE_SIZE; ++probe )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_names[ idx ].id );
        if ( slot_id == id )
            return id;
        if ( slot_id == 0 )
            break;
        idx = ( idx + 1 ) & ( PROF_NAME_TABLE_SIZE - 1 );
    }

    /* Cold path: insert under the lock (re-probe -- another thread may have won). */
    mutex_lock( &g_prof_lock );

    idx = id & ( PROF_NAME_TABLE_SIZE - 1 );
    for ( u32 probe = 0; probe < PROF_NAME_TABLE_SIZE; ++probe )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_names[ idx ].id );
        if ( slot_id == id )
            break;

        if ( slot_id == 0 )
        {
            u32 len = ( u32 )strlen( name ) + 1;
            if ( g_prof_name_count >= PROF_MAX_NAMES || g_prof_name_pool_top + len > PROF_NAME_POOL_SIZE )
                break;    /* registry full: id still valid for capture, lookup returns NULL */

            memcpy( g_prof_name_pool + g_prof_name_pool_top, name, len );
            g_prof_names[ idx ].offset = g_prof_name_pool_top;
            g_prof_name_pool_top      += len;
            g_prof_name_count++;
            sys_atomic_write( &g_prof_names[ idx ].id, ( i32 )id );    /* publish last */
            break;
        }
        idx = ( idx + 1 ) & ( PROF_NAME_TABLE_SIZE - 1 );
    }

    mutex_unlock( &g_prof_lock );
    return id;
}

const char*
prof_name_lookup( u32 id )
{
    if ( id == 0 )
        return NULL;

    u32 idx = id & ( PROF_NAME_TABLE_SIZE - 1 );
    for ( u32 probe = 0; probe < PROF_NAME_TABLE_SIZE; ++probe )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_names[ idx ].id );
        if ( slot_id == id )
            return g_prof_name_pool + g_prof_names[ idx ].offset;
        if ( slot_id == 0 )
            return NULL;
        idx = ( idx + 1 ) & ( PROF_NAME_TABLE_SIZE - 1 );
    }
    return NULL;
}

/*==============================================================================================
    Frame + Counters
==============================================================================================*/

/* INFO: Frame marks are how a flat event stream becomes per-frame data.

   Zones say what happened; frame marks say where the frame boundaries fall, so a consumer
   can segment the stream ("frame 1041 took 21 ms, and HERE are its zones"). The counter
   is a global 64-bit fetch-add so the number is unique and monotonic no matter which
   thread marks, and the event lands in the CALLER's ring stamped with that number so
   streams from different threads can be correlated against the same frame axis.           */

u64
prof_frame_mark( void )
{
    u64 frame = ( u64 )sys_atomic_exchange_add_64( &g_prof_frame, 1 ) + 1;

    if ( g_prof_enabled )
        prof_ring_push( prof_ring_mine(), PROF_EV_FRAME, ( u32 )frame, sys_tick_nanoseconds() );

    return frame;
}

u64
prof_frame_number( void )
{
    return ( u64 )sys_atomic_read_64( &g_prof_frame );
}

/* Find a counter slot, creating it on first touch. Inserts are contiguous (under the
   lock), so the lock-free scan can stop at the first empty slot. NULL when full. */
static prof_counter_slot_t*
prof_counter_find( u32 id )
{
    for ( i32 i = 0; i < PROF_MAX_COUNTERS; ++i )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_counters[ i ].id );
        if ( slot_id == id )
            return &g_prof_counters[ i ];
        if ( slot_id == 0 )
            break;
    }

    prof_ensure_init();
    mutex_lock( &g_prof_lock );

    prof_counter_slot_t* found = NULL;
    for ( i32 i = 0; i < PROF_MAX_COUNTERS; ++i )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_counters[ i ].id );
        if ( slot_id == id )
        {
            found = &g_prof_counters[ i ];
            break;
        }
        if ( slot_id == 0 )
        {
            found = &g_prof_counters[ i ];
            sys_atomic_write( &g_prof_counters[ i ].id, ( i32 )id );
            break;
        }
    }

    mutex_unlock( &g_prof_lock );
    return found;
}

/* INFO: Counter atomicity -- set and add are different problems.

   counter_add MUST be a hardware fetch-add: with a plain read-modify-write, two threads
   adding 1 concurrently can both read 5 and both store 6 -- one increment silently lost
   (the textbook "lost update"). counter_set only needs an atomic 64-bit STORE: last-
   writer-wins is the intended meaning of "set", and the only atomicity requirement is
   that a reader never sees a torn half-old/half-new 64-bit value -- which a plain store
   does not guarantee on every target, hence sys_atomic_write_64.                          */

void
prof_counter_set( u32 id, i64 value )
{
    prof_counter_slot_t* c = prof_counter_find( id );
    if ( c )
        sys_atomic_write_64( &c->value, value );
}

void
prof_counter_add( u32 id, i64 delta )
{
    prof_counter_slot_t* c = prof_counter_find( id );
    if ( c )
        sys_atomic_exchange_add_64( &c->value, delta );
}

u32
prof_counters( prof_counter_t* out, u32 max )
{
    u32 n = 0;
    for ( i32 i = 0; i < PROF_MAX_COUNTERS && n < max; ++i )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_counters[ i ].id );
        if ( slot_id == 0 )
            break;
        out[ n ].id    = slot_id;
        out[ n ]._pad  = 0;
        out[ n ].value = sys_atomic_read_64( &g_prof_counters[ i ].value );
        n++;
    }
    return n;
}

/*==============================================================================================
    Capture Control
==============================================================================================*/

void
prof_set_enabled( bool enabled )
{
    sys_atomic_write( &g_prof_enabled, enabled ? 1 : 0 );
}

bool
prof_is_enabled( void )
{
    return sys_atomic_read( &g_prof_enabled ) != 0;
}

void
prof_thread_name( const char* name )
{
    prof_ring_t* r = prof_ring_mine();
    if ( !r->discard )
        snprintf( r->label, sizeof( r->label ), "%s", name );
}

/*==============================================================================================
    Drain -- single consumer
==============================================================================================*/

u32
prof_thread_count( void )
{
    i32 n = sys_atomic_read( &g_prof_ring_count );
    return ( u32 )( n > PROF_MAX_THREADS ? PROF_MAX_THREADS : n );
}

const char*
prof_thread_label( u32 thread_index )
{
    return thread_index < prof_thread_count() ? g_prof_rings[ thread_index ].label : NULL;
}

u32
prof_thread_dropped( u32 thread_index )
{
    if ( thread_index >= prof_thread_count() )
        return ( u32 )sys_atomic_read( &g_prof_discard_ring.dropped );
    return ( u32 )sys_atomic_read( &g_prof_rings[ thread_index ].dropped );
}

/* INFO: The consumer side, and why "single consumer" is a hard rule.

   read_pos has exactly one writer: this function. Two concurrent drainers would both read
   the same rd, copy the same events, and double-advance the cursor -- duplicated AND lost
   data, with no crash to warn you. Hence the engine contract: one drainer total (the
   overlay OR the dump, once per frame), and dump_flush loudly takes over that role while
   a capture is active.

   The copy itself: the ring is a circle but the caller's buffer is a line, so a pending
   span can wrap the physical end of the array -- hence up to two memcpys (the piece to
   the end, then the piece from slot 0). The barriered read of write_pos is the acquire
   half of the writer's publish; read_pos is published only AFTER the copy completes, so
   when the writer sees it advance, the slots are genuinely reusable.                      */

u32
prof_drain( u32 thread_index, prof_event_t* out, u32 max )
{
    if ( thread_index >= prof_thread_count() )
        return 0;

    prof_ring_t* r  = &g_prof_rings[ thread_index ];
    u32          rd = ( u32 )r->read_pos;                       /* own cursor            */
    u32          w  = ( u32 )sys_atomic_read( &r->write_pos );  /* acquire the publishes */
    u32          n  = w - rd;

    if ( n > max )
        n = max;

    if ( out && n )
    {
        u32 first = rd & ( PROF_RING_CAP - 1 );
        u32 span  = PROF_RING_CAP - first;
        if ( span > n )
            span = n;
        memcpy( out, &r->events[ first ], span * sizeof( prof_event_t ) );
        if ( n > span )
            memcpy( out + span, &r->events[ 0 ], ( n - span ) * sizeof( prof_event_t ) );
    }

    sys_atomic_write( &r->read_pos, ( i32 )( rd + n ) );

#if PROF_RING_RECYCLE
    /* Owner released with events pending and this drain emptied it: free the slot. The
       write cursor cannot move (retired = no writer), so the emptiness check holds. */
    if ( rd + n == w && sys_atomic_read( &r->owner ) == PROF_RING_RETIRED )
        sys_atomic_compare_exchange( &r->owner, PROF_RING_FREE, PROF_RING_RETIRED );
#endif

    return n;
}

/*==============================================================================================
    Unity build -- dump consumer, then the API wiring (must be last: prof_api.c assigns
    every function to g_prof_api_struct)
==============================================================================================*/

#include "engine/prof/prof_dump.c"

#ifndef PROF_API_C_PRELUDE
#include "engine/prof/prof_api.c"
#endif

// clang-format on
/*============================================================================================*/
