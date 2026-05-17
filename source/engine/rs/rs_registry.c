/*==============================================================================================

    core/rs/rs_registry.c - Frame lifecycle, registration, lazy resolution.

    The registry is four flat tables (types, fields, attrs, enums) plus a stack of frames.
    Each frame remembers its starting indices into the four tables. Pop = truncate.

==============================================================================================*/

static rs_registry_t g_rs;
static const bool    rs_debug = true;

/*==============================================================================================
    Hash chain helpers

    Separate chaining: each bucket in type_hash[] is the head of a singly-linked list.
    The `next` field embedded in rs_type_t is the link pointer; no external node storage.
==============================================================================================*/

static void
rs_hash_insert( uint16_t type_id )
{
    rs_type_t* t           = &g_rs.types[ type_id ];
    uint32_t   slot        = t->hash & RS_TYPE_HASH_MASK;
    t->next                = g_rs.type_hash[ slot ];
    g_rs.type_hash[ slot ] = type_id;
}

static void
rs_hash_remove( uint16_t type_id )
{
    /* Unlink type_id from its hash chain. */
    rs_type_t* t    = &g_rs.types[ type_id ];
    uint32_t   slot = t->hash & RS_TYPE_HASH_MASK;
    uint16_t   cur  = g_rs.type_hash[ slot ];
    uint16_t   prev = RS_TYPE_INVALID;

    while ( cur != RS_TYPE_INVALID && cur != type_id )
    {
        prev = cur;
        cur  = g_rs.types[ cur ].next;
    }
    if ( cur == type_id )
    {
        if ( prev == RS_TYPE_INVALID )
            g_rs.type_hash[ slot ] = t->next;
        else
            g_rs.types[ prev ].next = t->next;
    }
}

static uint16_t
rs_hash_find( uint32_t hash )
{
    uint16_t idx = g_rs.type_hash[ hash & RS_TYPE_HASH_MASK ];
    while ( idx != RS_TYPE_INVALID )
    {
        if ( g_rs.types[ idx ].hash == hash )
            return idx;
        idx = g_rs.types[ idx ].next;
    }
    return RS_TYPE_INVALID;
}

/*==============================================================================================
    Built-in primitives - installed in frame 0 ("system")
==============================================================================================*/

static const struct
{
    const char* name;
    uint16_t    size;
    uint8_t     align;
} RS_BUILTINS[ RS_PRIM_COUNT ] = {
    [RS_PRIM_INVALID] = {"invalid",  0,               0              },
    [RS_PRIM_VOID]    = {"void",     0,               0              },
    [RS_PRIM_BOOL]    = {"bool",     1,               1              },
    [RS_PRIM_CHAR]    = {"char",     1,               1              },
    [RS_PRIM_I8]      = {"int8_t",   1,               1              },
    [RS_PRIM_U8]      = {"uint8_t",  1,               1              },
    [RS_PRIM_I16]     = {"int16_t",  2,               2              },
    [RS_PRIM_U16]     = {"uint16_t", 2,               2              },
    [RS_PRIM_I32]     = {"int32_t",  4,               4              },
    [RS_PRIM_U32]     = {"uint32_t", 4,               4              },
    [RS_PRIM_I64]     = {"int64_t",  8,               8              },
    [RS_PRIM_U64]     = {"uint64_t", 8,               8              },
    [RS_PRIM_F32]     = {"float",    4,               4              },
    [RS_PRIM_F64]     = {"double",   8,               8              },
    [RS_PRIM_STRING]  = {"string",   sizeof( char* ), sizeof( char* )},
};

static void
rs_install_builtins( void )
{
    for ( uint16_t i = 0; i < RS_PRIM_COUNT; i++ )
    {
        rs_type_t* t   = &g_rs.types[ i ];
        t->name_id     = rs_intern( RS_BUILTINS[ i ].name );
        t->hash        = rs_hash_str( RS_BUILTINS[ i ].name );
        t->schema_hash = 0;
        t->field_index = 0;
        t->field_count = 0;
        t->attr_index  = RS_ATTR_INVALID;
        t->attr_count  = 0;
        t->next        = RS_TYPE_INVALID;
        t->size        = RS_BUILTINS[ i ].size;
        t->align       = RS_BUILTINS[ i ].align;
        t->kind        = RS_KIND_PRIM;
        t->frame_id    = 0;
        rs_hash_insert( i );
    }
    /* type_count is now RS_PRIM_COUNT, which means each primitive's type_id equals
       its rs_prim_t enum value directly (e.g. RS_PRIM_F32 == type_id for float). */
    g_rs.type_count = RS_PRIM_COUNT;
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

void
rs_init( void )
{
    g_rs_str_top = 0;
    memset( &g_rs, 0, sizeof( g_rs ) );
    for ( int i = 0; i < RS_TYPE_HASH_SIZE; i++ ) g_rs.type_hash[ i ] = RS_TYPE_INVALID;

    /* Frame 0 = "system": holds the built-in primitives. Cannot be popped. */
    uint16_t sys = rs_push_frame( "system", 1 );
    ( void )sys;
    rs_install_builtins();
}

void
rs_exit( void )
{
    while ( g_rs.frame_count > 1 ) rs_pop_frame( g_rs.frame_count - 1 );
    memset( &g_rs, 0, sizeof( g_rs ) );
    g_rs_str_top = 0;
}

void
rs_get_stats( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count )
{
    if ( type_count )
        *type_count = g_rs.type_count;
    if ( field_count )
        *field_count = g_rs.field_count;
    if ( frame_count )
        *frame_count = g_rs.frame_count;
}

/*==============================================================================================
    Frames
==============================================================================================*/

uint16_t
rs_push_frame( const char* name, uint32_t version )
{
    if ( !name || g_rs.frame_count >= RS_MAX_FRAMES )
    {
        assert( 0 && "rs_push_frame: invalid name or frame table full" );
        return RS_FRAME_INVALID;
    }

    uint16_t    id = g_rs.frame_count++;
    rs_frame_t* f  = &g_rs.frames[ id ];

    f->name_id     = rs_intern( name );
    f->version     = version;
    f->dll_handle  = NULL;
    f->first_type  = g_rs.type_count;
    f->first_field = g_rs.field_count;
    f->first_attr  = g_rs.attr_count;
    f->first_enum  = g_rs.enum_count;

    if ( rs_debug )
        printf( "rs: push frame[%u] '%s' v%u\n", id, name, version );

    return id;
}

void
rs_pop_frame( uint16_t frame_id )
{
    /* Strict LIFO: caller must pop dependents first. */
    if ( frame_id == RS_FRAME_INVALID || frame_id != g_rs.frame_count - 1 )
    {
        assert( 0 && "rs_pop_frame: must pop in LIFO order" );
        return;
    }
    if ( frame_id == 0 )
    {
        assert( 0 && "rs_pop_frame: cannot pop the system frame" );
        return;
    }

    rs_frame_t* f = &g_rs.frames[ frame_id ];

    /* Unhook every type in this frame from its hash chain. */
    for ( uint16_t i = f->first_type; i < g_rs.type_count; i++ ) rs_hash_remove( i );

    /* Truncate every table back to the frame's start marks. */
    g_rs.type_count  = f->first_type;
    g_rs.field_count = f->first_field;
    g_rs.attr_count  = f->first_attr;
    g_rs.enum_count  = f->first_enum;
    g_rs.frame_count--;

    memset( f, 0, sizeof( *f ) );

    if ( rs_debug )
        printf( "rs: popped frame[%u]\n", frame_id );
}

const rs_frame_t*
rs_get_frame( uint16_t frame_id )
{
    if ( frame_id >= g_rs.frame_count )
        return NULL;
    return &g_rs.frames[ frame_id ];
}

/*==============================================================================================
    Schema hash - content hash of a type's field layout
==============================================================================================*/

static uint32_t
rs_fnv1a_step( uint32_t h, const void* data, size_t len )
{
    const uint8_t* p = ( const uint8_t* )data;
    for ( size_t i = 0; i < len; i++ )
    {
        h ^= p[ i ];
        h *= 16777619u; /* FNV-1a 32-bit prime */
    }
    return h;
}

static uint32_t
rs_compute_schema_hash( const rs_field_t* fields, uint16_t count )
{
    uint32_t h = 2166136261u; /* FNV-1a 32-bit offset basis */
    for ( uint16_t i = 0; i < count; i++ )
    {
        const rs_field_t* f = &fields[ i ];
        h                   = rs_fnv1a_step( h, &f->name_id, sizeof( f->name_id ) );
        h                   = rs_fnv1a_step( h, &f->offset, sizeof( f->offset ) );
        h                   = rs_fnv1a_step( h, &f->size, sizeof( f->size ) );
        h                   = rs_fnv1a_step( h, &f->mods, sizeof( f->mods ) );
        h                   = rs_fnv1a_step( h, &f->aux, sizeof( f->aux ) );
        h                   = rs_fnv1a_step( h, &f->type_hash, sizeof( f->type_hash ) );
    }
    return h;
}

/*==============================================================================================
    Registration
==============================================================================================*/

static uint16_t
rs_alloc_type_slot( void )
{
    if ( g_rs.type_count >= RS_MAX_TYPES )
    {
        assert( 0 && "rs: type table full" );
        return RS_TYPE_INVALID;
    }
    return g_rs.type_count++;
}

static uint16_t
rs_alloc_field_block( uint16_t count )
{
    if ( count == 0 )
        return 0; /* index unused when field_count == 0; 0 is a safe placeholder */
    if ( g_rs.field_count + count > RS_MAX_FIELDS )
    {
        assert( 0 && "rs: field table full" );
        return RS_FIELD_INVALID;
    }
    uint16_t start = g_rs.field_count;
    g_rs.field_count += count;
    return start;
}

static uint16_t
rs_alloc_attr_slot( void )
{
    if ( g_rs.attr_count >= RS_MAX_ATTRS )
    {
        assert( 0 && "rs: attribute table full" );
        return RS_ATTR_INVALID;
    }
    return g_rs.attr_count++;
}

static uint16_t
rs_alloc_enum_block( uint16_t count )
{
    if ( count == 0 )
        return 0;
    if ( g_rs.enum_count + count > RS_MAX_ENUMS )
    {
        assert( 0 && "rs: enum table full" );
        return RS_TYPE_INVALID;
    }
    uint16_t start = g_rs.enum_count;
    g_rs.enum_count += count;
    return start;
}

uint16_t
rs_register_type( const rs_type_t* type, const rs_field_t* fields, uint16_t field_count )
{
    if ( !type )
    {
        assert( 0 );
        return RS_TYPE_INVALID;
    }
    if ( field_count > 0 && !fields )
    {
        assert( 0 && "rs_register_type: missing fields array" );
        return RS_TYPE_INVALID;
    }
    if ( g_rs.frame_count == 0 )
    {
        assert( 0 && "rs_register_type: no active frame" );
        return RS_TYPE_INVALID;
    }

    uint16_t frame_id = g_rs.frame_count - 1;
    uint16_t type_id  = rs_alloc_type_slot();
    if ( type_id == RS_TYPE_INVALID )
        return RS_TYPE_INVALID;

    rs_type_t* t   = &g_rs.types[ type_id ];
    *t             = *type;
    t->frame_id    = ( uint8_t )frame_id;
    t->attr_index  = RS_ATTR_INVALID;
    t->attr_count  = 0;
    t->next        = RS_TYPE_INVALID;
    t->schema_hash = rs_compute_schema_hash( fields, field_count );

    /* Reserve a contiguous block of fields and copy them in. */
    if ( field_count > 0 )
    {
        uint16_t first = rs_alloc_field_block( field_count );
        if ( first == RS_FIELD_INVALID )
            return RS_TYPE_INVALID;

        t->field_index = first;
        t->field_count = field_count;

        for ( uint16_t i = 0; i < field_count; i++ )
        {
            rs_field_t* dst = &g_rs.fields[ first + i ];
            *dst            = fields[ i ];
            dst->attr_index = RS_ATTR_INVALID;
            dst->attr_count = 0;

            /* Lazy resolve: try once now; gaps resolved at finalize_frame. */
            uint16_t resolved = rs_hash_find( dst->type_hash );
            dst->type_id      = resolved;
            if ( resolved != RS_TYPE_INVALID )
                dst->kind = g_rs.types[ resolved ].kind;
        }
    }
    else
    {
        t->field_index = 0;
        t->field_count = 0;
    }

    rs_hash_insert( type_id );

    if ( rs_debug )
        printf( "rs: registered [%u] %s (frame %u, %u fields)\n", type_id, rs_cstr( t->name_id ), frame_id,
                field_count );

    return type_id;
}

/*==============================================================================================
    Enum registration
==============================================================================================*/

static uint32_t
rs_compute_enum_schema_hash( const rs_enum_t* e, uint16_t count )
{
    uint32_t h = 2166136261u;
    for ( uint16_t i = 0; i < count; i++ )
    {
        h = rs_fnv1a_step( h, &e[ i ].name_id, sizeof( e[ i ].name_id ) );
        h = rs_fnv1a_step( h, &e[ i ].value, sizeof( e[ i ].value ) );
    }
    return h;
}

uint16_t
rs_register_enum( const rs_type_t* type, const rs_enum_t* enums, uint16_t count )
{
    if ( !type )
    {
        assert( 0 );
        return RS_TYPE_INVALID;
    }
    if ( count > 0 && !enums )
    {
        assert( 0 );
        return RS_TYPE_INVALID;
    }
    if ( g_rs.frame_count == 0 )
    {
        assert( 0 && "rs_register_enum: no active frame" );
        return RS_TYPE_INVALID;
    }

    uint16_t frame_id = g_rs.frame_count - 1;
    uint16_t type_id  = rs_alloc_type_slot();
    if ( type_id == RS_TYPE_INVALID )
        return RS_TYPE_INVALID;

    rs_type_t* t   = &g_rs.types[ type_id ];
    *t             = *type;
    t->kind        = RS_KIND_ENUM;
    t->frame_id    = ( uint8_t )frame_id;
    t->attr_index  = RS_ATTR_INVALID;
    t->attr_count  = 0;
    t->next        = RS_TYPE_INVALID;
    t->schema_hash = rs_compute_enum_schema_hash( enums, count );

    if ( count > 0 )
    {
        uint16_t first = rs_alloc_enum_block( count );
        if ( first == RS_TYPE_INVALID )
            return RS_TYPE_INVALID;
        memcpy( &g_rs.enums[ first ], enums, count * sizeof( rs_enum_t ) );
        t->field_index = first; /* indexes enums[] when kind == RS_KIND_ENUM */
        t->field_count = count;
    }
    else
    {
        t->field_index = 0;
        t->field_count = 0;
    }

    rs_hash_insert( type_id );

    if ( rs_debug )
        printf( "rs: registered enum [%u] %s (frame %u, %u enumerators)\n", type_id, rs_cstr( t->name_id ),
                frame_id, count );

    return type_id;
}

uint16_t
rs_register_bitset( const rs_type_t* type, const rs_enum_t* enums, uint16_t count )
{
    if ( !type )
    {
        assert( 0 );
        return RS_TYPE_INVALID;
    }

    uint16_t type_id = rs_register_enum( type, enums, count );
    if ( type_id != RS_TYPE_INVALID )
        g_rs.types[ type_id ].kind = RS_KIND_BITSET;
    return type_id;
}

/*==============================================================================================
    Function-signature registration

    A signature type uses the same field-storage path as a struct; the only difference
    is kind == RS_KIND_FUNCTION and the index convention (slot 0 = return, 1..N = params).
==============================================================================================*/

uint16_t
rs_register_function( const rs_type_t* type, const rs_field_t* return_then_params, uint16_t count )
{
    if ( !type || count == 0 )
    {
        assert( 0 && "rs_register_function: a signature must have at least a return type" );
        return RS_TYPE_INVALID;
    }

    rs_type_t patched = *type;
    patched.kind      = RS_KIND_FUNCTION;
    /* Signatures themselves have no inherent size; pointer fields that reference one
       carry sizeof(void*) themselves. Leave caller's `size` as-is (commonly sizeof(void*)). */

    return rs_register_type( &patched, return_then_params, count );
}

/*==============================================================================================
    Attribute attachment

    Attributes must be appended contiguously: all attrs for one owner (type or field) must be
    pushed before moving to the next owner. We enforce that with the trailing-block check.
==============================================================================================*/

static bool
rs_append_attr( uint16_t* owner_index, uint16_t* owner_count, const rs_attrib_t* attr )
{
    if ( !attr )
    {
        assert( 0 );
        return false;
    }

    uint16_t slot = rs_alloc_attr_slot();
    if ( slot == RS_ATTR_INVALID )
        return false;

    if ( *owner_count == 0 )
    {
        *owner_index = slot;
        *owner_count = 1;
    }
    else
    {
        /* New entry must immediately follow this owner's existing block. */
        assert( ( uint16_t )( *owner_index + *owner_count ) == slot &&
                "rs: attributes must be added contiguously per owner" );
        ( *owner_count )++;
    }
    g_rs.attrs[ slot ] = *attr;
    return true;
}

bool
rs_type_add_attr( uint16_t type_id, const rs_attrib_t* attr )
{
    if ( type_id >= g_rs.type_count )
    {
        assert( 0 );
        return false;
    }
    rs_type_t* t = &g_rs.types[ type_id ];
    return rs_append_attr( &t->attr_index, &t->attr_count, attr );
}

bool
rs_field_add_attr( uint16_t field_id, const rs_attrib_t* attr )
{
    if ( field_id >= g_rs.field_count )
    {
        assert( 0 );
        return false;
    }
    rs_field_t* f = &g_rs.fields[ field_id ];
    return rs_append_attr( &f->attr_index, &f->attr_count, attr );
}

/*==============================================================================================
    Finalize frame - second-pass resolution and validation

    Fields registered before their base type was registered will still hold TYPE_INVALID.
    Walk the frame's fields, resolve anything still unresolved, and report leftovers.
==============================================================================================*/

bool
rs_finalize_frame( uint16_t frame_id )
{
    if ( frame_id >= g_rs.frame_count )
    {
        assert( 0 && "rs_finalize_frame: bad frame id" );
        return false;
    }

    const rs_frame_t* f = &g_rs.frames[ frame_id ];

    /* Determine the exclusive end of this frame's fields. The last (topmost) frame owns
       everything up to the current total; any earlier frame ends where the next one begins. */
    uint16_t field_end =
        ( frame_id + 1 == g_rs.frame_count ) ? g_rs.field_count : g_rs.frames[ frame_id + 1 ].first_field;

    /* Pass 1: retry resolve for any field that was a forward reference at
       registration time. By now, every type in this frame and all lower frames
       is in the hash table, so a single lookup settles it. */
    for ( uint16_t i = f->first_field; i < field_end; i++ )
    {
        rs_field_t* fld = &g_rs.fields[ i ];
        if ( fld->type_id != RS_TYPE_INVALID )
            continue;
        if ( fld->type_hash == 0 )
            continue;

        uint16_t resolved = rs_hash_find( fld->type_hash );
        if ( resolved != RS_TYPE_INVALID )
        {
            fld->type_id = resolved;
            fld->kind    = g_rs.types[ resolved ].kind;
        }
    }

    /* Pass 2: report leftovers. */
    bool ok = true;
    for ( uint16_t i = f->first_field; i < field_end; i++ )
    {
        const rs_field_t* fld = &g_rs.fields[ i ];
        if ( fld->type_id == RS_TYPE_INVALID && fld->type_hash != 0 )
        {
            printf( "rs: ERROR unresolved field '%s' (type hash 0x%08x) in frame %u\n",
                    rs_cstr( fld->name_id ), fld->type_hash, frame_id );
            ok = false;
        }
    }
    return ok;
}

/*==============================================================================================
    Module DLL integration
==============================================================================================*/

uint16_t
rs_load_module_dll( const char* name, void* dll )
{
    if ( !dll || !name )
        return RS_FRAME_INVALID;

    typedef void ( *rs_register_fn )( const rs_reg_api_t* api );
    rs_register_fn rs_reg = ( rs_register_fn )sys_library_get_symbol( ( lib_handle_t )dll, "rs_register" );
    if ( !rs_reg )
        return RS_FRAME_INVALID;

    uint16_t frame = rs_push_frame( name, 1 );
    if ( frame == RS_FRAME_INVALID )
        return RS_FRAME_INVALID;

    rs_reg_api_t api = {
        .intern             = rs_intern,
        .rs_register_type   = rs_register_type,
        .rs_register_enum   = rs_register_enum,
        .rs_register_bitset = rs_register_bitset,
        .rs_type_add_attr   = rs_type_add_attr,
        .rs_field_add_attr  = rs_field_add_attr,
        .rs_get_type        = rs_get_type,
    };

    rs_reg( &api );
    rs_finalize_frame( frame );
    return frame;
}

void
rs_unload_module_dll( const char* name )
{
    if ( !name || g_rs.frame_count <= 1 )
        return;

    uint16_t top = g_rs.frame_count - 1;
    if ( strcmp( rs_cstr( g_rs.frames[ top ].name_id ), name ) == 0 )
    {
        rs_pop_frame( top );
        return;
    }

    printf( "rs: WARNING rs_unload_module_dll: '%s' is not the topmost frame (top='%s'); skipped\n",
            name, rs_cstr( g_rs.frames[ top ].name_id ) );
}

uint16_t
rs_load_module_static( const char* name, void ( *rs_reg )( const rs_reg_api_t* ) )
{
    if ( !name || !rs_reg )
        return RS_FRAME_INVALID;

    uint16_t frame = rs_push_frame( name, 1 );
    if ( frame == RS_FRAME_INVALID )
        return RS_FRAME_INVALID;

    rs_reg_api_t api = {
        .intern             = rs_intern,
        .rs_register_type   = rs_register_type,
        .rs_register_enum   = rs_register_enum,
        .rs_register_bitset = rs_register_bitset,
        .rs_type_add_attr   = rs_type_add_attr,
        .rs_field_add_attr  = rs_field_add_attr,
        .rs_get_type        = rs_get_type,
    };

    rs_reg( &api );
    rs_finalize_frame( frame );
    return frame;
}

/*============================================================================================*/