/*==============================================================================================

    runtime_service/imgui/imgui_ctx_id.c -- ID hashing, combining, and scope stack.

    Three utilities the rest of the widget tree is built on:

        id_hash    -- FNV-1a 32-bit hash of a NUL-terminated string, salted per context
                      so identical labels in different contexts never collide.
        id_combine -- mix a scope seed + a local key into one stable id (boost hash_combine).
        id stack   -- the push/pop scope stack that widget ids combine against; regions seed
                      it automatically, push_id/pop_id add temporary levels for repeated rows.

    imgui_state_get and the IMGUI_STATE macro live here too: the keyed per-widget state pool
    (a slot table in the bound context's retained store) is the natural companion to the id
    system that keys it, and it has no other cross-cutting dependencies.

    Included by imgui.c after imgui_ctx.c, which defines g_ctx (needed for s_retained /
    imgui_context_t) and the id-stack variables (s_id_stack[], s_id_sp) referenced below.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    id_hash -- FNV-1a 32-bit hash of a NUL-terminated string
----------------------------------------------------------------------------------------------*/

static imgui_id_t
id_hash( const char* str )
{
    /* Seed FNV-1a with the context's id salt so the same string hashes to a distinct id per context.
       s_retained.id_salt is 0 for the default context -> the standard 0x811C9DC5 basis -> ids are
       byte-identical to the unsalted hash, so single-context behavior is unchanged. */
    u32 h = 0x811C9DC5u ^ s_retained.id_salt;
    for ( ; *str; ++str )
        h = ( h ^ (u8)*str ) * 0x01000193u;
    return h ? h : 1u;    /* never return IMGUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    id_combine -- mix a scope seed with a local key into one id (boost-style hash_combine).

    The single rule for how an id is namespaced: every sub-id (a leaf widget under a region, a
    child region under its parent, a window's chrome control) is id_combine(scope, key).  Unlike
    a bare XOR it avalanches and is order-dependent, so distinct (scope, key) pairs stay distinct.
----------------------------------------------------------------------------------------------*/

static imgui_id_t
id_combine( imgui_id_t seed, u32 key )
{
    u32 h = seed ^ ( key + 0x9E3779B9u + ( seed << 6 ) + ( seed >> 2 ) );
    return h ? h : 1u;    /* never return IMGUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    Id-scope stack functions

    id_seed/push/pop operate on the s_id_stack[] / s_id_sp variables declared in imgui_ctx.c.
    The top of the stack is the seed every widget id combines against; regions seed it
    automatically, and push_id / pop_id add temporary levels for repeated widgets in one region.
----------------------------------------------------------------------------------------------*/

/* Current scope seed -- top of the stack, or NONE when empty (a bare top-level widget). */
static imgui_id_t
id_seed( void )
{
    if ( s_id_sp == 0 ) return IMGUI_ID_NONE;
    u32  i = s_id_sp - 1;
    if ( i >= IMGUI_ID_STACK_DEPTH ) i = IMGUI_ID_STACK_DEPTH - 1;
    return s_id_stack[ i ];
}

static void
id_push( imgui_id_t id )
{
    if ( s_id_sp < IMGUI_ID_STACK_DEPTH )
        s_id_stack[ s_id_sp ] = id;
    ++s_id_sp;    /* count truthfully so push/pop stay paired even past the cap */
}

static void
id_pop( void )
{
    if ( s_id_sp ) --s_id_sp;
}

/*----------------------------------------------------------------------------------------------
    Keyed state pool -- persistent per-id widget state.

    imgui_state_get hands back a stable, zero-on-create pointer to `size` bytes for `id`; the
    IMGUI_STATE( T, id ) sugar casts it to a typed struct.  Storage is an open-addressing hash
    table in the bound context's retained store.  A slot untouched for more than one frame is
    a tombstone the next insert on its chain reclaims; no sweep or free list is needed.
----------------------------------------------------------------------------------------------*/

/* Stable storage for `id`: the same pointer every frame the id stays live, zeroed the frame it is
   first seen or recycled.  size must fit IMGUI_STATE_CAP.  Never returns NULL. */
static void*
imgui_state_get( imgui_id_t id, u32 size )
{
    ORB_ASSERT( size <= IMGUI_STATE_CAP );
    if ( id == IMGUI_ID_NONE ) id = 1u;             /* never key on the empty sentinel */
    (void)size;

    u32                 bucket = id & IMGUI_STATE_MASK;
    imgui_state_slot_t* reuse  = NULL;               /* first tombstone (cold slot) seen on the chain */
    imgui_state_slot_t* dst    = NULL;               /* where a fresh entry lands when id is absent */

    for ( u32 i = 0; i < IMGUI_STATE_SLOTS; ++i )
    {
        imgui_state_slot_t* s = &s_retained.state[ ( bucket + i ) & IMGUI_STATE_MASK ];

        if ( s->id == id )                           /* live hit: restamp and hand back the storage */
        {
            s->seen_frame = s_retained.frame;
            return s->data.bytes;
        }
        if ( s->id == IMGUI_ID_NONE )                /* empty ends the probe: id is absent */
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
    memset( dst->data.bytes, 0, sizeof dst->data.bytes );
    return dst->data.bytes;
}

/* Typed sugar: a zero-on-create T* persisted by id.  sizeof(T) must be <= IMGUI_STATE_CAP. */
#define IMGUI_STATE( T, id ) ( (T*)imgui_state_get( ( id ), (u32)sizeof( T ) ) )

/* Read-only, non-allocating, non-stamping probe for `id`.  Returns a pointer to the slot's
   data payload when the slot exists in the pool (regardless of freshness), else NULL.
   Zero side effects -- safe to call on every widget every frame as a guard check. */
static const void*
imgui_state_peek( imgui_id_t id )
{
    if ( id == IMGUI_ID_NONE ) id = 1u;
    u32 bucket = id & IMGUI_STATE_MASK;
    for ( u32 i = 0; i < IMGUI_STATE_SLOTS; ++i )
    {
        const imgui_state_slot_t* s = &s_retained.state[ ( bucket + i ) & IMGUI_STATE_MASK ];
        if ( s->id == id          ) return s->data.bytes;
        if ( s->id == IMGUI_ID_NONE ) return NULL;   /* empty slot ends the chain */
    }
    return NULL;
}

/* Animation utilities (imgui_anim_f32, imgui_anim_bg, ...) live in imgui_anim.c,
   included after imgui_widget_core.c which provides the color palette they blend. */

// clang-format on
/*============================================================================================*/
