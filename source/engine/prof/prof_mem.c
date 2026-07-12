/*==============================================================================================

    engine/prof/prof_mem.c - Memory hooks: per-scope allocation accounting.

    A scope is one named allocation domain (an arena, a pool, a subsystem heap). The
    allocator calls prof_mem_alloc/free with the scope id and a byte count at its own
    call sites; the table maintains live bytes, high-water peak, and alloc/free event
    counts exactly, from any thread, lock-free on the hot path. Consumers snapshot via
    prof_mem_stats; the Chrome-trace dump samples every scope each flush as a counter
    track ("used" + "peak" series).

    Slot layout mirrors the counter table: contiguous inserts under the cold-path mutex,
    id published last, lock-free find that may stop at the first empty slot. Whole file
    is gated by ORB_PROFILE_MEM; at 0 the entry points compile to inert stubs.

    Included by prof.c (unity) before prof_dump.c.

==============================================================================================*/

#if ORB_PROFILE_MEM

// clang-format off

typedef struct prof_mem_slot_s
{
    volatile i32 id;         // scope name hash; 0 = empty; inserts are contiguous
    volatile i64 current;    // live bytes (fetch-add)
    volatile i64 peak;       // high-water live bytes (CAS-max)
    volatile i64 allocs;     // allocation events (fetch-add)
    volatile i64 frees;      // free events (fetch-add)

} prof_mem_slot_t;

static prof_mem_slot_t g_prof_mem[ PROF_MAX_MEM_SCOPES ];

// clang-format on

/* Full reset for prof_exit (unity-internal). */
static void
prof_mem_reset( void )
{
    memset( ( void* )g_prof_mem, 0, sizeof( g_prof_mem ) );
}

/* Find a scope slot, creating it on first touch -- same shape as prof_counter_find. */
static prof_mem_slot_t*
prof_mem_find( u32 id )
{
    for ( i32 i = 0; i < PROF_MAX_MEM_SCOPES; ++i )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_mem[ i ].id );
        if ( slot_id == id )
            return &g_prof_mem[ i ];
        if ( slot_id == 0 )
            break;
    }

    prof_ensure_init();
    mutex_lock( &g_prof_lock );

    prof_mem_slot_t* found = NULL;
    for ( i32 i = 0; i < PROF_MAX_MEM_SCOPES; ++i )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_mem[ i ].id );
        if ( slot_id == id )
        {
            found = &g_prof_mem[ i ];
            break;
        }
        if ( slot_id == 0 )
        {
            found = &g_prof_mem[ i ];
            sys_atomic_write( &g_prof_mem[ i ].id, ( i32 )id );
            break;
        }
    }

    mutex_unlock( &g_prof_lock );
    return found;
}

/*==============================================================================================
    Public surface
==============================================================================================*/

/* INFO: The CAS-max loop -- maintaining a maximum without a lock.

   current is easy: fetch-add is exact under any interleaving. peak is the interesting
   one, because "peak = max( peak, current )" is a read-modify-write of TWO values and no
   single hardware op does it. The loop pattern: read peak; if our current is not higher,
   done (the common case -- zero writes); otherwise try to CAS peak from the value we read
   to ours. If another thread raced a bigger value in first, the CAS fails, we re-read and
   re-decide -- either someone else's peak already covers ours (loop exits) or we try
   again. The result converges on the true maximum because a failed CAS always means the
   value MOVED, and it only ever moves upward. This read-check-CAS-retry shape is the
   standard recipe for any monotonic lock-free aggregate (max, min, high-water).

   One honest caveat: current-then-peak is two separate atomics, not one, so a reader can
   observe the new current before the peak store lands -- a snapshot may briefly show
   current > peak mid-race. Closing that window would need a lock on every allocation;
   telemetry does not warrant it, and the peak itself still converges exactly.             */

void
prof_mem_alloc( u32 id, i64 bytes )
{
    prof_mem_slot_t* m = prof_mem_find( id );
    if ( !m )
        return;    /* scope table full: accounting for this scope is silently off */

    i64 now = sys_atomic_exchange_add_64( &m->current, bytes ) + bytes;
    sys_atomic_exchange_add_64( &m->allocs, 1 );

    i64 peak;
    while ( ( peak = sys_atomic_read_64( &m->peak ) ) < now )
        if ( sys_atomic_compare_exchange_64( &m->peak, now, peak ) == peak )
            break;
}

void
prof_mem_free( u32 id, i64 bytes )
{
    prof_mem_slot_t* m = prof_mem_find( id );
    if ( !m )
        return;

    sys_atomic_exchange_add_64( &m->current, -bytes );
    sys_atomic_exchange_add_64( &m->frees, 1 );
}

/* Snapshot every live scope -- same contract as prof_counters. */
u32
prof_mem_stats( prof_mem_t* out, u32 max )
{
    u32 n = 0;
    for ( i32 i = 0; i < PROF_MAX_MEM_SCOPES && n < max; ++i )
    {
        u32 slot_id = ( u32 )sys_atomic_read( &g_prof_mem[ i ].id );
        if ( slot_id == 0 )
            break;
        out[ n ].id      = slot_id;
        out[ n ]._pad    = 0;
        out[ n ].current = sys_atomic_read_64( &g_prof_mem[ i ].current );
        out[ n ].peak    = sys_atomic_read_64( &g_prof_mem[ i ].peak );
        out[ n ].allocs  = sys_atomic_read_64( &g_prof_mem[ i ].allocs );
        out[ n ].frees   = sys_atomic_read_64( &g_prof_mem[ i ].frees );
        n++;
    }
    return n;
}

#else /* ORB_PROFILE_MEM == 0 -- inert stubs; the vtable shape never changes */

// clang-format off

static void prof_mem_reset ( void ) { }

void prof_mem_alloc ( u32 id, i64 bytes )           { UNUSED( id ); UNUSED( bytes ); }
void prof_mem_free  ( u32 id, i64 bytes )           { UNUSED( id ); UNUSED( bytes ); }
u32  prof_mem_stats ( prof_mem_t* out, u32 max )    { UNUSED( out ); UNUSED( max ); return 0; }

// clang-format on

#endif    /* ORB_PROFILE_MEM */

/*============================================================================================*/
