/*==============================================================================================

    runtime_service/gui/0_foundation/gui_state.c -- Keyed state pool: persistent per-id tracking.

    A pure tracking service: hand it an id, it hands back stable storage.  It does not know
    what it tracks -- animations (2_interact/gui_anim.c), scroll offsets, persisted child sizes,
    and text-edit carets all rent slots from the same pool.  One-way: consumers depend on it,
    it depends on nothing above the context.

    gui_state_get hands back a stable, zero-on-create pointer to `size` bytes for `id`; the
    GUI_STATE( T, id ) sugar casts it to a typed struct.  Storage is an open-addressing hash
    table in the bound context's retained store.  A slot untouched for more than one frame is
    a tombstone the next insert on its chain reclaims; no sweep or free list is needed.

    Included by gui.c after 0_foundation/gui_id.c (the identity service that mints the keys) --
    g_ctx / s_retained come from 0_foundation/gui_ctx.c above both.

==============================================================================================*/
// clang-format off

/* Stable storage for `id`: the same pointer every frame the id stays live, zeroed the frame it is
   first seen or recycled.  size must fit GUI_STATE_CAP.  Never returns NULL. */
static void*
gui_state_get( gui_id_t id, u32 size )
{
    ORB_ASSERT( size <= GUI_STATE_CAP );
    if ( id == GUI_ID_NONE ) id = 1u;             /* never key on the empty sentinel */
    (void)size;

    u32               bucket = id & s_retained.state_mask;
    gui_state_slot_t* reuse  = NULL;               /* first tombstone (cold slot) seen on the chain */
    gui_state_slot_t* dst    = NULL;               /* where a fresh entry lands when id is absent */

    for ( u32 i = 0; i < s_retained.state_count; ++i )
    {
        gui_state_slot_t* s = &s_retained.state[ ( bucket + i ) & s_retained.state_mask ];

        if ( s->id == id )                           /* live hit: restamp and hand back the storage */
        {
            s->seen_frame = s_retained.frame;
            return s->data;
        }
        if ( s->id == GUI_ID_NONE )                /* empty ends the probe: id is absent */
        {
            dst = reuse ? reuse : s;                 /* reclaim a tombstone if we passed one, else grow */
            break;
        }
        if ( !reuse && s_retained.frame - s->seen_frame > 1u )
            reuse = s;                               /* two+ frames cold -> reclaimable in place */
    }

    /* Absent: settle into dst.  If the table is wall-to-wall live entries (no empty slot and no
       tombstone -- 512 distinct persistent widgets in one frame), clobber the home bucket: a rare
       degradation rather than an overflow.  reuse covers the no-empty-but-some-cold case. */
    if ( !dst ) dst = reuse ? reuse : &s_retained.state[ bucket ];

    dst->id         = id;
    dst->seen_frame = s_retained.frame;
    memset( dst->data, 0, sizeof dst->data );
    return dst->data;
}

/* Typed sugar: a zero-on-create T* persisted by id.  sizeof(T) must be <= GUI_STATE_CAP. */
#define GUI_STATE( T, id ) ( (T*)gui_state_get( ( id ), (u32)sizeof( T ) ) )

/* Read-only, non-allocating, non-stamping probe for `id`.  Returns a pointer to the slot's
   data payload when the slot exists in the pool (regardless of freshness), else NULL.
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

/* Animation utilities (gui_anim_f32, gui_anim_bg, ...) live in 2_interact/gui_anim.c,
   included after 2_present/gui_widget_core.c which provides the color palette they blend. */

// clang-format on
/*============================================================================================*/
