/*==============================================================================================

    runtime_service/gui/core/gui_state.c -- Keyed state pool: persistent per-id tracking.

    A pure tracking service: hand it an id, it hands back stable storage.  It does not know
    what it tracks -- animations (interact/gui_anim.c), scroll offsets, persisted child sizes,
    text-edit carets, and table column widths all rent slots from the same pool.  One-way:
    consumers depend on it, it depends on nothing above the context.

    gui_state_get hands back a stable, zero-on-create pointer to `size` bytes for `id`; the
    GUI_STATE( T, id ) sugar casts it to a typed struct.  Storage is three open-addressing
    hash tables in the bound context's retained store, picked by the requested size:

        tiny  (GUI_STATE_TINY_CAP bytes)  -- the hot one-or-two-word renters: anim dampers /
                                             timers, open flags, open_frame stamps.  16-byte
                                             slots, 4 per cache line -- probe walks stay dense.
        small (GUI_STATE_CAP bytes)       -- regions, scroll links, anim4, text-edit carets.
        big   (GUI_STATE_BIG_CAP bytes)   -- the rare large tenant (table persist, multiline).

    A tenant selects its class purely by sizeof, in gui_state_get AND gui_state_peek alike --
    grow a struct past a cap and it migrates classes on the next build with no call-site change
    (it re-zeroes there; the old slot goes cold and is reclaimed).  Peek must be given the same
    type's size as the get for the same id, which the GUI_STATE_PEEK sugar guarantees.

    The home bucket is picked by multiply-shift range reduction ((id * count) >> 32) and the
    probe wraps by increment, so table counts need NOT be powers of two -- the class partition
    is tunable per class.  A slot untouched for more than one frame is a tombstone the next
    insert on its chain reclaims; no sweep or free list is needed.

    Zero-on-create is part of the contract: a tenant's fields must make 0 the natural default
    (a "no sort" is 0, not -1), since a reclaimed or fresh slot always starts zeroed.

    Included by gui.c after core/gui_id.c (the identity service that mints the keys) --
    g_ctx / g_ctx->retained come from core/gui_ctx.c above both.

==============================================================================================*/
// clang-format off

ORB_STATIC_ASSERT( sizeof( gui_region_t ) <= GUI_STATE_CAP,
                   "gui_region_t is the small class's sizing tenant; grow GUI_STATE_CAP" );

/*============================================================================================*/
/* One class table: base array, slot stride, slot count.  Resolved from the requested size by
   state_class_for -- the single place the size -> class rule lives, shared by get and peek. */

typedef struct
{
    u8* base;
    u32 stride;
    u32 count;

} state_class_t;

static state_class_t
state_class_for( u32 size )
{
    if ( size <= GUI_STATE_TINY_CAP )
        return ( state_class_t ){ (u8*)g_ctx->retained.state_tiny,
                                  (u32)sizeof( gui_state_tiny_slot_t ), g_ctx->retained.tiny_count };
    if ( size <= GUI_STATE_CAP )
        return ( state_class_t ){ (u8*)g_ctx->retained.state,
                                  (u32)sizeof( gui_state_slot_t ),      g_ctx->retained.state_count };
    return     ( state_class_t ){ (u8*)g_ctx->retained.state_big,
                                  (u32)sizeof( gui_state_big_slot_t ),  g_ctx->retained.big_count };
}

/* Home bucket by multiply-shift range reduction: maps the full 32-bit id range uniformly onto
   [0, count) with one multiply -- no modulo, and count is free to be any size (the partition is
   not tied to powers of two).  Uses the id's high bits, which the FNV hash distributes well. */
static u32
state_bucket( gui_id_t id, u32 count )
{
    return (u32)( ( (u64)id * (u64)count ) >> 32 );
}

/*============================================================================================*/
/* The one probe, walked over any class.  Every slot type begins with the gui_state_hdr_t prefix
   (id, seen_frame); the payload follows it.  Live hit: restamp and return the payload.  Absent:
   settle into the first empty slot on the chain -- or a tombstone (two+ frames cold) passed on
   the way.  A wall-to-wall live table (no empty, no tombstone) clobbers the home bucket: a rare
   degradation rather than an overflow. */

static void*
state_probe( state_class_t c, gui_id_t id )
{
    u32 b      = state_bucket( id, c.count );
    u8* reuse  = NULL;   /* first tombstone (cold slot) seen on the chain */
    u8* dst    = NULL;   /* where a fresh entry lands when id is absent   */

    for ( u32 i = 0; i < c.count; ++i )
    {
        u8*              p = c.base + (size_t)b * c.stride;
        gui_state_hdr_t* h = (gui_state_hdr_t*)p;

        if ( h->id == id )                           /* live hit: restamp and hand back the storage */
        {
            h->seen_frame = g_ctx->retained.frame;
            return p + sizeof( gui_state_hdr_t );
        }
        if ( h->id == GUI_ID_NONE )                  /* empty ends the probe: id is absent */
        {
            dst = reuse ? reuse : p;                 /* reclaim a tombstone if we passed one, else grow */
            break;
        }
        if ( !reuse && g_ctx->retained.frame - h->seen_frame > 1u )
            reuse = p;                               /* two+ frames cold -> reclaimable in place */

        if ( ++b == c.count ) b = 0;                 /* wrap by increment (count is not a pow2) */
    }

    if ( !dst )
    {
        if ( !reuse )
        {
            /* Wall-to-wall live table: no empty slot, no tombstone -- the insert below clobbers
               the home bucket's live tenant.  From the user's side that is a scroll position or
               animation randomly resetting with nothing in the log, so warn once with the class
               (identified by stride) so the right GUI_STATE_*_SLOTS partition gets raised. */
            static bool warned = false;
            if ( !warned )
            {
                printf( "[gui] WARNING: keyed state pool full (class stride %u, %u slots) -- a "
                        "live entry was evicted; per-widget state (scroll/anim/caret) may reset. "
                        "Raise the class's slot count (gui_internal.h).\n", c.stride, c.count );
                fflush( stdout );
                warned = true;
            }
            ORB_ASSERT_MSG_ONCE( false, "gui keyed state pool full -- live state evicted; raise "
                                        "GUI_STATE_*_SLOTS (gui_internal.h)" );
        }
        dst = reuse ? reuse : c.base + (size_t)state_bucket( id, c.count ) * c.stride;
    }

    gui_state_hdr_t* h = (gui_state_hdr_t*)dst;
    h->id         = id;
    h->seen_frame = g_ctx->retained.frame;
    memset( dst + sizeof( gui_state_hdr_t ), 0, c.stride - sizeof( gui_state_hdr_t ) );
    return dst + sizeof( gui_state_hdr_t );
}

/*============================================================================================*/
/* Stable storage for `id`: the same pointer every frame the id stays live, zeroed the frame it is
   first seen or recycled.  `size` picks the class; it must fit GUI_STATE_BIG_CAP.  Never NULL. */

static void*
gui_state_get( gui_id_t id, u32 size )
{
    ORB_ASSERT( size <= GUI_STATE_BIG_CAP );
    if ( id == GUI_ID_NONE ) id = 1u;             /* never key on the empty sentinel */
    return state_probe( state_class_for( size ), id );
}

/*============================================================================================*/
/* Typed sugar: a zero-on-create T* persisted by id.  sizeof(T) must be <= GUI_STATE_BIG_CAP. */

#define GUI_STATE( T, id ) ( (T*)gui_state_get( ( id ), (u32)sizeof( T ) ) )

/*============================================================================================*/
/* Read-only, non-allocating, non-stamping probe for `id`.  `size` must be the same tenant size
   the gets for this id use (the GUI_STATE_PEEK sugar passes sizeof(T)) -- it picks the class,
   and a mismatched size probes the wrong table.  Returns a pointer to the slot's payload when
   the slot exists (regardless of freshness), else NULL.  Zero side effects -- safe to call on
   every widget every frame as a guard check. */

static const void*
gui_state_peek( gui_id_t id, u32 size )
{
    if ( id == GUI_ID_NONE ) id = 1u;
    state_class_t c = state_class_for( size );
    u32           b = state_bucket( id, c.count );
    for ( u32 i = 0; i < c.count; ++i )
    {
        const gui_state_hdr_t* h = (const gui_state_hdr_t*)( c.base + (size_t)b * c.stride );
        if ( h->id == id          ) return (const u8*)h + sizeof( gui_state_hdr_t );
        if ( h->id == GUI_ID_NONE ) return NULL;   /* empty slot ends the chain */
        if ( ++b == c.count ) b = 0;
    }
    return NULL;
}

#define GUI_STATE_PEEK( T, id ) ( (const T*)gui_state_peek( ( id ), (u32)sizeof( T ) ) )

/*============================================================================================*/
/* Pool load metric: live (touched this frame or last) and occupied (live + not-yet-reclaimed
   tombstones) slot counts per class.  Live is the real working set -- what the partition sizes
   should be judged against; occupied is how full the probe actually walks.  A full-table walk
   (~1K slots), so call it when displaying (the perf overlay), not unconditionally per frame. */

typedef struct
{
    u32 tiny_live,  tiny_used,  tiny_cap;
    u32 small_live, small_used, small_cap;
    u32 big_live,   big_used,   big_cap;

} gui_state_usage_t;

static void
state_count_class( state_class_t c, u32* out_live, u32* out_used )
{
    u32 live = 0, used = 0, frame = g_ctx->retained.frame;
    for ( u32 i = 0; i < c.count; ++i )
    {
        const gui_state_hdr_t* h = (const gui_state_hdr_t*)( c.base + (size_t)i * c.stride );
        if ( h->id == GUI_ID_NONE ) continue;
        ++used;
        if ( frame - h->seen_frame <= 1u ) ++live;
    }
    *out_live = live;
    *out_used = used;
}

static gui_state_usage_t
gui_state_usage( void )
{
    gui_state_usage_t u;
    state_class_t     t = state_class_for( GUI_STATE_TINY_CAP );
    state_class_t     s = state_class_for( GUI_STATE_CAP );
    state_class_t     g = state_class_for( GUI_STATE_BIG_CAP );
    state_count_class( t, &u.tiny_live,  &u.tiny_used  );  u.tiny_cap  = t.count;
    state_count_class( s, &u.small_live, &u.small_used );  u.small_cap = s.count;
    state_count_class( g, &u.big_live,   &u.big_used   );  u.big_cap   = g.count;
    return u;
}

/*============================================================================================*/
/* Animation utilities (gui_anim_f32, ...) live in interact/gui_anim.c,
   included after present/gui_paint_core.c which provides the color palette they blend. */

// clang-format on
/*============================================================================================*/
