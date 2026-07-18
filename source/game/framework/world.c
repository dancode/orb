/*==============================================================================================

    game/framework/world.c -- bound-world table, entity allocation, generation recycling.

    The framework owns no memory: owners embed world_t (in module state, so it survives
    hot-reload) and bind it into a slot.  The slot table itself is DLL-static and does NOT
    survive a reload -- which is exactly why world_bind must run in init() AND reload().

==============================================================================================*/

/*============================================================================================*/

static world_t* s_worlds[ WORLD_MAX_WORLDS ];    // rebound by owners after every reload

world_t*
world_resolve( world_id_t id )
{
    if ( id < 0 || id >= WORLD_MAX_WORLDS )
        return NULL;
    return s_worlds[ id ];
}

world_comp_t*
world_comp_resolve( world_t* w, comp_id_t c )
{
    if ( !w || c < 0 || ( u32 )c >= w->comp_count )
        return NULL;
    return &w->comps[ c ];
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

world_id_t
world_bind( i32 slot, world_t* storage )
{
    if ( slot < 0 || slot >= WORLD_MAX_WORLDS || !storage )
    {
        LOG_ERROR( "world_bind: bad slot %d", slot );
        return WORLD_INVALID;
    }

    s_worlds[ slot ] = storage;

    if ( storage->magic != WORLD_MAGIC )
    {
        /* Fresh storage: a zeroed block is almost a valid empty world -- only the sparse
           tables (carved later by comp_register) need non-zero init.  Stamp and go. */
        memset( storage, 0, sizeof( *storage ) );
        storage->magic = WORLD_MAGIC;
        LOG_INFO( "world %d: fresh (%u KB block)", slot, ( u32 )( sizeof( world_t ) / 1024u ) );
    }
    else
    {
        /* Rebind after hot-reload: data is intact; ref type ids go stale and are
           re-resolved by the owner's comp_register calls that follow this bind. */
        LOG_INFO( "world %d: rebound (%u entities, %u comps)", slot, storage->ent_count, storage->comp_count );
    }

    return ( world_id_t )slot;
}

void
world_reset( world_id_t wid )
{
    world_t* w = world_resolve( wid );
    if ( !w )
        return;

    /* Bump generations on every slot ever used so outstanding handles die; registrations
       and carved pools survive, ready for the next population (scene load). */
    for ( u32 i = 1; i <= w->ent_high; i++ )
        w->ent_gen[ i ]++;

    memset( w->ent_live, 0, sizeof( w->ent_live ) );
    memset( w->ent_mask, 0, sizeof( w->ent_mask ) );
    w->ent_count      = 0;
    w->ent_high       = 0;
    w->ent_free_count = 0;

    for ( u32 c = 0; c < w->comp_count; c++ )
        pool_clear( w, &w->comps[ c ] );
}

u32
world_ent_count( world_id_t wid )
{
    world_t* w = world_resolve( wid );
    return w ? w->ent_count : 0;
}

/*==============================================================================================
    Entities
==============================================================================================*/

ent_t
ent_create( world_id_t wid )
{
    world_t* w = world_resolve( wid );
    if ( !w )
        return ENT_INVALID;

    u32 index;
    if ( w->ent_free_count > 0 )
        index = w->ent_free[ --w->ent_free_count ];
    else if ( w->ent_high < WORLD_MAX_ENT )
        index = ++w->ent_high;
    else
    {
        LOG_WARN( "world %d: entity cap %u reached", wid, ( u32 )WORLD_MAX_ENT );
        return ENT_INVALID;
    }

    w->ent_live[ index ] = 1;
    w->ent_mask[ index ] = 0;
    w->ent_count++;

    return ( ent_t ){ index, w->ent_gen[ index ] };
}

bool
ent_alive( world_id_t wid, ent_t e )
{
    world_t* w = world_resolve( wid );
    if ( !w || e.index == 0 || e.index > w->ent_high )
        return false;
    return w->ent_live[ e.index ] && w->ent_gen[ e.index ] == e.gen;
}

void
ent_destroy( world_id_t wid, ent_t e )
{
    world_t* w = world_resolve( wid );
    if ( !w || !ent_alive( wid, e ) )
        return;

    /* Detach every attached component (mask walk), then recycle the slot. */
    u64 mask = w->ent_mask[ e.index ];
    for ( u32 c = 0; mask != 0 && c < w->comp_count; c++ )
    {
        if ( mask & ( 1ull << c ) )
        {
            pool_detach( w, &w->comps[ c ], e.index );
            mask &= ~( 1ull << c );
        }
    }

    w->ent_mask[ e.index ] = 0;
    w->ent_live[ e.index ] = 0;
    w->ent_gen[ e.index ]++;    // outstanding copies of this handle are now stale
    w->ent_free[ w->ent_free_count++ ] = e.index;
    w->ent_count--;
}

/*============================================================================================*/
