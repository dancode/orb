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

// clang-format on
/*============================================================================================*/
#endif    // PROF_API_H
