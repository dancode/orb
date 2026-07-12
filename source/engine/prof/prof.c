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
      - Rings are claimed one per thread on first emit and never released. Threads past
        PROF_MAX_THREADS share a discard ring that only counts drops.

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
_Static_assert( ( PROF_NAME_TABLE_SIZE & ( PROF_NAME_TABLE_SIZE - 1 ) ) == 0,
                                                                     "name table must be a power of two" );
_Static_assert( PROF_NAME_TABLE_SIZE >= 2 * PROF_MAX_NAMES,          "name table needs <= 0.5 load factor" );

/*==============================================================================================
    Storage
==============================================================================================*/

typedef struct prof_ring_s
{
    volatile i32 write_pos;                    // modular cursor: total events written (owner thread)
    volatile i32 read_pos;                     // modular cursor: total events consumed (drain thread)
    volatile i32 dropped;                      // events lost to overflow / discard
    bool         discard;                      // overflow-thread ring: count drops, store nothing
    char         label[ PROF_THREAD_NAME_MAX ];// thread display name for readouts
    prof_event_t events[ PROF_RING_CAP ];

} prof_ring_t;

/* Name registry slot: id published LAST (atomic write) so lock-free readers that see a
   nonzero id are guaranteed to see the offset it guards. Slots are never removed. */
typedef struct prof_name_slot_s
{
    volatile i32 id;                           // zone/counter name hash; 0 = empty
    u32          offset;                       // byte offset into the name pool

} prof_name_slot_t;

typedef struct prof_counter_slot_s
{
    volatile i32 id;                           // counter name hash; 0 = empty; inserts are contiguous
    volatile i64 value;

} prof_counter_slot_t;

static prof_ring_t         g_prof_rings[ PROF_MAX_THREADS ];
static prof_ring_t         g_prof_discard_ring = { .discard = true, .label = "overflow" };

static volatile i32        g_prof_ring_count;                        // claimed rings (may overshoot MAX)
static volatile i32        g_prof_enabled = 1;                       // runtime capture switch
static volatile i64        g_prof_frame;                             // global frame counter

static prof_name_slot_t    g_prof_names[ PROF_NAME_TABLE_SIZE ];
static char                g_prof_name_pool[ PROF_NAME_POOL_SIZE ];
static u32                 g_prof_name_pool_top;
static u32                 g_prof_name_count;

static prof_counter_slot_t g_prof_counters[ PROF_MAX_COUNTERS ];

static mutex_t             g_prof_lock;                              // guards all cold-path inserts
static volatile i32        g_prof_boot;                              // 0 = cold, 1 = initializing, 2 = ready

static ORB_THREAD_LOCAL prof_ring_t* t_prof_ring;                    // this thread's claimed ring

/*==============================================================================================
    Lifecycle
==============================================================================================*/

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
    t_prof_ring          = NULL;

    sys_atomic_write( &g_prof_boot, 0 );
}

/*==============================================================================================
    Rings
==============================================================================================*/

static prof_ring_t*
prof_ring_acquire( void )
{
    prof_ensure_init();

    i32          idx = sys_atomic_increment( &g_prof_ring_count ) - 1;
    prof_ring_t* r;

    if ( idx >= PROF_MAX_THREADS )
    {
        r = &g_prof_discard_ring;
    }
    else
    {
        r = &g_prof_rings[ idx ];
        if ( !r->label[ 0 ] )
            snprintf( r->label, sizeof( r->label ), "thread_%d", idx );
    }

    t_prof_ring = r;
    return r;
}

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
    prof_ring_t* r = t_prof_ring;
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

void
prof_counter_set( u32 id, i64 value )
{
    prof_counter_slot_t* c = prof_counter_find( id );
    if ( !c )
        return;

    /* No atomic 64-bit store in the sys layer -- emulate with a CAS loop. */
    i64 old = sys_atomic_read_64( &c->value );
    while ( sys_atomic_compare_exchange_64( &c->value, value, old ) != old )
        old = sys_atomic_read_64( &c->value );
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
    return n;
}

/*==============================================================================================
    API wiring  (must be last -- assigns every function to g_prof_api_struct)
==============================================================================================*/

#ifndef PROF_API_C_PRELUDE
#include "engine/prof/prof_api.c"
#endif

// clang-format on
/*============================================================================================*/
