/*==============================================================================================

    runtime_service/gui/style/gui_style_block.c -- the style block registry: store and work.

    The neutral BACKEND under every style value.  It knows nothing about colors, metrics, or
    chrome: a block is a named run of u32 slots with a fill function, and the registry hands
    back a base.  Everything with a slot vocabulary (chrome's palette, the element stratum, a
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
    Capacities.  The store is sized for the split ahead (an instanced element schema plus
    chrome's tokens, its vars, and a kit block or two); the work set only ever holds one
    instance per block, so it stays a fraction of the store.  Both are asserted at register
    time -- an overflow is a build-time authoring error, not a runtime condition.
==============================================================================================*/

#define STYLE_BLOCK_MAX  8      // registered blocks
#define STYLE_STORE_MAX  256    // installed slots: all blocks, all instances
#define STYLE_WORK_MAX   128    // working slots: all blocks, current instance only

/* Refill one instance's run of the store.  Called for every instance at every store refill,
   so a block re-derives its installed values instead of being clobbered.  (Block-level today
   because every block is single-instance; the per-instance sources a kit registers arrive
   with the set stack, and the fn moves onto the instance record then.) */
typedef void ( *style_install_fn )( void* user, u32* dst, u16 count );

typedef struct style_block_desc_t
{
    const char*      name;        // diagnostic only -- the registry never keys on it
    u16              count;       // slots per instance
    u16              instances;   // 1 for a single-instance block
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

/*==============================================================================================
    The two refresh steps.

    store_refill -- run every install fn: the installed layer re-derives from whatever it
                    sources (the active theme today, a kit's palette after the split).
    work_reseed  -- mirror each block's CURRENT instance into its work run, discarding whatever
                    overrides had been written over it.  Callers re-apply live overrides after.
==============================================================================================*/

static void
style_store_refill( void )
{
    for ( u16 i = 0; i < s_block_n; ++i )
    {
        const style_block_t* b = &s_block[ i ];
        if ( !b->install ) continue;

        for ( u16 inst = 0; inst < b->instances; ++inst )
            b->install( b->user, &s_store[ b->store_base + inst * b->count ], b->count );
    }
}

static void
style_work_reseed( void )
{
    for ( u16 i = 0; i < s_block_n; ++i )
    {
        const style_block_t* b = &s_block[ i ];
        for ( u16 s = 0; s < b->count; ++s )
            s_work[ b->work_base + s ] = s_store[ b->store_base + b->current * b->count + s ];
    }
}

// clang-format on
/*============================================================================================*/
