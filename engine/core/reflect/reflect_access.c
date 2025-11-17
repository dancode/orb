/*==============================================================================================

    reflect_access.c

==============================================================================================*/
/*==============================================================================================

    reflect_access.c : type accessors

==============================================================================================*/

const rf_type_t*
rf_get_type( uint32_t id )
{
    /* get type by id */

    if ( id == TYPE_INVALID || id >= g_registry.type_count )
    {
        // lets find when this actually happens
        assert( 0 && "Invalid type id" ); 

        // return the 'invalid type' for out-of-bounds
        // we want code that operates on types to always have a valid pointer
        // so we don't have to null check constantly on every return value.
        return &g_registry.type_array[ 0 ];    // default invalid type
    }

    return &g_registry.type_array[ id ];    // valid type
}

uint16_t
rf_get_type_id( const rf_type_t* type )
{
    /* get type id by pointer */
    if ( !type )
    {
        assert( 0 && "Invalid type pointer (NULL)" );
        return TYPE_INVALID;
    }

    ptrdiff_t index = type - g_registry.type_array;
    if ( index < 0 || index >= g_registry.type_count )
        return TYPE_INVALID;

    return ( uint16_t )index;
}

uint16_t
rf_get_type_id_by_hash( uint32_t hash )
{
    /* get type id by hash (type name) */

    if ( hash == 0 )
    {
        assert( 0 && "Invalid type hash (0)" );
        return TYPE_INVALID;
    }

    uint32_t slot = hash & TYPE_HASH_MASK;
    uint16_t idx  = g_registry.type_hash[ slot ];
    while ( idx != TYPE_INVALID )
    {
        const rf_type_t* t = &g_registry.type_array[ idx ];
        if ( t->hash == hash )
        {
            return idx;    // found the type!
        }
        idx = t->next;
    }
    return TYPE_INVALID;
}

const rf_type_t*
rf_get_type_by_name( const char* type_name )
{
    /* get type by name -- slower method */

    if ( !type_name )
    {
        assert( 0 && "Invalid type name (NULL)" );
        return &g_registry.type_array[ 0 ];
    }

    const uint32_t hash    = sid_hash( type_name );
    const uint16_t type_id = rf_get_type_id_by_hash( hash );
    return rf_get_type( type_id );
}

uint16_t /* id */
rf_get_type_id_by_name( const char* type_name )
{
    /* get type id by name -- slower method */

    if ( !type_name ) {
        assert( 0 && "Invalid type name (NULL)" );
        return TYPE_INVALID;
    }

    return rf_get_type_id_by_hash( sid_hash( type_name ) );
}

/*==============================================================================================

    reflect_access.c : field accessors

==============================================================================================*/

const rf_field_t*
rf_get_field( uint32_t field_id )
{
    /* get field by id */

    if ( field_id > g_registry.field_count )
    {
        assert( 0 && "Invalid field id" );
        return NULL;
    }

    return &g_registry.field_array[ field_id ];
}

uint16_t
rf_get_field_id( const rf_field_t* field )
{
    /* get field id by pointer */

    if ( !field )
    {
        assert( 0 && "Invalid field pointer (NULL)" );
        return FIELD_INVALID;
    }

    ptrdiff_t index = field - g_registry.field_array;
    if ( index < 0 || index >= g_registry.field_count )
        return FIELD_INVALID;

    return ( uint16_t )index;
}

const rf_field_t*
rf_get_field_by_sid( uint16_t type_id, sid_t name_sid )
{
    /* get field by name sid within type */

    const rf_type_t* t = rf_get_type( type_id );
    if ( !t )
        return NULL;

    /* search fields for matchig sid */
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rf_field_t* f = &g_registry.field_array[ t->field_index + i ];
        if ( sid_equals( f->name_sid, name_sid ) )
            return f;    // <-- found it!
    }
    return NULL;    // not found
}

const rf_field_t*
rf_get_field_by_name( uint16_t type_id, const char* name )
{
    /* get field by name within type */

    sid_t name_sid = sid_intern_cstr( name );
    return rf_get_field_by_sid( type_id, name_sid );
}

uint16_t
rf_each_field( u32 type_id, rf_field_cb_t callback, void* userdata )
{
    if ( type_id == TYPE_INVALID || type_id >= g_registry.type_count )
    {
        return 0;
    }

    rf_type_t* t = &g_registry.type_array[ type_id ];
    if ( !t->valid || !callback )
    {
        return 0;
    }

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t field_idx = t->field_index + i;
        if ( field_idx >= g_registry.field_count )
            break;

        callback( field_idx, &g_registry.field_array[ field_idx ], userdata );
    }

    return t->field_count;
}

/*==============================================================================================

    reflect_access.c : attribute accessors

==============================================================================================*/

const rf_attrib_t*
rf_type_get_attr( uint16_t type_id, const char* name )
{
    const rf_type_t* t = rf_get_type( type_id );
    if ( !t || !name || t->attr_count == 0 )
        return NULL;

    sid_t name_sid = sid_intern_cstr( name );

    for ( uint16_t i = 0; i < t->attr_count; i++ )
    {
        const rf_attrib_t* a = &g_registry.attrib_array[ t->attr_index + i ];
        if ( sid_equals( a->name_sid, name_sid ) )
        {
            return a;
        }
    }

    return NULL;
}

const rf_attrib_t*
rf_field_get_attr( uint16_t field_id, const char* name )
{
    const rf_field_t* f = rf_get_field( field_id );
    if ( !f || !name || f->attr_count == 0 )
        return NULL;

    sid_t name_sid = sid_intern_cstr( name );

    for ( uint16_t i = 0; i < f->attr_count; i++ )
    {
        const rf_attrib_t* a = &g_registry.attrib_array[ f->attr_index + i ];
        if ( sid_equals( a->name_sid, name_sid ) )
        {
            return a;
        }
    }

    return NULL;
}

bool
rf_type_has_attr( uint16_t type_id, const char* name )
{
    return rf_type_get_attr( type_id, name ) != NULL;
}

bool
rf_field_has_attr( uint16_t field_id, const char* name )
{
    return rf_field_get_attr( field_id, name ) != NULL;
}

/*============================================================================================*/
