/*==============================================================================================

    runtime_service/gui/style/gui_style_block.c -- the style block registry: store and work.

    The neutral BACKEND under every style value.  It knows nothing about colors, metrics, or
    chrome: a block is a named run of u32 slots with a fill function, and the registry hands
    back a base.  Everything with a slot vocabulary (chrome's theme, a kit's own style set, a
    kit's private set) registers one and reads through its base -- which is what lets two
    schemas, or two INSTANCES of one schema, sit in the same space without colliding.

    TWO arrays, and the split is the whole design:

        s_store -- the INSTALLED data.  Every block, every instance.  Written only by a block's
                   install fn; never by a push.  This is the "what the theme / kit said" layer.
        s_work  -- the WORKING SET.  ONE run per block: the CURRENT instance, with push/pop and
                   next-item overrides already applied.  This is the only array a read touches,
                   so a resolved read is a single indexed load with no scan and no fallback
                   chain.  Overrides cost at the WRITE, reads stay flat.

    Instances: a block reserves count * instances slots in the store, instance i at
    store_base + i * count.  It mirrors exactly one of them into its work run at a time
    (`current`).  Today every block is single-instance and `current` is always 0; the layout
    arithmetic is here from the start so bases never have to be recomputed when the element
    schema grows instances and the set stack switches between them.

    Included by gui_style.c after gui_theme.c (blocks fill from theme state) and before
    gui_style_core.c (which registers the concrete blocks and reads through their bases).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Capacities.  The store carries every slot of every instance, so it scales with
    GUI_STYLE_SET_MAX: one gui_style_t is ~50 slots, so the four built-in sets alone are ~200.
    The work set only ever holds one instance per block, so it stays a fraction of the store.
    Both are asserted at register time -- an overflow is an authoring error, not a runtime
    condition, but it fires on the first frame rather than at compile time, so the store is
    sized with real headroom for another block (a kit's own) and a wider schema.
==============================================================================================*/

#define STYLE_BLOCK_MAX  8      // registered blocks
#define STYLE_STORE_MAX  1024   // installed slots: all blocks, all instances
#define STYLE_WORK_MAX   256    // working slots: all blocks, current instance only

/* Refill one instance's run of the store.  Called for every instance at every store refill, so
   a block re-derives its installed values instead of being clobbered.  `instance` is which run
   dst points at: a multi-instance block (the element schema, one instance per style set) fills
   each from a different owner. */
typedef void ( *style_install_fn )( void* user, u32* dst, u16 count, u16 instance );

/* WHEN a block re-derives.  The distinction is about where the block's data comes from, and it
   is load-bearing: a block whose installed values can be POKED between landings (the element
   style, through gui_style_edit()) must not be refilled per frame or the poke dies immediately;
   a block that mirrors live state (s_style, written by set_check_style with no apply) must be,
   or the write never lands. */
typedef enum
{
    STYLE_REFILL_LANDING = 0,   // theme / font landings only -- pokes survive between them
    STYLE_REFILL_FRAME          // every frame as well -- the block mirrors live state

} style_refill_t;

typedef struct style_block_desc_t
{
    const char*      name;        // diagnostic only -- the registry never keys on it
    u16              count;       // slots per instance
    u16              instances;   // 1 for a single-instance block
    u8               refill;      // style_refill_t
    style_install_fn install;     // fills one instance's run of the store
    void*            user;        // passed back to install

} style_block_desc_t;

typedef struct style_block_t
{
    const char*      name;        //
    style_install_fn install;     //
    void*            user;        //
    u16              store_base;  // first slot in s_store (instance i at + i * count)
    u16              work_base;   // first slot in s_work -- the base every read adds
    u16              count;       // slots per instance
    u16              instances;   //
    u16              current;     // which instance is mirrored into the work run
    u8               refill;      // style_refill_t

} style_block_t;

static style_block_t s_block[ STYLE_BLOCK_MAX ];
static u16           s_block_n;
static u16           s_store_used;
static u16           s_work_used;

static u32 s_store[ STYLE_STORE_MAX ];   // installed values
static u32 s_work [ STYLE_WORK_MAX  ];   // resolved working set -- what reads index

/*==============================================================================================
    Registration -- carve the two bases out of the store and the work set.

    Returns the block handle (an index).  The caller keeps the WORK base for its read accessor;
    that base is the only number a read site ever adds, and it is stable for the process.
==============================================================================================*/

static u16
style_block_register( const style_block_desc_t* d )
{
    u16 instances = ( d->instances > 0 ) ? d->instances : 1u;

    ORB_ASSERT( s_block_n < STYLE_BLOCK_MAX && "style block registry full" );
    ORB_ASSERT( s_store_used + d->count * instances <= STYLE_STORE_MAX && "style store full" );
    ORB_ASSERT( s_work_used  + d->count             <= STYLE_WORK_MAX  && "style work set full" );

    style_block_t* b = &s_block[ s_block_n ];

    b->name       = d->name;
    b->install    = d->install;
    b->user       = d->user;
    b->count      = d->count;
    b->instances  = instances;
    b->current    = 0;
    b->refill     = d->refill;
    b->store_base = s_store_used;
    b->work_base  = s_work_used;

    s_store_used = (u16)( s_store_used + d->count * instances );
    s_work_used  = (u16)( s_work_used  + d->count );

    return s_block_n++;
}

/* The work base of a registered block -- what a read accessor adds to its local slot index. */
static u16
style_block_work_base( u16 blk )
{
    return s_block[ blk ].work_base;
}

/* The store run of one instance -- the INSTALLED values, writable.  The door a style source
   (and gui_style_edit) writes its look through; reads go to the work set instead. */
static u32*
style_block_instance( u16 blk, u16 inst )
{
    const style_block_t* b = &s_block[ blk ];
    if ( inst >= b->instances ) inst = 0;
    return &s_store[ b->store_base + inst * b->count ];
}

static u16  style_block_current  ( u16 blk )            { return s_block[ blk ].current; }
static u16  style_block_instances( u16 blk )            { return s_block[ blk ].instances; }
static void style_block_set_current( u16 blk, u16 inst )
{
    if ( inst < s_block[ blk ].instances ) s_block[ blk ].current = inst;
}

/* Is an absolute work slot inside this block's run?  The filter a single-block reseed uses to
   decide which live overrides have to be re-applied over the freshly mirrored values. */
static bool
style_slot_in_block( u16 slot, u16 blk )
{
    const style_block_t* b = &s_block[ blk ];
    return slot >= b->work_base && slot < (u16)( b->work_base + b->count );
}

/*==============================================================================================
    The two refresh steps.

    store_refill -- run install fns: the installed layer re-derives from whatever it sources.
                    `landing` true runs every block (a theme / font change); false runs only
                    the STYLE_REFILL_FRAME blocks, leaving poke-able installed data alone.
    work_reseed  -- mirror each block's CURRENT instance into its work run, discarding whatever
                    overrides had been written over it.  Callers re-apply live overrides after.
==============================================================================================*/

static void
style_store_refill( bool landing )
{
    for ( u16 i = 0; i < s_block_n; ++i )
    {
        const style_block_t* b = &s_block[ i ];
        if ( !b->install ) continue;
        if ( !landing && b->refill != STYLE_REFILL_FRAME ) continue;

        for ( u16 inst = 0; inst < b->instances; ++inst )
            b->install( b->user, &s_store[ b->store_base + inst * b->count ], b->count, inst );
    }
}

/* Mirror ONE block's current instance into its work run.  The set-switch primitive: swapping
   which instance a block resolves through is this copy plus a replay of the live overrides. */
static void
style_block_reseed( u16 blk )
{
    const style_block_t* b = &s_block[ blk ];
    const u32*           src = &s_store[ b->store_base + b->current * b->count ];

    for ( u16 s = 0; s < b->count; ++s ) s_work[ b->work_base + s ] = src[ s ];
}

static void
style_work_reseed( void )
{
    for ( u16 i = 0; i < s_block_n; ++i ) style_block_reseed( i );
}

// clang-format on
/*============================================================================================*/
