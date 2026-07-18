/*==============================================================================================

    game/framework/pool.c -- dense component pool (sparse set) over the world arena.

    One pool per registered component type.  Three offset-addressed blocks in pool_mem:

        sparse[ent index] -> dense slot   (WORLD_SPARSE_NONE when not attached)
        dense[slot]       -> ent index    (the packed prefix: slots 0..count-1 are live)
        data[slot]        -> component    (elem_size bytes each, zeroed on attach)

    Removal is swap-with-last, so the live region stays packed and iteration is a flat
    linear walk.  Offsets, never pointers: the world block stays relocatable.

==============================================================================================*/

/*============================================================================================*/

u32
pool_carve( world_t* w, u32 bytes )
{
    u32 off = ALIGN_UP( w->pool_used, 16u );
    if ( off > WORLD_POOL_BYTES || bytes > WORLD_POOL_BYTES - off )
        return WORLD_OFF_NONE;
    w->pool_used = off + bytes;
    return off;
}

u32*
pool_sparse( world_t* w, world_comp_t* c )
{
    return ( u32* )( w->pool_mem + c->sparse_off );
}

u32*
pool_dense( world_t* w, world_comp_t* c )
{
    return ( u32* )( w->pool_mem + c->dense_off );
}

void*
pool_data( world_t* w, world_comp_t* c, u32 slot )
{
    return w->pool_mem + c->data_off + ( size_t )slot * c->elem_size;
}

/*============================================================================================*/

void*
pool_attach( world_t* w, world_comp_t* c, u32 ent_index )
{
    u32* sparse = pool_sparse( w, c );

    if ( sparse[ ent_index ] != WORLD_SPARSE_NONE )
        return pool_data( w, c, sparse[ ent_index ] );    // already attached: hand back existing data

    if ( c->count >= c->cap )
        return NULL;

    u32 slot                    = c->count++;
    sparse[ ent_index ]         = slot;
    pool_dense( w, c )[ slot ]  = ent_index;

    void* data = pool_data( w, c, slot );
    memset( data, 0, c->elem_size );
    return data;
}

bool
pool_detach( world_t* w, world_comp_t* c, u32 ent_index )
{
    u32* sparse = pool_sparse( w, c );
    u32  slot   = sparse[ ent_index ];

    if ( slot == WORLD_SPARSE_NONE )
        return false;

    /* Swap the last live element into the vacated slot to keep the prefix packed. */
    u32* dense = pool_dense( w, c );
    u32  last  = c->count - 1;

    if ( slot != last )
    {
        u32 moved_ent          = dense[ last ];
        dense[ slot ]          = moved_ent;
        sparse[ moved_ent ]    = slot;
        memcpy( pool_data( w, c, slot ), pool_data( w, c, last ), c->elem_size );
    }

    sparse[ ent_index ] = WORLD_SPARSE_NONE;
    c->count            = last;
    return true;
}

void*
pool_get( world_t* w, world_comp_t* c, u32 ent_index )
{
    u32 slot = pool_sparse( w, c )[ ent_index ];
    if ( slot == WORLD_SPARSE_NONE )
        return NULL;
    return pool_data( w, c, slot );
}

void
pool_clear( world_t* w, world_comp_t* c )
{
    u32* sparse = pool_sparse( w, c );
    for ( u32 i = 0; i <= WORLD_MAX_ENT; i++ )
        sparse[ i ] = WORLD_SPARSE_NONE;
    c->count = 0;
}

/*============================================================================================*/
