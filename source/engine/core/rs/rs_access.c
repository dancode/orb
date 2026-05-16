/*==============================================================================================

    core/rs/rs_access.c - Type, field, and attribute lookups + iteration.

==============================================================================================*/

/*==============================================================================================
    Types
==============================================================================================*/

const rs_type_t*
rs_get_type( uint16_t type_id )
{
    if ( type_id == RS_TYPE_INVALID || type_id >= g_rs.type_count )
        return NULL;
    return &g_rs.types[ type_id ];
}

uint16_t
rs_find_type( uint32_t name_hash )
{
    return rs_hash_find( name_hash );
}

uint16_t
rs_find_type_by_name( const char* name )
{
    if ( !name )
        return RS_TYPE_INVALID;
    return rs_hash_find( sid_hash( name ) );
}

/*==============================================================================================
    Fields
==============================================================================================*/

const rs_field_t*
rs_get_field( uint16_t field_id )
{
    if ( field_id == RS_FIELD_INVALID || field_id >= g_rs.field_count )
        return NULL;
    return &g_rs.fields[ field_id ];
}

const rs_field_t*
rs_find_field( uint16_t type_id, const char* name )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || !name )
        return NULL;

    sid_t target = sid_intern_cstr( name );
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_field_t* f = &g_rs.fields[ t->field_index + i ];
        if ( sid_equals( f->name_sid, target ) )
            return f;
    }
    return NULL;
}

/*==============================================================================================
    Attributes - first match wins (use the iterator for repeated entries)
==============================================================================================*/

static const rs_attrib_t*
rs_find_attr_in_block( uint16_t first, uint16_t count, const char* name )
{
    if ( count == 0 || first == RS_ATTR_INVALID || !name )
        return NULL;

    sid_t target = sid_intern_cstr( name );
    for ( uint16_t i = 0; i < count; i++ )
    {
        const rs_attrib_t* a = &g_rs.attrs[ first + i ];
        if ( sid_equals( a->name_sid, target ) )
            return a;
    }
    return NULL;
}

const rs_attrib_t*
rs_type_get_attr( uint16_t type_id, const char* name )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t ) return NULL;
    return rs_find_attr_in_block( t->attr_index, t->attr_count, name );
}

const rs_attrib_t*
rs_field_get_attr( uint16_t field_id, const char* name )
{
    const rs_field_t* f = rs_get_field( field_id );
    if ( !f ) return NULL;
    return rs_find_attr_in_block( f->attr_index, f->attr_count, name );
}

/*==============================================================================================
    Enumerators (only valid for types with kind == RS_KIND_ENUM)
==============================================================================================*/

const rs_enumerator_t*
rs_get_enumerator( uint16_t enum_id )
{
    if ( enum_id >= g_rs.enum_count )
        return NULL;
    return &g_rs.enums[ enum_id ];
}

const rs_enumerator_t*
rs_enum_find_by_name( uint16_t type_id, const char* name )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_ENUM || !name )
        return NULL;

    sid_t target = sid_intern_cstr( name );
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enumerator_t* e = &g_rs.enums[ t->field_index + i ];
        if ( sid_equals( e->name_sid, target ) )
            return e;
    }
    return NULL;
}

const rs_enumerator_t*
rs_enum_find_by_value( uint16_t type_id, int64_t value )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_ENUM )
        return NULL;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enumerator_t* e = &g_rs.enums[ t->field_index + i ];
        if ( e->value == value )
            return e;
    }
    return NULL;
}

/*==============================================================================================
    Function signature accessors (kind == RS_KIND_FUNCTION)
==============================================================================================*/

const rs_field_t*
rs_function_get_return( uint16_t type_id )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_FUNCTION || t->field_count == 0 )
        return NULL;
    return &g_rs.fields[ t->field_index ];
}

uint16_t
rs_function_param_count( uint16_t type_id )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_FUNCTION || t->field_count == 0 )
        return 0;
    return (uint16_t)( t->field_count - 1 );
}

const rs_field_t*
rs_function_get_param( uint16_t type_id, uint16_t param_index )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_FUNCTION )
        return NULL;
    if ( (uint16_t)( param_index + 1 ) >= t->field_count )
        return NULL;
    return &g_rs.fields[ t->field_index + 1 + param_index ];
}

/*==============================================================================================
    Iteration
==============================================================================================*/

uint16_t
rs_each_type( rs_type_cb_t cb, void* user )
{
    if ( !cb ) return 0;
    for ( uint16_t i = 0; i < g_rs.type_count; i++ )
        cb( i, &g_rs.types[ i ], user );
    return g_rs.type_count;
}

uint16_t
rs_each_type_in_frame( uint8_t frame_id, rs_type_cb_t cb, void* user )
{
    if ( !cb || frame_id >= g_rs.frame_count ) return 0;

    uint16_t first = g_rs.frames[ frame_id ].first_type;
    uint16_t end   = ( frame_id + 1 == g_rs.frame_count )
                     ? g_rs.type_count
                     : g_rs.frames[ frame_id + 1 ].first_type;

    for ( uint16_t i = first; i < end; i++ )
        cb( i, &g_rs.types[ i ], user );
    return (uint16_t)( end - first );
}

uint16_t
rs_each_field( uint16_t type_id, rs_field_cb_t cb, void* user )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || !cb || t->kind == RS_KIND_ENUM ) return 0;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t fid = (uint16_t)( t->field_index + i );
        cb( fid, &g_rs.fields[ fid ], user );
    }
    return t->field_count;
}

uint16_t
rs_each_enumerator( uint16_t type_id, rs_enum_cb_t cb, void* user )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || !cb || t->kind != RS_KIND_ENUM ) return 0;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t eid = (uint16_t)( t->field_index + i );
        cb( eid, &g_rs.enums[ eid ], user );
    }
    return t->field_count;
}

/*============================================================================================*/
