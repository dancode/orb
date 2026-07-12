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

/* INFO: What an instrumenting profiler is, in one page.

   There are two big families of profilers:

     1. SAMPLING profilers (VTune, Superluminal, the VS profiler) interrupt the program a
        few thousand times a second and record the callstack. Zero code changes and whole-
        program coverage, but the results are statistical -- blurry at frame granularity.

     2. INSTRUMENTING profilers (Tracy, Optick, this library) require the code to announce
        "work X starts NOW" / "work X ends NOW". Exact, frame-accurate, and events carry
        engine meaning (zones named by the developer), but only measure what you marked.

   This library is the second kind. Its entire runtime job is deliberately tiny: append a
   timestamped BEGIN or END record to an in-memory log. Everything smart -- pairing BEGINs
   with ENDs, computing durations, drawing flame graphs -- happens LATER, outside the
   measured code (a drain consumer, an overlay, an offline viewer). That split is the core
   design idea: the hot path must be so cheap that measuring the program does not change
   the program (the "observer effect" every profiler fights).

   The data flow, end to end:

       PROF_ZONE_BEGIN("sim")  -> 16-byte event -> this thread's ring buffer     (hot, ~ns)
       PROF_ZONE_END()         -> 16-byte event -> same ring                     (hot, ~ns)
       once per frame:            a single consumer drains every ring            (cold)
       consumer replays BEGIN/END order to rebuild nesting, then renders / dumps (cold)   */

/*==============================================================================================
    Compile-time capture level

    ORB_PROFILE gates the CALL-SITE macros in prof_api.h; the library itself always builds
    in full (the vtable shape never changes with the level, so mixed-level binaries stay
    ABI-compatible). Within an enabled level, capture also has a runtime on/off switch
    (prof()->set_enabled) so instrumented builds only pay while recording.

        0  off    -- every PROF_* macro expands to nothing
        1  frame  -- frame marks, counters, thread names
        2  zones  -- + scoped zone capture (dev default)

    ORB_PROFILE_FAST (default 0) additionally routes the ZONE macros through an inline
    TLS-ring write instead of the vtable call. It only takes effect in translation units
    statically linked with prof (PROF_STATIC / BUILD_STATIC) -- the inline path needs
    direct sys calls, which DLL modules do not have, so they silently keep the vtable
    path. Both flavors produce identical events; the toggle is free to flip per build.

    PROF_RING_RECYCLE (default 1) enables the ring release path: a thread that calls
    prof()->thread_release() (PROF_THREAD_RELEASE) before exiting returns its ring to a
    free pool for the next thread. At 0, rings are claimed forever (the original
    fixed-pool behavior -- fine for a host + fixed worker pool, no transient churn).

    ORB_PROFILE_HITCH (default 1) compiles the hitch-capture consumer (prof_hitch.c):
    an armed per-frame monitor that folds drained events into rolling history buffers
    and auto-writes a Chrome trace of the recent past when a frame exceeds a threshold.
    At 0 the history storage vanishes and the hitch entry points become inert stubs
    (the vtable shape is unchanged -- only the bodies are gated).

    ORB_PROFILE_MEM (default 1) compiles the memory hooks (prof_mem.c): per-scope
    allocation accounting (live bytes, peak, alloc/free counts) fed by PROF_MEM_ALLOC /
    PROF_MEM_FREE at allocator call sites and sampled into dumps as counter tracks. At 0
    the macros vanish and the entry points become inert stubs (vtable shape unchanged).
==============================================================================================*/

/* INFO: Why compile-time levels AND a runtime switch.

   Instrumentation macros end up sprinkled through hot engine code, so there must be a way
   to make them cost literally zero in a shipping build: at level 0 the macros expand to
   ( (void)0 ) and the compiler deletes them -- no branch, no dead name string, nothing.
   The runtime switch (set_enabled) serves a different need: a dev build should idle cheaply
   (one predictable branch per event) and start recording the instant you ask, without a
   rebuild. Two gates, two costs, two audiences.

   Note what the level does NOT change: prof_api_t (the function-pointer table) always
   carries every entry point. Hot-reloaded DLLs compiled at different levels must still
   agree on that struct's layout, so only CALL SITES are gated, never the ABI.             */

#ifndef ORB_PROFILE
    #define ORB_PROFILE 2
#endif

#ifndef ORB_PROFILE_FAST
    #define ORB_PROFILE_FAST 0
#endif

#ifndef PROF_RING_RECYCLE
    #define PROF_RING_RECYCLE 1
#endif

#ifndef ORB_PROFILE_HITCH
    #define ORB_PROFILE_HITCH 1
#endif

#ifndef ORB_PROFILE_MEM
    #define ORB_PROFILE_MEM 1
#endif

/*==============================================================================================
    Limits

    All storage is static fixed pools (2 MB of rings in BSS at the defaults). Rings are
    claimed one per thread on first use; with PROF_RING_RECYCLE a thread that releases
    before exiting returns its slot to the pool, otherwise slots are claimed forever.
    Threads past PROF_MAX_THREADS fall back to a shared discard ring that only counts
    drops.
==============================================================================================*/

#define PROF_MAX_THREADS       16               // per-thread rings; extras discard        
#define PROF_RING_CAP          8192             // events per ring; must be a power of two 
#define PROF_MAX_NAMES         256              // distinct zone/counter names             
#define PROF_NAME_TABLE_SIZE   512              // id -> name hash table; power of two     
#define PROF_NAME_POOL_SIZE    ( 8 * 1024 )     // interned name string bytes              
#define PROF_MAX_COUNTERS      64               // distinct live counters
#define PROF_MAX_MEM_SCOPES    64               // distinct memory scopes (ORB_PROFILE_MEM)
#define PROF_THREAD_NAME_MAX   32               // thread label bytes, including NUL

/* Hitch capture (ORB_PROFILE_HITCH) -- overridable per build. */
#ifndef PROF_HITCH_HISTORY_CAP
    #define PROF_HITCH_HISTORY_CAP 8192         // rolling history events per thread; power of two
#endif
#ifndef PROF_HITCH_COOLDOWN_FRAMES
    #define PROF_HITCH_COOLDOWN_FRAMES 120      // frames suppressed after a capture fires
#endif

/* INFO: Why fixed static pools instead of malloc.

   Every byte the profiler will ever use is a global array sized at compile time (~2 MB,
   all in BSS, which occupies no space in the exe on disk). Reasons:
     - The hot path may never allocate: an allocator can take locks, fault pages, or call
       back into instrumented code (infinite recursion). Fixed pools bound every cost.
     - Failure becomes graceful degradation instead of a crash: too many threads -> shared
       discard ring that only counts; too many names -> capture still works, lookups just
       return NULL; ring full -> events dropped and counted.
     - A profiler must stay trustworthy while the engine is misbehaving -- that is exactly
       when you need it -- so it depends on nothing above the OS clock and its own arrays. */

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

/* INFO: Why the zone id IS a hash of the name.

   Every event must identify its zone in a fixed-size field. The alternatives and why they
   fail here:
     - Pointer to the string literal (Tracy's trick): free and unique, but this engine
       hot-reloads DLLs -- the literal's address dies with the old DLL image, and history
       recorded before a reload would dangle.
     - Sequential id from a registry: stable, but every call site then needs a handshake
       with the registry before it can emit anything.
   A hash of the name is stable across reloads, runs, machines, and modules -- two DLLs
   that both say "sim/update" agree on the id without ever coordinating. Matching sid_hash
   exactly is a bonus: profiler ids compare directly against engine string ids. The
   1-in-4-billion collision odds over a few hundred names are accepted on purpose.         */

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

/* INFO: Anatomy of the 16-byte event.

   16 bytes is a deliberate target: four events per 64-byte cache line, 128 KB per
   8192-event ring, and a power-of-two size so slot math is a shift. Note what is NOT
   stored:
     - no thread id     -- implied by WHICH ring the event sits in (one ring per thread)
     - no duration      -- computed later as END.tick_ns - BEGIN.tick_ns
     - no nesting depth -- implied by ORDER: on one thread zones form a proper stack
       (calls cannot return out of order), so replaying BEGIN/END like push/pop rebuilds
       the tree exactly; recording depth would pay bytes for redundant information.
   The timestamp is the expensive field: one QPC read (~20-40 ns) dominates the entire
   cost of emitting an event. Everything else is a handful of ordinary stores.             */

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

/* INFO: Counters vs zones.

   Zones answer "how LONG did this span take"; counters answer "how MUCH of something
   exists right now" (draw calls, live entities, bytes in flight). A value like that does
   not need an event trail -- only its latest value matters -- so counters skip the rings
   entirely and live in a small table the consumer snapshots once per frame. Writing one
   is a single atomic 64-bit store: the cheapest telemetry in the library.                 */

/*==============================================================================================
    Memory scope snapshot -- ORB_PROFILE_MEM

    One scope per allocator/arena name, updated from any thread by prof_mem_alloc/free at
    the allocator's own call sites; sampled by the consumer via prof()->mem_stats(). Hooks
    must be balanced by the caller -- a negative current truthfully reports an unbalanced
    pair, it is never clamped.
==============================================================================================*/

typedef struct prof_mem_s
{
    u32 id;         // scope name hash
    u32 _pad;       // reserved
    i64 current;    // live bytes right now
    i64 peak;       // high-water live bytes
    i64 allocs;     // allocation events
    i64 frees;      // free events
} prof_mem_t;

/* INFO: Why memory gets its own table instead of reusing counters.

   A counter could carry "bytes in use" -- but only its LATEST value: a consumer sampling
   once per frame would never see an allocation spike that rose and fell between samples,
   and could not tell 2 allocs from 2000 if the total came out even. The scope table
   updates AT THE ALLOCATION ITSELF, so peak (high-water) and alloc/free churn are exact
   no matter when anyone looks. That is the whole feature: three extra fields maintained
   at the source, recovering exactly the information sampling destroys. The hooks are
   allocator-agnostic on purpose -- an arena, a heap wrapper, or a pool all just call
   alloc/free with a scope name and a byte count.                                          */

/*==============================================================================================
    Ring storage -- INTERNAL

    Public only so the ORB_PROFILE_FAST inline path (prof_api.h) can write events without
    a call; treat as opaque everywhere else. Each ring is SPSC: the owning thread writes,
    the single drain consumer reads. Cursors are free-running modular u32 counters stored
    in volatile i32 (w - r is the pending count, correct across wrap).
==============================================================================================*/

#define PROF_RING_FREE      0    /* slot unclaimed, or released with nothing pending      */
#define PROF_RING_OWNED     1    /* a live thread is writing                              */
#define PROF_RING_RETIRED   2    /* owner released with events pending; drain frees it    */

typedef struct prof_ring_s
{
    volatile i32 write_pos;                     // modular cursor: total events written (owner thread)
    volatile i32 read_pos;                      // modular cursor: total events consumed (drain thread)
    volatile i32 dropped;                       // events lost to overflow / discard
    volatile i32 owner;                         // PROF_RING_* claim state (recycle path)
    bool         discard;                       // overflow-thread ring: count drops, store nothing

    char         label[ PROF_THREAD_NAME_MAX ]; // thread display name for readouts
    prof_event_t events[ PROF_RING_CAP ];

} prof_ring_t;

/* INFO: The SPSC ring, and why the cursors never wrap.

   SPSC = Single Producer, Single Consumer: exactly one thread appends (the ring's owner)
   and exactly one thread reads (the drain consumer). That pairing is what makes a
   lock-free queue SIMPLE: each cursor has exactly one writer, so the fast path has no
   compare-and-swap races at all -- just carefully ordered plain stores (see the publish
   protocol notes in prof.c).

   The cursors count events EVER written / EVER read, not slot indexes; the slot is the
   cursor masked with (PROF_RING_CAP - 1). Free-running unsigned counters make the math
   immune to overflow: write_pos - read_pos is the pending count even after either wraps
   past 2^32, because the difference of two unsigned counters is exact while they stay
   within one ring-capacity of each other. They are stored in volatile i32 only because
   the sys atomics API traffics in i32 -- the casts to u32 at every use are where the
   modular arithmetic actually happens.                                                    */

// clang-format on
/*============================================================================================*/
#endif    // PROF_H
