/*==============================================================================================

    game/framework/comp.c -- component registry and the query surface.

    A component TYPE is a ref-described POD struct; this file maps a ref type onto a pool
    and keeps the per-entity attach mask in sync.  All layout facts (size, schema hash)
    come from the reflection registry -- never from hand-written per-type code.

==============================================================================================*/

/*============================================================================================*/

static comp_id_t
comp_find_by_hash( world_t* w, u32 name_hash )
{
    for ( u32 i = 0; i < w->comp_count; i++ )
        if ( w->comps[ i ].name_hash == name_hash )
            return ( comp_id_t )i;
    return COMP_INVALID;
}

/*==============================================================================================
    Registration
==============================================================================================*/

comp_id_t
comp_register( world_id_t wid, const char* ref_type_name, u32 cap )
{
    world_t* w = world_resolve( wid );
    if ( !w || !ref_type_name )
        return COMP_INVALID;

    u16 tid = ref()->find_type_by_name( ref_type_name );
    if ( tid == REF_TYPE_INVALID )
    {
        LOG_ERROR( "comp_register: type '%s' not in ref registry", ref_type_name );
        return COMP_INVALID;
    }

    const ref_type_t* t    = ref()->get_type( tid );
    u32               hash = ref_hash_str( ref_type_name );

    /* Re-registration (reload / repeated init): re-resolve the type id, gate on schema. */
    comp_id_t existing = comp_find_by_hash( w, hash );
    if ( existing != COMP_INVALID )
    {
        world_comp_t* c = &w->comps[ existing ];
        c->type_id      = tid;

        if ( c->schema_hash != t->schema_hash )
        {
            if ( ( u32 )t->size * c->cap > c->data_bytes )
            {
                LOG_ERROR( "comp '%s': grew past its carved pool -- restart the host", ref_type_name );
                return COMP_INVALID;
            }
            LOG_WARN( "comp '%s': layout changed across reload -- pool wiped", ref_type_name );
            c->elem_size   = t->size;
            c->schema_hash = t->schema_hash;
            pool_clear( w, c );
            for ( u32 i = 1; i <= w->ent_high; i++ )
                w->ent_mask[ i ] &= ~( 1ull << existing );
        }
        return existing;
    }

    /* Fresh registration: carve sparse + dense + data from the world arena. */
    if ( w->comp_count >= WORLD_MAX_COMP )
    {
        LOG_ERROR( "comp_register: comp cap %u reached", ( u32 )WORLD_MAX_COMP );
        return COMP_INVALID;
    }
    if ( cap == 0 || t->size == 0 )
    {
        LOG_ERROR( "comp_register: '%s' zero cap or zero size", ref_type_name );
        return COMP_INVALID;
    }

    u32 sparse_off = pool_carve( w, ( WORLD_MAX_ENT + 1 ) * sizeof( u32 ) );
    u32 dense_off  = pool_carve( w, cap * sizeof( u32 ) );
    u32 data_bytes = cap * ( u32 )t->size;
    u32 data_off   = pool_carve( w, data_bytes );

    if ( sparse_off == WORLD_OFF_NONE || dense_off == WORLD_OFF_NONE || data_off == WORLD_OFF_NONE )
    {
        LOG_ERROR( "comp_register: '%s' pool arena exhausted", ref_type_name );
        return COMP_INVALID;
    }

    comp_id_t     id = ( comp_id_t )w->comp_count++;
    world_comp_t* c  = &w->comps[ id ];

    c->name_hash   = hash;
    c->name        = core()->sid_intern_cstr( ref_type_name );
    c->schema_hash = t->schema_hash;
    c->cap         = cap;
    c->count       = 0;
    c->type_id     = tid;
    c->elem_size   = t->size;
    c->data_bytes  = data_bytes;
    c->sparse_off  = sparse_off;
    c->dense_off   = dense_off;
    c->data_off    = data_off;

    pool_clear( w, c );    // sparse table starts as all WORLD_SPARSE_NONE

    LOG_INFO( "comp %d: '%s' size=%u cap=%u", id, ref_type_name, ( u32 )t->size, cap );
    return id;
}

comp_id_t
comp_find( world_id_t wid, const char* ref_type_name )
{
    world_t* w = world_resolve( wid );
    if ( !w || !ref_type_name )
        return COMP_INVALID;
    return comp_find_by_hash( w, ref_hash_str( ref_type_name ) );
}

/*==============================================================================================
    Attach / detach / access
==============================================================================================*/

void*
comp_attach( world_id_t wid, ent_t e, comp_id_t cid )
{
    world_t*      w = world_resolve( wid );
    world_comp_t* c = world_comp_resolve( w, cid );
    if ( !c || !ent_alive( wid, e ) )
        return NULL;

    void* data = pool_attach( w, c, e.index );
    if ( !data )
    {
        LOG_WARN( "comp_attach: '%s' pool full (cap %u)", core()->sid_cstr( c->name ), c->cap );
        return NULL;
    }

    w->ent_mask[ e.index ] |= ( 1ull << cid );
    return data;
}

void
comp_detach( world_id_t wid, ent_t e, comp_id_t cid )
{
    world_t*      w = world_resolve( wid );
    world_comp_t* c = world_comp_resolve( w, cid );
    if ( !c || !ent_alive( wid, e ) )
        return;

    if ( pool_detach( w, c, e.index ) )
        w->ent_mask[ e.index ] &= ~( 1ull << cid );
}

void*
comp_get( world_id_t wid, ent_t e, comp_id_t cid )
{
    world_t*      w = world_resolve( wid );
    world_comp_t* c = world_comp_resolve( w, cid );
    if ( !c || !ent_alive( wid, e ) )
        return NULL;
    return pool_get( w, c, e.index );
}

bool
comp_has( world_id_t wid, ent_t e, comp_id_t cid )
{
    world_t* w = world_resolve( wid );
    if ( !w || cid < 0 || !ent_alive( wid, e ) )
        return false;
    return ( w->ent_mask[ e.index ] & ( 1ull << cid ) ) != 0;
}

u32
comp_count( world_id_t wid, comp_id_t cid )
{
    world_t*      w = world_resolve( wid );
    world_comp_t* c = world_comp_resolve( w, cid );
    return c ? c->count : 0;
}

u16
comp_type_id( world_id_t wid, comp_id_t cid )
{
    world_t*      w = world_resolve( wid );
    world_comp_t* c = world_comp_resolve( w, cid );
    return c ? c->type_id : REF_TYPE_INVALID;
}

/*==============================================================================================
    Queries

    The cursor walks the dense prefix from the top down, so swap-remove of the CURRENT
    element (detach/destroy) moves an already-visited element into the vacated slot --
    nothing is skipped and nothing is visited twice.
==============================================================================================*/

world_iter_t
world_query( world_id_t wid, comp_id_t cid )
{
    world_iter_t  it = { .ent = ENT_INVALID, .data = NULL, .comp = COMP_INVALID, .slot = 0 };
    world_t*      w  = world_resolve( wid );
    world_comp_t* c  = world_comp_resolve( w, cid );
    if ( c )
    {
        it.comp = cid;
        it.slot = c->count;
    }
    return it;
}

bool
world_iter_next( world_id_t wid, world_iter_t* it )
{
    world_t*      w = world_resolve( wid );
    world_comp_t* c = world_comp_resolve( w, it->comp );
    if ( !c || it->slot == 0 )
        return false;

    if ( it->slot > c->count )    // pool shrank under the cursor (bulk detach): clamp
        it->slot = c->count;
    if ( it->slot == 0 )
        return false;

    it->slot--;
    u32 index = pool_dense( w, c )[ it->slot ];
    it->ent   = ( ent_t ){ index, w->ent_gen[ index ] };
    it->data  = pool_data( w, c, it->slot );
    return true;
}

/*============================================================================================*/
