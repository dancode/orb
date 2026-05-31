/* engine/ref/ref_access.c - Type, field, attribute lookups and iteration.

   All functions here are read-only accessors into g_ref. They are safe to call from any
   thread that holds a read lock on the registry; none of them mutate state.
   ref_hash_find (in ref_registry.c) is the fast O(1) path; the linear scans below are
   used only when an index is not available (find-by-name within a type, etc.). */

/*==============================================================================================
    Type Access

    Retrieving types by ID or name hash. Type IDs are stable within a session as long as
    the owning frame has not been popped. Across hot-reloads the ID changes; use name_hash
    for identity that survives reload.
==============================================================================================*/

const ref_type_t*
ref_get_type( uint16_t type_id )
{
    if ( type_id == REF_TYPE_INVALID || type_id >= g_ref.type_count )
        return NULL;
    return &g_ref.types[ type_id ];
}

uint16_t
ref_find_type( uint32_t name_hash )
{
    return ref_hash_find( name_hash );
}

uint16_t
ref_find_type_by_name( const char* name )
{
    if ( !name ) return REF_TYPE_INVALID;
    return ref_hash_find( ref_hash_str( name ) );
}

/*==============================================================================================
    Field Access

    Retrieving field metadata by ID or name within a type.
==============================================================================================*/

const ref_field_t*
ref_get_field( uint16_t field_id )
{
    if ( field_id == REF_FIELD_INVALID || field_id >= g_ref.field_count )
        return NULL;
    return &g_ref.fields[ field_id ];
}

const ref_field_t*
ref_find_field( uint16_t type_id, const char* name )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || !name ) return NULL;

    /* Linear scan through the type's field block. Acceptable because structs rarely have
       more than ~16 fields; a hash would add complexity for no practical gain. */
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const ref_field_t* f = &g_ref.fields[ t->field_index + i ];
        if ( strcmp( ref_cstr( f->name_id ), name ) == 0 )
            return f;
    }
    return NULL;
}

/*==============================================================================================
    Attribute Access

    Attributes are accessed by name string; the first matching entry in the owner's block
    is returned. For multi-entry attributes (e.g. @range with min and max as two consecutive
    entries sharing the same name_id), the caller must read subsequent entries via pointer
    arithmetic (+1) since there is no ref_each_attr iterator in the public API.
==============================================================================================*/

static const ref_attrib_t*
ref_find_attr_in_block( uint16_t first, uint16_t count, const char* name )
{
    if ( count == 0 || first == REF_ATTR_INVALID || !name ) return NULL;
    for ( uint16_t i = 0; i < count; i++ )
    {
        const ref_attrib_t* a = &g_ref.attrs[ first + i ];
        if ( strcmp( ref_cstr( a->name_id ), name ) == 0 ) return a;
    }
    return NULL;
}

const ref_attrib_t*
ref_type_get_attr( uint16_t type_id, const char* name )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t ) return NULL;
    return ref_find_attr_in_block( t->attr_index, t->attr_count, name );
}

const ref_attrib_t*
ref_field_get_attr( uint16_t field_id, const char* name )
{
    const ref_field_t* f = ref_get_field( field_id );
    if ( !f ) return NULL;
    return ref_find_attr_in_block( f->attr_index, f->attr_count, name );
}

/*==============================================================================================
    Enumerator Access

    Retrieving individual enum values or searching by name/value.
==============================================================================================*/

const ref_enum_t*
ref_get_enumerator( uint16_t enum_id )
{
    if ( enum_id >= g_ref.enum_count ) return NULL;
    return &g_ref.enums[ enum_id ];
}

const ref_enum_t*
ref_enum_find_by_name( uint16_t type_id, const char* name )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || !ref_kind_is_enum( (ref_kind_t)t->kind ) || !name ) return NULL;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const ref_enum_t* e = &g_ref.enums[ t->field_index + i ];
        if ( strcmp( ref_cstr( e->name_id ), name ) == 0 ) return e;
    }
    return NULL;
}

const ref_enum_t*
ref_enum_find_by_value( uint16_t type_id, int32_t value )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || !ref_kind_is_enum( (ref_kind_t)t->kind ) ) return NULL;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const ref_enum_t* e = &g_ref.enums[ t->field_index + i ];
        if ( e->value == value ) return e;
    }
    return NULL;
}

/*==============================================================================================
    Bitset Helpers

    Bitsets are enums where values are independent bit masks that can be OR'd together.
    The "greedy bit-claim" algorithm in ref_bitset_each_set_flag/ref_bitset_describe iterates
    enumerators in registration order and claims bits as it matches them. Once a flag's bits
    are claimed they are cleared from the remaining mask, so smaller overlapping flags do not
    double-fire on bits already consumed by a multi-bit entry.

    This means registration order determines precedence: place multi-bit compound values
    (e.g. ALL = READ|WRITE|EXEC) BEFORE their constituent single-bit flags so that a value
    of ALL prints as "ALL" rather than "READ | WRITE | EXEC".
==============================================================================================*/

bool
ref_enum_is_bitset( uint16_t type_id )
{
    const ref_type_t* t = ref_get_type( type_id );
    return t && t->kind == REF_KIND_BITSET;
}

const ref_enum_t*
ref_bitset_find_flag( uint16_t type_id, int32_t mask )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || t->kind != REF_KIND_BITSET || mask == 0 ) return NULL;

    /* Exact mask match -- used when the caller has a single flag value and wants its name. */
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const ref_enum_t* e = &g_ref.enums[ t->field_index + i ];
        if ( e->value == mask ) return e;
    }
    return NULL;
}

uint16_t
ref_bitset_each_set_flag( uint16_t type_id, int32_t value, ref_enum_cb_t cb, void* user )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || t->kind != REF_KIND_BITSET || !cb ) return 0;

    /* Greedy bit-claim: matched flag's bits are cleared so smaller overlapping flags
       don't double-fire on the same bits. Registration order controls priority. */
    int32_t  remaining = value;
    uint16_t hits      = 0;
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const ref_enum_t* e = &g_ref.enums[ t->field_index + i ];
        if ( e->value == 0 ) continue;
        if ( ( remaining & e->value ) == e->value )
        {
            uint16_t eid = (uint16_t)( t->field_index + i );
            cb( eid, e, user );
            remaining &= ~e->value;   /* consume bits so narrower flags don't re-fire */
            hits++;
        }
    }
    return hits;
}

size_t
ref_bitset_describe( uint16_t type_id, int32_t value, char* buf, size_t buf_size )
{
    if ( !buf || buf_size == 0 ) return 0;
    buf[ 0 ] = '\0';

    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || t->kind != REF_KIND_BITSET ) return 0;

    /* Zero is a special case: look for a zero-valued enumerator (e.g. "NONE"), or just "0". */
    if ( value == 0 )
    {
        const ref_enum_t* z = ref_enum_find_by_value( type_id, 0 );
        const char* s = z ? ref_cstr( z->name_id ) : "0";
        size_t n = 0;
        while ( s[ n ] && n + 1 < buf_size ) { buf[ n ] = s[ n ]; n++; }
        buf[ n ] = '\0';
        return n;
    }

    size_t  pos       = 0;
    int32_t remaining = value;
    bool    first     = true;

    /* Emit "FLAG_A | FLAG_B" for matched bits; each matched flag clears its bits from remaining. */
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const ref_enum_t* e = &g_ref.enums[ t->field_index + i ];
        if ( e->value == 0 ) continue;
        if ( ( remaining & e->value ) != e->value ) continue;

        if ( !first ) { const char* sep = " | "; while ( *sep && pos + 1 < buf_size ) buf[ pos++ ] = *sep++; }
        const char* name = ref_cstr( e->name_id );
        while ( *name && pos + 1 < buf_size ) buf[ pos++ ] = *name++;
        first = false;
        remaining &= ~e->value;
    }

    /* Any bits not claimed by a named flag are appended as a hex literal tail. */
    if ( remaining != 0 )
    {
        char tmp[ 32 ];
        snprintf( tmp, sizeof( tmp ), "%s0x%llx", first ? "" : " | ", (long long)remaining );
        const char* p = tmp;
        while ( *p && pos + 1 < buf_size ) buf[ pos++ ] = *p++;
    }

    if ( pos < buf_size ) buf[ pos ] = '\0';
    return pos;
}

/*==============================================================================================
    Function Signature Access

    Function signature types store their return and parameters as fields, following the
    convention: field[0] = return type, field[1..field_count-1] = parameters in order.
    These helpers provide named access to avoid off-by-one mistakes with that index layout.
    Use these instead of ref_each_field() for REF_KIND_FUNCTION types.
==============================================================================================*/

const ref_field_t*
ref_function_get_return( uint16_t type_id )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || t->kind != REF_KIND_FUNCTION || t->field_count == 0 ) return NULL;
    return &g_ref.fields[ t->field_index ];
}

uint16_t
ref_function_param_count( uint16_t type_id )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || t->kind != REF_KIND_FUNCTION || t->field_count == 0 ) return 0;
    return (uint16_t)( t->field_count - 1 );
}

const ref_field_t*
ref_function_get_param( uint16_t type_id, uint16_t param_index )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t || t->kind != REF_KIND_FUNCTION ) return NULL;
    if ( (uint16_t)( param_index + 1 ) >= t->field_count ) return NULL;
    return &g_ref.fields[ t->field_index + 1 + param_index ];
}

/*==============================================================================================
    Iteration

    Callback-based iteration over types, fields, and enumerators. The callback receives
    both the id (index into the relevant pool) and a const pointer to the record so the
    caller can either use the record directly or store the id for later re-lookup.

    ref_each_field and ref_each_enumerator are intentionally exclusive -- ref_each_field
    rejects enum/bitset types because their "fields" are actually entries in enums[], not
    fields[], and reading them as ref_field_t would be incorrect.
==============================================================================================*/

uint16_t
ref_each_type( ref_type_cb_t cb, void* user )
{
    if ( !cb ) return 0;
    for ( uint16_t i = 0; i < g_ref.type_count; i++ ) cb( i, &g_ref.types[ i ], user );
    return g_ref.type_count;
}

uint16_t
ref_each_type_in_frame( uint16_t frame_id, ref_type_cb_t cb, void* user )
{
    if ( !cb || frame_id >= g_ref.frame_count ) return 0;

    uint16_t first = g_ref.frames[ frame_id ].first_type;

    /* Topmost frame owns [first_type .. type_count); earlier frames own
       [first_type .. next_frame.first_type). Derive the end boundary accordingly. */
    uint16_t end   = ( frame_id + 1 == g_ref.frame_count )
                     ? g_ref.type_count
                     : g_ref.frames[ frame_id + 1 ].first_type;

    for ( uint16_t i = first; i < end; i++ ) cb( i, &g_ref.types[ i ], user );
    return (uint16_t)( end - first );
}

uint16_t
ref_each_field( uint16_t type_id, ref_field_cb_t cb, void* user )
{
    const ref_type_t* t = ref_get_type( type_id );

    /* Enum and bitset types store their entries in enums[], not fields[]. Iterating them
       here would be a table-confusion bug; callers must use ref_each_enumerator instead. */
    if ( !t || !cb || ref_kind_is_enum( (ref_kind_t)t->kind ) ) return 0;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t fid = (uint16_t)( t->field_index + i );
        cb( fid, &g_ref.fields[ fid ], user );
    }
    return t->field_count;
}

uint16_t
ref_each_enumerator( uint16_t type_id, ref_enum_cb_t cb, void* user )
{
    const ref_type_t* t = ref_get_type( type_id );

    /* Only valid for enum/bitset types -- for structs/unions use ref_each_field. */
    if ( !t || !cb || !ref_kind_is_enum( (ref_kind_t)t->kind ) ) return 0;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t eid = (uint16_t)( t->field_index + i );
        cb( eid, &g_ref.enums[ eid ], user );
    }
    return t->field_count;
}

/*============================================================================================*/
