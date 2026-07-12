/*==============================================================================================

    engine/prof/prof.h - Profiler types, enums, constants, and the zone-id hash.

    Pure types only -- no vtable, no function declarations.
    Include engine/prof/prof_api.h for the runtime vtable + capture macros (DLL modules).
    Include engine/prof/prof_host.h for direct function calls (host/tests).

    prof is a leaf engine library (deps: sys only), modeled on engine/ref: it sits below
    core so core, mod, app, and everything above them can be instrumented. Capture is
    scoped zones written as fixed 16-byte events into per-thread SPSC ring buffers; a
    single consumer drains the rings once per frame and rebuilds nesting from BEGIN/END
    order. Names are interned once into an internal pool; hot-path events carry only the
    32-bit name hash.

==============================================================================================*/
#ifndef PROF_H
#define PROF_H

#include "orb.h"

// clang-format off

/*==============================================================================================
    Compile-time capture level

    ORB_PROFILE gates the CALL-SITE macros in prof_api.h; the library itself always builds
    in full (the vtable shape never changes with the level, so mixed-level binaries stay
    ABI-compatible). Within an enabled level, capture also has a runtime on/off switch
    (prof()->set_enabled) so instrumented builds only pay while recording.

        0  off    -- every PROF_* macro expands to nothing
        1  frame  -- frame marks, counters, thread names
        2  zones  -- + scoped zone capture (dev default)
==============================================================================================*/

#ifndef ORB_PROFILE
    #define ORB_PROFILE 2
#endif

/*==============================================================================================
    Limits

    All storage is static fixed pools (2 MB of rings in BSS at the defaults). Rings are
    claimed one per thread on first use and never released -- sized for a host + a fixed
    worker pool, not for transient thread churn. Threads past PROF_MAX_THREADS fall back
    to a shared discard ring that only counts drops.
==============================================================================================*/

#define PROF_MAX_THREADS       16              /* per-thread rings; extras discard          */
#define PROF_RING_CAP          8192            /* events per ring; must be a power of two   */
#define PROF_MAX_NAMES         256             /* distinct zone/counter names               */
#define PROF_NAME_TABLE_SIZE   512             /* id -> name hash table; power of two       */
#define PROF_NAME_POOL_SIZE    ( 8 * 1024 )    /* interned name string bytes                */
#define PROF_MAX_COUNTERS      64              /* distinct live counters                    */
#define PROF_THREAD_NAME_MAX   32              /* thread label bytes, including NUL         */

/*==============================================================================================
    prof_hash_str -- case-insensitive FNV-1a

    Zone/counter ids ARE this hash of their name -- stable across sessions, hot-reloads,
    and modules (no pointer identity, so a reloaded DLL's zones keep their history).
    Must remain algorithmically identical to sid_hash / ref_hash_str so a prof id can be
    compared against a SID of the same string. A computed hash of 0 is remapped to 1
    (0 is the empty-slot sentinel in the name table).
==============================================================================================*/

static inline u32
prof_hash_str( const char* s )
{
    u32 h = 2166136261u;
    while ( *s )
    {
        unsigned char c = ( unsigned char )*s++;
        if ( c >= 'A' && c <= 'Z' )
            c = ( unsigned char )( c + 32 );
        h = ( h ^ c ) * 16777619u;
    }
    return h ? h : 1u;
}

/*==============================================================================================
    Event

    One fixed 16-byte record in a thread's ring. Nesting is NOT stored -- BEGIN/END events
    on one thread are properly nested by construction, so the drain side rebuilds the zone
    tree with a trivial stack walk. Consumers must tolerate unpaired events: overflow drops
    and mid-zone enable/disable can orphan a BEGIN or END.

    tick_ns is sys_tick_nanoseconds() -- the engine-wide QPC clock domain, so zone times
    compare directly against frame stats and the frame clock.
==============================================================================================*/

typedef enum prof_event_type_e
{
    PROF_EV_NONE  = 0,
    PROF_EV_BEGIN = 1,    /* zone open:  id = zone name hash                 */
    PROF_EV_END   = 2,    /* zone close: id = 0 (pair by nesting)            */
    PROF_EV_FRAME = 3,    /* frame mark: id = low 32 bits of frame number    */

} prof_event_type_t;

typedef struct prof_event_s
{
    i64 tick_ns;    // sys_tick_nanoseconds() at emit
    u32 id;         // zone/counter name hash (see prof_event_type_t per type)
    u16 type;       // prof_event_type_t
    u16 _pad;       // reserved
} prof_event_t;

/*==============================================================================================
    Counter snapshot

    Counters are NOT ring events: they live in a small global table (set/add from any
    thread) and are sampled by the consumer once per frame via prof()->counters().
==============================================================================================*/

typedef struct prof_counter_s
{
    u32 id;         // counter name hash
    u32 _pad;       // reserved
    i64 value;      // current value at snapshot time
} prof_counter_t;

// clang-format on
/*============================================================================================*/
#endif    // PROF_H
