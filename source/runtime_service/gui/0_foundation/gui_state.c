/*==============================================================================================

    runtime_service/gui/0_foundation/gui_state.c -- Keyed state pool: persistent per-id tracking.

    A pure tracking service: hand it an id, it hands back stable storage.  It does not know
    what it tracks -- animations (2_interact/gui_anim.c), scroll offsets, persisted child sizes,
    text-edit carets, and table column widths all rent slots from the same pool.  One-way:
    consumers depend on it, it depends on nothing above the context.

    gui_state_get hands back a stable, zero-on-create pointer to `size` bytes for `id`; the
    GUI_STATE( T, id ) sugar casts it to a typed struct.  Storage is two open-addressing hash
    tables in the bound context's retained store -- a small class (GUI_STATE_CAP bytes, the
    common case) and a big class (GUI_STATE_BIG_CAP bytes, the rare large tenant) -- picked by
    the requested size and walked by the one probe below.  A slot untouched for more than one
    frame is a tombstone the next insert on its chain reclaims; no sweep or free list is needed.

    Zero-on-create is part of the contract: a tenant's fields must make 0 the natural default
    (a "no sort" is 0, not -1), since a reclaimed or fresh slot always starts zeroed.

    Included by gui.c after 0_foundation/gui_id.c (the identity service that mints the keys) --
    g_ctx / s_retained come from 0_foundation/gui_ctx.c above both.

==============================================================================================*/
// clang-format off

ORB_STATIC_ASSERT( sizeof( gui_region_t ) <= GUI_STATE_CAP,
                   "gui_region_t is the small class's sizing tenant; grow GUI_STATE_CAP" );

/* The one probe, walked over either class: `base` is the slot array, `stride` the slot size in
   bytes.  Both slot types begin with the gui_state_hdr_t prefix (id, seen_frame); the payload
   follows it.  Live hit: restamp and return the payload.  Absent: settle into the first empty
   slot on the chain -- or a tombstone (two+ frames cold) passed on the way.  A wall-to-wall
   live table (no empty, no tombstone) clobbers the home bucket: a rare degradation rather than
   an overflow. */
static void*
state_probe( u8* base, u32 stride, u32 count, u32 mask, gui_id_t id )
{
    u32 bucket = id & mask;
    u8* reuse  = NULL;   /* first tombstone (cold slot) seen on the chain */
    u8* dst    = NULL;   /* where a fresh entry lands when id is absent   */

    for ( u32 i = 0; i < count; ++i )
    {
        u8*              p = base + (size_t)( ( bucket + i ) & mask ) * stride;
        gui_state_hdr_t* h = (gui_state_hdr_t*)p;

        if ( h->id == id )                           /* live hit: restamp and hand back the storage */
        {
            h->seen_frame = s_retained.frame;
            return p + sizeof( gui_state_hdr_t );
        }
        if ( h->id == GUI_ID_NONE )                  /* empty ends the probe: id is absent */
        {
            dst = reuse ? reuse : p;                 /* reclaim a tombstone if we passed one, else grow */
            break;
        }
        if ( !reuse && s_retained.frame - h->seen_frame > 1u )
            reuse = p;                               /* two+ frames cold -> reclaimable in place */
    }

    if ( !dst ) dst = reuse ? reuse : base + (size_t)bucket * stride;

    gui_state_hdr_t* h = (gui_state_hdr_t*)dst;
    h->id         = id;
    h->seen_frame = s_retained.frame;
    memset( dst + sizeof( gui_state_hdr_t ), 0, stride - sizeof( gui_state_hdr_t ) );
    return dst + sizeof( gui_state_hdr_t );
}

/* Stable storage for `id`: the same pointer every frame the id stays live, zeroed the frame it is
   first seen or recycled.  `size` picks the class; it must fit GUI_STATE_BIG_CAP.  Never NULL. */
static void*
gui_state_get( gui_id_t id, u32 size )
{
    ORB_ASSERT( size <= GUI_STATE_BIG_CAP );
    if ( id == GUI_ID_NONE ) id = 1u;             /* never key on the empty sentinel */

    if ( size <= GUI_STATE_CAP )
        return state_probe( (u8*)s_retained.state, (u32)sizeof( gui_state_slot_t ),
                            s_retained.state_count, s_retained.state_mask, id );

    return state_probe( (u8*)s_retained.state_big, (u32)sizeof( gui_state_big_slot_t ),
                        GUI_STATE_BIG_SLOTS, s_retained.big_mask, id );
}

/* Typed sugar: a zero-on-create T* persisted by id.  sizeof(T) must be <= GUI_STATE_BIG_CAP. */
#define GUI_STATE( T, id ) ( (T*)gui_state_get( ( id ), (u32)sizeof( T ) ) )

/* Read-only, non-allocating, non-stamping probe for `id` in the SMALL class (every current
   caller peeks a small tenant -- the animation types).  Returns a pointer to the slot's data
   payload when the slot exists in the pool (regardless of freshness), else NULL.
   Zero side effects -- safe to call on every widget every frame as a guard check. */
static const void*
gui_state_peek( gui_id_t id )
{
    if ( id == GUI_ID_NONE ) id = 1u;
    u32 bucket = id & s_retained.state_mask;
    for ( u32 i = 0; i < s_retained.state_count; ++i )
    {
        const gui_state_slot_t* s = &s_retained.state[ ( bucket + i ) & s_retained.state_mask ];
        if ( s->id == id          ) return s->data;
        if ( s->id == GUI_ID_NONE ) return NULL;   /* empty slot ends the chain */
    }
    return NULL;
}

/* Animation utilities (gui_anim_f32, ...) live in 2_interact/gui_anim.c,
   included after 2_present/gui_widget_core.c which provides the color palette they blend. */

// clang-format on
/*============================================================================================*/
