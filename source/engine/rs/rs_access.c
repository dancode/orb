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
    return rs_hash_find( rs_hash_str( name ) );
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

    uint32_t h = rs_hash_str( name );
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_field_t* f = &g_rs.fields[ t->field_index + i ];
        if ( f->name_hash == h )
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

    uint32_t h = rs_hash_str( name );
    for ( uint16_t i = 0; i < count; i++ )
    {
        const rs_attrib_t* a = &g_rs.attrs[ first + i ];
        if ( a->name_hash == h )
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
    Enumerators (only valid for types with rs_kind_is_enum(kind))
==============================================================================================*/

const rs_enum_t*
rs_get_enumerator( uint16_t enum_id )
{
    if ( enum_id >= g_rs.enum_count )
        return NULL;
    return &g_rs.enums[ enum_id ];
}

const rs_enum_t*
rs_enum_find_by_name( uint16_t type_id, const char* name )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || !rs_kind_is_enum( (rs_kind_t)t->kind ) || !name )
        return NULL;

    uint32_t h = rs_hash_str( name );
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enum_t* e = &g_rs.enums[ t->field_index + i ];
        if ( e->name_hash == h )
            return e;
    }
    return NULL;
}

const rs_enum_t*
rs_enum_find_by_value( uint16_t type_id, int64_t value )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || !rs_kind_is_enum( (rs_kind_t)t->kind ) )
        return NULL;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enum_t* e = &g_rs.enums[ t->field_index + i ];
        if ( e->value == value )
            return e;
    }
    return NULL;
}

/*==============================================================================================
    Bitset-enum helpers

      Set semantics: an enumerator E is "set" in `value` iff E.value != 0
                     and (value & E.value) == E.value.

      Decoding is order-dependent when enums have overlapping bits (e.g. a multi-bit
      mask alongside its single-bit components). Place multi-bit constants FIRST in the
      registration order to get them matched preferentially.
==============================================================================================*/

bool
rs_enum_is_bitset( uint16_t type_id )
{
    const rs_type_t* t = rs_get_type( type_id );
    return t && t->kind == RS_KIND_BITSET;
}

const rs_enum_t*
rs_bitset_find_flag( uint16_t type_id, int64_t mask )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_BITSET || mask == 0 )
        return NULL;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enum_t* e = &g_rs.enums[ t->field_index + i ];
        if ( e->value == mask )
            return e;
    }
    return NULL;
}

uint16_t
rs_bitset_each_set_flag( uint16_t type_id, int64_t value, rs_enum_cb_t cb, void* user )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_BITSET || !cb )
        return 0;

    /* Greedy bit-claim: each matched flag's bits are cleared from `remaining` so that
       smaller overlapping flags cannot double-fire on the same bits. Registration order
       therefore controls which flag wins (e.g. ALL before READ/WRITE/EXEC). */
    int64_t  remaining = value;
    uint16_t hits      = 0;
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enum_t* e = &g_rs.enums[ t->field_index + i ];
        if ( e->value == 0 ) continue;
        if ( ( remaining & e->value ) == e->value )
        {
            uint16_t eid = (uint16_t)( t->field_index + i );
            cb( eid, e, user );
            remaining &= ~e->value;
            hits++;
        }
    }
    return hits;
}

size_t
rs_bitset_describe( uint16_t type_id, int64_t value, char* buf, size_t buf_size )
{
    if ( !buf || buf_size == 0 ) return 0;
    buf[ 0 ] = '\0';

    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || t->kind != RS_KIND_BITSET ) return 0;

    /* Special case: zero. Prefer a registered zero-named enumerator (often "NONE"). */
    if ( value == 0 )
    {
        const rs_enum_t* z = rs_enum_find_by_value( type_id, 0 );
        const char* s = z ? rs_cstr( z->name_id ) : "0";
        size_t      n = 0;
        while ( s[ n ] && n + 1 < buf_size ) { buf[ n ] = s[ n ]; n++; }
        buf[ n ] = '\0';
        return n;
    }

    size_t  pos        = 0;
    int64_t remaining  = value;
    bool    first      = true;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_enum_t* e = &g_rs.enums[ t->field_index + i ];
        if ( e->value == 0 ) continue;
        if ( ( remaining & e->value ) != e->value ) continue;

        if ( !first )
        {
            const char* sep = " | ";
            while ( *sep && pos + 1 < buf_size ) buf[ pos++ ] = *sep++;
        }
        const char* name = rs_cstr( e->name_id );
        while ( *name && pos + 1 < buf_size ) buf[ pos++ ] = *name++;
        first = false;
        remaining &= ~e->value;
    }

    if ( remaining != 0 )
    {
        char tmp[ 32 ];
        snprintf( tmp, sizeof( tmp ), "%s0x%llx",
                  first ? "" : " | ", (long long)remaining );
        const char* p = tmp;
        while ( *p && pos + 1 < buf_size ) buf[ pos++ ] = *p++;
    }

    if ( pos < buf_size ) buf[ pos ] = '\0';
    return pos;
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
rs_each_type_in_frame( uint16_t frame_id, rs_type_cb_t cb, void* user )
{
    if ( !cb || frame_id >= g_rs.frame_count ) return 0;

    uint16_t first = g_rs.frames[ frame_id ].first_type;
    /* Last (topmost) frame owns up to type_count; earlier frames end where the next begins. */
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
    if ( !t || !cb || rs_kind_is_enum( (rs_kind_t)t->kind ) ) return 0;

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
    if ( !t || !cb || !rs_kind_is_enum( (rs_kind_t)t->kind ) ) return 0;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t eid = (uint16_t)( t->field_index + i );
        cb( eid, &g_rs.enums[ eid ], user );
    }
    return t->field_count;
}

/*============================================================================================*/
