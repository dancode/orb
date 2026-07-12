/*==============================================================================================

    engine/prof/prof_api.h - prof_api_t function-pointer struct, module accessor macros, and
    the PROF_* capture macros.

    The capture macros route through the prof() gateway so ONE macro text serves every
    consumer: hosts and static builds resolve to a direct (LTO-devirtualizable) call, DLL
    modules pay a single cached-pointer load. A module that never fetched the api (or a
    level-0 build) captures nothing -- the macros null-check the gateway.

==============================================================================================*/
#ifndef PROF_API_H
#define PROF_API_H

#include "engine/prof/prof.h"
#include "engine/mod/mod_import.h"

// clang-format off

/*==============================================================================================
    Profiler Runtime API

    The vtable carries every entry point at every ORB_PROFILE level -- the level gates the
    call-site macros only, never the struct shape, so func_api_size is level-invariant.
==============================================================================================*/

/* INFO: Why a struct full of function pointers.

   prof is compiled INTO the host exe. A hot-reloaded DLL cannot call prof_zone_begin by
   name -- the linker that built the DLL has no idea where that symbol lives inside the
   exe. So, like every engine module here, prof publishes ONE struct of function pointers
   through the module registry; a DLL fetches the struct pointer once at init/reload and
   calls through it thereafter. Cost per call: one extra pointer load versus a direct
   call. In static/monolithic builds the gateway below bypasses the table entirely --
   identical source text, two different machine-code outcomes.                             */

typedef struct prof_api_s
{
    /* Zones (level 2). zone_begin takes a pre-registered id (fastest); zone_begin_name
       hashes + registers per call (convenience / dynamic names). */
    void        ( *zone_begin )      ( u32 id );
    void        ( *zone_begin_name ) ( const char* name );
    void        ( *zone_end )        ( void );
    u32         ( *name_register )   ( const char* name );      /* intern; returns id (the hash) */
    const char* ( *name_lookup )     ( u32 id );                /* NULL when id is unregistered  */

    /* Frame + counters (level 1). frame_mark stamps the calling thread's ring and returns
       the new global frame number. */
    u64         ( *frame_mark )      ( void );
    u64         ( *frame_number )    ( void );
    void        ( *counter_set )     ( u32 id, i64 value );
    void        ( *counter_add )     ( u32 id, i64 delta );
    u32         ( *counters )        ( prof_counter_t* out, u32 max );   /* snapshot; returns count */

    /* Capture control. Runtime switch under the compile-time level; thread_name labels
       the calling thread's ring for readouts. thread_release returns the calling thread's
       ring to the free pool (PROF_RING_RECYCLE) -- call it before a transient thread
       exits; a no-op when recycling is compiled out. */
    void        ( *set_enabled )     ( bool enabled );
    bool        ( *is_enabled )      ( void );
    void        ( *thread_name )     ( const char* name );
    void        ( *thread_release )  ( void );

    /* Drain -- SINGLE consumer (one reader total, once per frame). Copies up to max
       pending events from one thread's ring into out and consumes them; out may be NULL
       to discard. thread_dropped counts events lost to ring overflow. */
    u32         ( *thread_count )    ( void );
    const char* ( *thread_label )    ( u32 thread_index );
    u32         ( *thread_dropped )  ( u32 thread_index );
    u32         ( *drain )           ( u32 thread_index, prof_event_t* out, u32 max );

    /* Chrome-trace dump (chrome://tracing, https://ui.perfetto.dev). dump_begin opens the
       file; dump_flush drains EVERY ring into it (the dump IS the single consumer while
       active -- call once per frame); dump_end writes the final flush + thread metadata
       and closes. Consumer-side only, never call from worker threads. */
    bool        ( *dump_begin )      ( const char* path );
    u32         ( *dump_flush )      ( void );
    void        ( *dump_end )        ( void );
    bool        ( *dump_active )     ( void );

    /* Hitch capture (ORB_PROFILE_HITCH; stubs when compiled out). hitch_arm arms
       (threshold_ms > 0, resets the session) or disarms (<= 0); hitch_update runs once
       per frame from the consumer thread -- it IS the drain consumer while armed -- and
       auto-writes "<prefix>_<frame>.json" when frame_ms crosses the threshold, returning
       the events written (0 = no capture). Inert while an explicit dump is active. */
    void        ( *hitch_arm )       ( f64 threshold_ms, const char* path_prefix );
    bool        ( *hitch_armed )     ( void );
    u32         ( *hitch_update )    ( f64 frame_ms );
    u32         ( *hitch_count )     ( void );                    /* captures since armed          */
    const char* ( *hitch_last_path ) ( void );                    /* NULL until a capture fires    */

    /* Memory hooks (ORB_PROFILE_MEM; stubs when compiled out). Call alloc/free from the
       allocator's own call sites, balanced, from any thread; mem_stats snapshots every
       scope (returns count). Scopes also land in dumps as "used"/"peak" counter tracks. */
    void        ( *mem_alloc )       ( u32 id, i64 bytes );
    void        ( *mem_free )        ( u32 id, i64 bytes );
    u32         ( *mem_stats )       ( prof_mem_t* out, u32 max );

} prof_api_t;

/*============================================================================================*/
/* prof is always statically linked into the host -- PROF_STATIC is set by the build for
   every target that declares `dep prof`. */

#if defined( BUILD_STATIC ) || defined( PROF_STATIC )
    MOD_GATEWAY_STATIC( prof_api_t, prof )
    #define MOD_USE_PROF    /* static build -- no pointer needed */
    #define MOD_FETCH_PROF  true
#else
    MOD_GATEWAY_DYNAMIC( prof_api_t, prof )
    #define MOD_USE_PROF    MOD_DEFINE_API_PTR( prof_api_t, prof )
    #define MOD_FETCH_PROF  MOD_FETCH_API( prof_api_t, prof )
#endif

/* INFO: The prof() gateway, decoded.

   From here on every consumer writes prof()->fn(...) no matter how it is built:
     - static link (host exe, monolithic build): prof() is an inline that returns
       &g_prof_api_struct. The optimizer sees the final struct and devirtualizes the
       whole thing into direct calls.
     - dynamic DLL: prof() reads a pointer that MOD_FETCH_PROF cached during the module's
       init/reload. NULL means "prof is not loaded", which is why the capture macros
       null-check the gateway -- a module keeps working with profiling absent.             */

/*==============================================================================================
    Fast path (stage 2) -- ORB_PROFILE_FAST

    Inline TLS-ring writes for the ZONE macros: no vtable call, no function call at all on
    the hot path -- one TLS load, one enabled check, one cursor check, one 16-byte store,
    one publishing cursor write. Active only in statically-linked TUs (the inline body
    needs direct sys calls and prof's TLS ring pointer, both host-only); everywhere else
    the same macro text compiles to the vtable path. Every COLD case (no ring claimed yet,
    discard ring, disabled, ring full) falls back to the full call, which owns claiming
    and drop accounting -- the two paths are behaviorally identical.
==============================================================================================*/

/* INFO: What "stage 2" buys, concretely.

   The vtable path per event: load api pointer -> indirect call -> callee prologue -> TLS
   load -> checks -> clock read -> stores -> return. The inline path deletes the call frame
   entirely; what remains AT THE CALL SITE is: one TLS load, three predictable branches,
   the clock read, four plain stores, one atomic store. Because the QPC clock read
   (~20-40 ns) dominates both flavors, the measured win is modest on x64 -- the deeper
   reason this path exists is to cap the ceiling: zone macros can sit inside per-entity
   inner loops with no function call in sight, and a future cheaper clock (calibrated
   rdtsc) would widen the gap.

   The fallback shape is the part worth copying into your own lock-free code: the inline
   body handles ONLY the perfect case (have a ring, enabled, not full, not the discard
   ring). Every rare case bails into the full call, which already owns ring claiming and
   drop accounting. The hot body stays a straight line, and the two implementations cannot
   drift apart behaviorally because the cold logic exists exactly once.                    */

#if ORB_PROFILE_FAST && ORB_PROFILE >= 2 && ( defined( BUILD_STATIC ) || defined( PROF_STATIC ) )
    #define PROF_FAST_ACTIVE 1
#else
    #define PROF_FAST_ACTIVE 0
#endif

#if PROF_FAST_ACTIVE

#include "engine/sys/sys_host.h"    /* sys_tick_nanoseconds + sys_atomic_write */

/* prof.c internals, reachable here because this TU links prof statically. */
extern ORB_THREAD_LOCAL prof_ring_t* g_prof_tls_ring;
extern volatile i32                  g_prof_enabled;

/* Hot path only: false sends the caller to the full vtable call, which handles every
   cold case (claim ring, discard ring, disabled, full ring drop accounting). */
static ORB_INLINE bool
prof_fast_try_emit( u16 type, u32 id )
{
    prof_ring_t* r = g_prof_tls_ring;
    if ( !r || r->discard || !g_prof_enabled )
        return false;

    u32 w  = ( u32 )r->write_pos;
    u32 rd = ( u32 )r->read_pos;
    if ( w - rd >= PROF_RING_CAP )
        return false;

    prof_event_t* e = &r->events[ w & ( PROF_RING_CAP - 1 ) ];
    e->tick_ns      = sys_tick_nanoseconds();
    e->id           = id;
    e->type         = type;
    e->_pad         = 0;

    /* Publish after the payload -- same ordering contract as prof_ring_push. */
    sys_atomic_write( &r->write_pos, ( i32 )( w + 1 ) );
    return true;
}

static ORB_INLINE void
prof_fast_begin( u32 id )
{
    if ( !prof_fast_try_emit( PROF_EV_BEGIN, id ) )
        prof()->zone_begin( id );
}

static ORB_INLINE void
prof_fast_end( void )
{
    if ( !prof_fast_try_emit( PROF_EV_END, 0 ) )
        prof()->zone_end();
}

#endif    /* PROF_FAST_ACTIVE */

/*==============================================================================================
    Capture Macros

    PROF_ZONE_BEGIN / PROF_ZONE_END is the fast form: the name is hashed and registered
    once per call site (cached in a function-local static; a hot-reloaded DLL's statics
    reset and simply re-register to the same id). PROF_SCOPE is the convenience form
    wrapping one statement/block; it re-hashes the name per entry (~a few ns) and, being
    a for-trick, leaks the zone open if the body exits via break/return -- prefer the
    BEGIN/END pair in hot or early-exiting code. (PROF_SCOPE always routes through the
    vtable; only the BEGIN/END pair gets the ORB_PROFILE_FAST inline path.)
==============================================================================================*/

/* INFO: Why these are macros and not functions.

   Three jobs only a macro can do here:
     1. Vanish completely at level 0 -- even an empty inline function still evaluates its
        arguments; ( (void)0 ) does not even keep the zone name string in the binary.
     2. Plant a PER-CALL-SITE static (s_prof_zone_id): the name is hashed and registered
        on the first pass through each site, then every later pass is a load + compare.
        Each macro expansion is textually distinct code, so C gives each its own static.
     3. Wrap in do { } while ( 0 ) so the expansion is exactly one statement and binds
        correctly under an unbraced if/else.
   After a DLL hot-reload the statics reset to zero -- the site simply re-registers and,
   because the id is a pure hash of the name, lands on the SAME id: history survives.      */

#if PROF_FAST_ACTIVE

    /* Static link: prof() can never be NULL, so the gateway guard is dropped. */
    #define PROF_ZONE_BEGIN( name_lit )                                       \
        do                                                                    \
        {                                                                     \
            static u32 s_prof_zone_id;                                        \
            if ( !s_prof_zone_id )                                            \
                s_prof_zone_id = prof()->name_register( name_lit );           \
            prof_fast_begin( s_prof_zone_id );                                \
        } while ( 0 )

    #define PROF_ZONE_END()                                                   \
        do                                                                    \
        {                                                                     \
            prof_fast_end();                                                  \
        } while ( 0 )

#elif ORB_PROFILE >= 2

    #define PROF_ZONE_BEGIN( name_lit )                                       \
        do                                                                    \
        {                                                                     \
            static u32 s_prof_zone_id;                                        \
            if ( prof() )                                                     \
            {                                                                 \
                if ( !s_prof_zone_id )                                        \
                    s_prof_zone_id = prof()->name_register( name_lit );       \
                prof()->zone_begin( s_prof_zone_id );                         \
            }                                                                 \
        } while ( 0 )

    #define PROF_ZONE_END()                                                   \
        do                                                                    \
        {                                                                     \
            if ( prof() )                                                     \
                prof()->zone_end();                                           \
        } while ( 0 )

#else

    #define PROF_ZONE_BEGIN( name_lit )  ( ( void )0 )
    #define PROF_ZONE_END()              ( ( void )0 )

#endif    /* zone macro flavor */

#if ORB_PROFILE >= 2

    /* Expression-shaped helpers so PROF_SCOPE fits in a for() header. */
    static inline u32
    prof_scope_enter( const char* name )
    {
        if ( prof() )
            prof()->zone_begin_name( name );
        return 1;
    }

    static inline u32
    prof_scope_leave( void )
    {
        if ( prof() )
            prof()->zone_end();
        return 0;
    }

    /* INFO: The for-trick: for( init; cond; increment ) runs enter() once in the init,
       the body once (cond starts at 1), then the increment (leave) flips cond to 0 and
       the loop exits. It lets one macro wrap the following { block } in begin/end with
       no matching END macro required -- but a break/return inside the block skips the
       increment, which is exactly the "leaks the zone open" hazard documented above.      */

    #define PROF_SCOPE( name_lit )                                            \
        for ( u32 prof_scope_it = prof_scope_enter( name_lit );               \
              prof_scope_it;                                                  \
              prof_scope_it = prof_scope_leave() )

#else

    /* Still runs the body exactly once. */
    #define PROF_SCOPE( name_lit )                                            \
        for ( int prof_scope_it = 1; prof_scope_it; prof_scope_it = 0 )

#endif    /* ORB_PROFILE >= 2 */

#if ORB_PROFILE >= 1

    #define PROF_FRAME_MARK()                                                 \
        do                                                                    \
        {                                                                     \
            if ( prof() )                                                     \
                prof()->frame_mark();                                         \
        } while ( 0 )

    #define PROF_COUNTER_SET( name_lit, val )                                 \
        do                                                                    \
        {                                                                     \
            static u32 s_prof_counter_id;                                     \
            if ( prof() )                                                     \
            {                                                                 \
                if ( !s_prof_counter_id )                                     \
                    s_prof_counter_id = prof()->name_register( name_lit );    \
                prof()->counter_set( s_prof_counter_id, ( i64 )( val ) );     \
            }                                                                 \
        } while ( 0 )

    #define PROF_COUNTER_ADD( name_lit, delta )                               \
        do                                                                    \
        {                                                                     \
            static u32 s_prof_counter_id;                                     \
            if ( prof() )                                                     \
            {                                                                 \
                if ( !s_prof_counter_id )                                     \
                    s_prof_counter_id = prof()->name_register( name_lit );    \
                prof()->counter_add( s_prof_counter_id, ( i64 )( delta ) );   \
            }                                                                 \
        } while ( 0 )

    #define PROF_THREAD_NAME( name_lit )                                      \
        do                                                                    \
        {                                                                     \
            if ( prof() )                                                     \
                prof()->thread_name( name_lit );                              \
        } while ( 0 )

    /* Call before a transient thread exits so its ring returns to the pool. */
    #define PROF_THREAD_RELEASE()                                             \
        do                                                                    \
        {                                                                     \
            if ( prof() )                                                     \
                prof()->thread_release();                                     \
        } while ( 0 )

#else

    #define PROF_FRAME_MARK()                    ( ( void )0 )
    #define PROF_COUNTER_SET( name_lit, val )    ( ( void )0 )
    #define PROF_COUNTER_ADD( name_lit, delta )  ( ( void )0 )
    #define PROF_THREAD_NAME( name_lit )         ( ( void )0 )
    #define PROF_THREAD_RELEASE()                ( ( void )0 )

#endif    /* ORB_PROFILE >= 1 */

/* Memory hooks -- place at the allocator's own alloc/free sites, always balanced. Same
   per-call-site id caching as the counter macros; vanish when the level or
   ORB_PROFILE_MEM compiles the feature out. */
#if ORB_PROFILE >= 1 && ORB_PROFILE_MEM

    #define PROF_MEM_ALLOC( name_lit, bytes )                                 \
        do                                                                    \
        {                                                                     \
            static u32 s_prof_mem_id;                                         \
            if ( prof() )                                                     \
            {                                                                 \
                if ( !s_prof_mem_id )                                         \
                    s_prof_mem_id = prof()->name_register( name_lit );        \
                prof()->mem_alloc( s_prof_mem_id, ( i64 )( bytes ) );         \
            }                                                                 \
        } while ( 0 )

    #define PROF_MEM_FREE( name_lit, bytes )                                  \
        do                                                                    \
        {                                                                     \
            static u32 s_prof_mem_id;                                         \
            if ( prof() )                                                     \
            {                                                                 \
                if ( !s_prof_mem_id )                                         \
                    s_prof_mem_id = prof()->name_register( name_lit );        \
                prof()->mem_free( s_prof_mem_id, ( i64 )( bytes ) );          \
            }                                                                 \
        } while ( 0 )

#else

    #define PROF_MEM_ALLOC( name_lit, bytes )    ( ( void )0 )
    #define PROF_MEM_FREE( name_lit, bytes )     ( ( void )0 )

#endif    /* memory hooks */

// clang-format on
/*============================================================================================*/
#endif    // PROF_API_H
