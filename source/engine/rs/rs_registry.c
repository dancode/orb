/*==============================================================================================

    engine/rs/rs_registry.c - Frame lifecycle, registration, lazy type resolution

==============================================================================================*/
/*==============================================================================================
    Type Hashing

    Open-addressed bucket hash table for O(1) type lookup by name hash.

    The chain links are stored intrinsically in rs_type_t.next rather than in a separate
    linked-list node pool. This keeps the type record self-contained and avoids a secondary
    allocation. Collision chains are singly-linked and sorted newest-first (prepend on insert).

    RS_TYPE_INVALID (0xFFFF) is the end-of-chain sentinel, so the table must be initialized
    with that value rather than zero before first use.
==============================================================================================*/
// clang-format off

static void
rs_hash_insert( uint16_t type_id )
{
    rs_type_t* t           = &g_rs.types[ type_id ];
    uint32_t   slot        = t->name_hash & RS_TYPE_HASH_MASK;

    /* Prepend this type to the front of the bucket chain. */
    t->next                = g_rs.type_hash[ slot ];
    g_rs.type_hash[ slot ] = type_id;
}

static void
rs_hash_remove( uint16_t type_id )
{
    rs_type_t* t    = &g_rs.types[ type_id ];
    uint32_t   slot = t->name_hash & RS_TYPE_HASH_MASK;
    uint16_t   cur  = g_rs.type_hash[ slot ];
    uint16_t   prev = RS_TYPE_INVALID;

    /* Walk the chain to find the node and stitch around it. */
    while ( cur != RS_TYPE_INVALID && cur != type_id )
    {
        prev = cur;
        cur  = g_rs.types[ cur ].next;
    }
    if ( cur == type_id )
    {
        if ( prev == RS_TYPE_INVALID )
            g_rs.type_hash[ slot ] = t->next;      /* was the head */
        else
            g_rs.types[ prev ].next = t->next;     /* middle of chain */
    }
}

static uint16_t
rs_hash_find( uint32_t hash )
{
    /* Walk the bucket chain; compare full 32-bit hash to handle collisions. */
    uint16_t idx = g_rs.type_hash[ hash & RS_TYPE_HASH_MASK ];
    while ( idx != RS_TYPE_INVALID )
    {
        if ( g_rs.types[ idx ].name_hash == hash )
            return idx;
        idx = g_rs.types[ idx ].next;
    }
    return RS_TYPE_INVALID;
}

/*==============================================================================================
    Built-in Primitives

    Registers the fundamental C types in the "rs" frame (frame 0) so that field type_hash
    values for int, float, etc. can be resolved during module registration.

    Critical invariant: the type_id assigned to each primitive equals its rs_prim_t enum
    value (e.g. RS_PRIM_F32 == the type_id for float). This is guaranteed because
    rs_install_builtins fills slots 0..RS_PRIM_COUNT-1 in order before any module type
    is registered, and the type table always starts empty at init.
==============================================================================================*/

static const struct { const char* name; uint16_t size; uint8_t align; }
RS_BUILTINS[ RS_PRIM_COUNT ] = {
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
        t->name_hash   = rs_hash_str( RS_BUILTINS[ i ].name );
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
    /* type_id == rs_prim_t enum value for each primitive (e.g. RS_PRIM_F32 == type_id for float) */
    g_rs.type_count = RS_PRIM_COUNT;
}

/*==============================================================================================
    Lifecycle

    Global init/exit for the reflection system.

    rs_init is normally called once at program start through rs_wire_mod_callbacks() in
    the host, but rs_ensure_init() provides a lazy guard so registration callbacks fired
    during module load are safe even if the call order is unusual.
==============================================================================================*/

static bool g_rs_initialized = false;

void
rs_init( void )
{
    g_rs_str_top = 0;
    memset( &g_rs, 0, sizeof( g_rs ) );

    /* Hash buckets must be RS_TYPE_INVALID (0xFFFF), not 0, which is a valid type_id. */
    for ( int i = 0; i < RS_TYPE_HASH_SIZE; i++ ) g_rs.type_hash[ i ] = RS_TYPE_INVALID;

    /* Frame 0 is owned by the reflection system itself and holds only the built-in prims.
       It is never popped; rs_exit tears it down by resetting the whole registry. */
    uint16_t sys = rs_push_frame( "rs" );
    ( void )sys;
    rs_install_builtins();

    g_rs_initialized = true;
}

static inline void
rs_ensure_init( void )
{
    /* Module DLL load callbacks may fire before the host has called rs_init explicitly,
       so every public entry point that touches g_rs must call this guard first. */
    if ( !g_rs_initialized )
        rs_init();
}

void
rs_exit( void )
{
    if ( !g_rs_initialized )
        return;

    /* Pop all module frames in LIFO order before wiping the registry so that pop-callbacks
       (if any) see a consistent table during teardown. Frame 0 ("rs") is destroyed by the
       memset below rather than by rs_pop_frame, since frame 0 can never be popped. */
    while ( g_rs.frame_count > 1 ) rs_pop_frame( g_rs.frame_count - 1 );
    memset( &g_rs, 0, sizeof( g_rs ) );
    g_rs_str_top     = 0;
    g_rs_initialized = false;
}

void
rs_get_stats( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count )
{
    if ( type_count )  *type_count  = g_rs.type_count;
    if ( field_count ) *field_count = g_rs.field_count;
    if ( frame_count ) *frame_count = g_rs.frame_count;
}

/*==============================================================================================
    Frame Management

    A frame is a "watermark" snapshot of the four pool counters taken at DLL load time.
    When a module unloads, popping its frame rewinds all four pools to those watermarks --
    an O(1) bulk free with no tombstones, validity flags, or memory fragmentation.

    Frames must be popped in strict LIFO order because the arrays are contiguous: popping
    a frame from the middle would leave a gap that invalidates every later type_id, field_id,
    and attr_id. The mod system guarantees reverse-load order on unload.
==============================================================================================*/

uint16_t
rs_push_frame( const char* name )
{
    if ( !name || g_rs.frame_count >= RS_MAX_FRAMES )
    {
        assert( 0 && "rs_push_frame: invalid name or frame table full" );
        return RS_FRAME_INVALID;
    }

    uint16_t    id = g_rs.frame_count++;
    rs_frame_t* f  = &g_rs.frames[ id ];

    f->name_id = rs_intern( name );

    /* Snapshot the current pool tops; pop rewinds back to these exact values. */
    f->first_type  = g_rs.type_count;
    f->first_field = g_rs.field_count;
    f->first_attr  = g_rs.attr_count;
    f->first_enum  = g_rs.enum_count;

    if ( rs_debug )
        printf( "rs: push frame[%u] '%s'\n", id, name );

    return id;
}

void
rs_pop_frame( uint16_t frame_id )
{
    if ( frame_id == RS_FRAME_INVALID || frame_id != g_rs.frame_count - 1 )
    {
        assert( 0 && "rs_pop_frame: must pop in LIFO order" );
        return;
    }
    if ( frame_id == 0 )
    {
        assert( 0 && "rs_pop_frame: cannot pop the reflect frame" );
        return;
    }

    rs_frame_t* f = &g_rs.frames[ frame_id ];

    /* Remove every type owned by this frame from the hash table before rewinding the count,
       so stale type_ids are never returned by rs_hash_find. */
    for ( uint16_t i = f->first_type; i < g_rs.type_count; i++ ) rs_hash_remove( i );

    /* Rewind the pool counters -- the records themselves are simply abandoned in place;
       the next push will overwrite them without any explicit free. */
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
    Schema Hashing

    Computes a deterministic hash of the physical field layout for a type. This hash is
    stored in rs_type_t.schema_hash and used in two places:

      1. Hot-reload ABI safety: if a reloaded DLL produces a different schema_hash for the
         same type name, the host knows the struct layout changed and can refuse to
         reuse old state or save data.

      2. Save-game compatibility: rs_write embeds schema_hash in the file header; rs_read
         rejects a buffer whose hash does not match, preventing silent data corruption
         when a type's layout changes between game versions.

    Fields included in the hash: name_id, offset, size, mods, aux, type_hash.
    type_flags and attr data are intentionally excluded -- they are editor/runtime hints
    and changing them should not invalidate saves.
==============================================================================================*/

static uint32_t
rs_fnv1a_step( uint32_t h, const void* data, size_t len )
{
    const uint8_t* p = ( const uint8_t* )data;
    for ( size_t i = 0; i < len; i++ ) { h ^= p[ i ]; h *= 16777619u; }
    return h;
}

static uint32_t
rs_compute_schema_hash( const rs_field_t* fields, uint16_t count )
{
    uint32_t h = 2166136261u;
    for ( uint16_t i = 0; i < count; i++ )
    {
        const rs_field_t* f = &fields[ i ];
        h = rs_fnv1a_step( h, &f->name_id,   sizeof( f->name_id ) );
        h = rs_fnv1a_step( h, &f->offset,    sizeof( f->offset ) );
        h = rs_fnv1a_step( h, &f->size,      sizeof( f->size ) );
        h = rs_fnv1a_step( h, &f->mods,      sizeof( f->mods ) );
        h = rs_fnv1a_step( h, &f->aux,       sizeof( f->aux ) );
        h = rs_fnv1a_step( h, &f->type_hash, sizeof( f->type_hash ) );
    }
    return h;
}

/*==============================================================================================
    Table Allocators

    All four data tables (types, fields, attrs, enums) use a simple bump-allocator pattern:
    advance the count by the requested amount and return the old top as the start index.
    Allocation is O(1) with no fragmentation. Deallocation is implicit via frame pop.

    Fields and enums are allocated in contiguous blocks so that a type's entries can be
    addressed as fields[type.field_index .. type.field_index + type.field_count).
    Attributes are also required to be contiguous per owner (see rs_append_attr).
==============================================================================================*/

static uint16_t rs_alloc_type_slot( void )
{
    if ( g_rs.type_count >= RS_MAX_TYPES ) {
        assert( 0 && "rs: type table full" ); return RS_TYPE_INVALID;
    }
    return g_rs.type_count++;
}

static uint16_t rs_alloc_field_block( uint16_t count )
{
    if ( count == 0 ) return 0;
    if ( g_rs.field_count + count > RS_MAX_FIELDS ) {
        assert( 0 && "rs: field table full" ); return RS_FIELD_INVALID;
    }
    uint16_t start = g_rs.field_count;
    g_rs.field_count += count;
    return start;
}

static uint16_t rs_alloc_enum_block( uint16_t count )
{
    if ( count == 0 ) return 0;
    if ( g_rs.enum_count + count > RS_MAX_ENUMS ) {
        assert( 0 && "rs: enum table full" ); return RS_TYPE_INVALID;
    }
    uint16_t start = g_rs.enum_count;
    g_rs.enum_count += count;
    return start;
}

static uint16_t rs_alloc_attr_slot( void )
{
    if ( g_rs.attr_count >= RS_MAX_ATTRS ) {
        assert( 0 && "rs: attribute table full" ); return RS_ATTR_INVALID;
    }
    return g_rs.attr_count++;
}

/*==============================================================================================
    Type Registration

    Registers a struct, union, or function-signature type into the current (topmost) frame.
    The caller provides a fully populated rs_type_t and an array of rs_field_t descriptors
    generated by the code-gen macros in rs_import.h.

    Two-phase design: fields carry a type_hash (FNV-1a of the base type name) instead of a
    resolved type_id because the referenced type may not yet be registered when this call
    is made. rs_register_type does a best-effort immediate resolve from the hash table;
    anything that remains unresolved is patched in rs_finalize_frame() after all types in
    the frame are registered.
==============================================================================================*/

uint16_t
rs_register_type( const rs_type_t* type, const rs_field_t* fields, uint16_t field_count )
{
    if ( !type )                      { assert( 0 ); return RS_TYPE_INVALID; }
    if ( field_count > 0 && !fields ) { assert( 0 ); return RS_TYPE_INVALID; }
    if ( g_rs.frame_count <= 1 )      { assert( 0 ); return RS_TYPE_INVALID; }

    /* The topmost frame owns types registered from this point; frame_count was already
       incremented by rs_push_frame, so the current frame index is frame_count - 1. */
    uint16_t frame_id = g_rs.frame_count - 1;
    uint16_t type_id  = rs_alloc_type_slot();
    if ( type_id == RS_TYPE_INVALID ) return RS_TYPE_INVALID;

    /* Copy the caller-supplied descriptor, then overwrite the internal bookkeeping fields
       (frame_id, attr_index, next, schema_hash) which the caller must not pre-fill. */
    rs_type_t* t   = &g_rs.types[ type_id ];
    *t             = *type;
    t->frame_id    = ( uint8_t )frame_id;
    t->attr_index  = RS_ATTR_INVALID;
    t->attr_count  = 0;
    t->next        = RS_TYPE_INVALID;
    t->schema_hash = rs_compute_schema_hash( fields, field_count );

    if ( field_count > 0 )
    {
        uint16_t first = rs_alloc_field_block( field_count );
        if ( first == RS_FIELD_INVALID ) return RS_TYPE_INVALID;

        t->field_index = first;
        t->field_count = field_count;

        for ( uint16_t i = 0; i < field_count; i++ )
        {
            rs_field_t* dst = &g_rs.fields[ first + i ];
            *dst            = fields[ i ];
            dst->attr_index = RS_ATTR_INVALID;
            dst->attr_count = 0;

            /* Best-effort immediate resolve from the hash table; catches types that were
               already registered (e.g. primitives, earlier types in the same frame).
               Remaining unresolved fields are patched in rs_finalize_frame(). */
            uint16_t resolved = rs_hash_find( dst->type_hash );
            dst->type_id      = resolved;
            if ( resolved != RS_TYPE_INVALID )
                dst->kind = g_rs.types[ resolved ].kind;   /* cache kind for fast dispatch */
        }
    }
    else
    {
        t->field_index = 0;
        t->field_count = 0;
    }

    /* Insert into the name hash table so subsequent cross-type references can find it. */
    rs_hash_insert( type_id );

    if ( rs_debug )
        printf( "rs: registered [%u] %s (frame %u, %u fields)\n", type_id, rs_cstr( t->name_id ), frame_id, field_count );

    return type_id;
}

/*==============================================================================================
    Enum & Bitset Registration

    Enums and bitsets are structurally identical (name/value pairs) but interpreted
    differently. Both reuse rs_type_t.field_index / field_count, which normally index into
    fields[], but for enum kinds they index into enums[] instead. rs_each_field() excludes
    enum kinds specifically to avoid mixing these two tables up.

    Bitsets use the same registration path as enums and are differentiated only by
    rs_type_t.kind == RS_KIND_BITSET. rs_bitset_describe() uses greedy bit-claim decoding,
    so registration order of the enumerators controls precedence for overlapping masks.
==============================================================================================*/

static uint32_t
rs_compute_enum_schema_hash( const rs_enum_t* e, uint16_t count )
{
    uint32_t h = 2166136261u;
    for ( uint16_t i = 0; i < count; i++ )
    {
        h = rs_fnv1a_step( h, &e[ i ].name_id, sizeof( e[ i ].name_id ) );
        h = rs_fnv1a_step( h, &e[ i ].value,   sizeof( e[ i ].value ) );
    }
    return h;
}

uint16_t
rs_register_enum( const rs_type_t* type, const rs_enum_t* enums, uint16_t count )
{
    if ( !type )                 { assert( 0 ); return RS_TYPE_INVALID; }
    if ( count > 0 && !enums )   { assert( 0 ); return RS_TYPE_INVALID; }
    if ( g_rs.frame_count <= 1 ) { assert( 0 ); return RS_TYPE_INVALID; }

    uint16_t frame_id = g_rs.frame_count - 1;
    uint16_t type_id  = rs_alloc_type_slot();
    if ( type_id == RS_TYPE_INVALID )
        return RS_TYPE_INVALID;

    /* Copy the caller descriptor and overwrite internal fields. Force kind=ENUM regardless
       of what the caller supplied, since enums[] vs. fields[] dispatch is keyed on kind. */
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
        if ( first == RS_TYPE_INVALID ) return RS_TYPE_INVALID;
        memcpy( &g_rs.enums[ first ], enums, count * sizeof( rs_enum_t ) );

        /* field_index/field_count reused to index into enums[] when kind == RS_KIND_ENUM.
           Callers should use rs_each_enumerator() rather than rs_each_field() for enum types. */
        t->field_index = first;
        t->field_count = count;
    }
    else
    {
        t->field_index = 0;
        t->field_count = 0;
    }

    rs_hash_insert( type_id );

    if ( rs_debug )
        printf( "rs: registered enum [%u] %s (frame %u, %u enumerators)\n", type_id, rs_cstr( t->name_id ), frame_id, count );

    return type_id;
}

uint16_t
rs_register_bitset( const rs_type_t* type, const rs_enum_t* enums, uint16_t count )
{
    /* Enums and bitsets share the same registration path since they are structurally
       identical. Register as an enum first, then fix the kind so bitset-specific helpers
       (rs_bitset_describe, rs_bitset_each_set_flag) can find it by kind. */
    if ( !type ) { assert( 0 ); return RS_TYPE_INVALID; }
    uint16_t type_id = rs_register_enum( type, enums, count );
    if ( type_id != RS_TYPE_INVALID )
        g_rs.types[ type_id ].kind = RS_KIND_BITSET;
    return type_id;
}

/*==============================================================================================
    Function Signature Registration

    Function signatures are stored as a special struct-like type with RS_KIND_FUNCTION.
    The fields array follows a fixed convention: field[0] is the return type descriptor,
    field[1..count-1] are the parameter descriptors in declaration order.

    This lets code that holds a field with RS_MODS_FUNCTION look up the signature type
    via field.aux (which holds the sig type_id) and use rs_function_get_return /
    rs_function_get_param to inspect it without any special-casing.
==============================================================================================*/

uint16_t
rs_register_function( const rs_type_t* type, const rs_field_t* return_then_params, uint16_t count )
{
    if ( !type || count == 0 ) { assert( 0 && "rs_register_function: must have at least a return type" ); return RS_TYPE_INVALID; }

    /* Force kind to FUNCTION regardless of caller's setting, then delegate to rs_register_type. */
    rs_type_t patched = *type;
    patched.kind      = RS_KIND_FUNCTION;
    return rs_register_type( &patched, return_then_params, count );
}

/*==============================================================================================
    Attribute Management

    Attributes (e.g. @range, @tooltip) are stored in a flat attrs[] array. A type or field
    owns a contiguous block: [attr_index .. attr_index + attr_count). This contiguity is
    enforced by the assertion in rs_append_attr and is required by rs_find_attr_in_block.

    Multi-value attributes (such as @range which needs a min and max entry) are represented
    as two consecutive entries with the same name_id. The rs_attrib_t.ci field encodes
    total group count and this entry's index within the group (see RS_ATTR_CI macros).
==============================================================================================*/

static bool
rs_append_attr( uint16_t* owner_index, uint16_t* owner_count, const rs_attrib_t* attr )
{
    if ( !attr ) { assert( 0 ); return false; }

    uint16_t slot = rs_alloc_attr_slot();
    if ( slot == RS_ATTR_INVALID ) return false;

    if ( *owner_count == 0 )
    {
        /* First attribute for this owner -- claim the starting index. */
        *owner_index = slot;
        *owner_count = 1;
    }
    else
    {
        /* Subsequent attributes must be allocated immediately after the previous one so
           the block stays contiguous. This means all attrs for a type/field must be added
           before moving on to the next owner. */
        assert( ( uint16_t )( *owner_index + *owner_count ) == slot && "rs: attributes must be added contiguously per owner" );
        ( *owner_count )++;
    }
    g_rs.attrs[ slot ] = *attr;
    return true;
}

bool rs_type_add_attr( uint16_t type_id, const rs_attrib_t* attr )
{
    if ( type_id >= g_rs.type_count ) { assert( 0 ); return false; }
    rs_type_t* t = &g_rs.types[ type_id ];
    return rs_append_attr( &t->attr_index, &t->attr_count, attr );
}

bool rs_field_add_attr( uint16_t field_id, const rs_attrib_t* attr )
{
    if ( field_id >= g_rs.field_count ) { assert( 0 ); return false; }
    rs_field_t* f = &g_rs.fields[ field_id ];
    return rs_append_attr( &f->attr_index, &f->attr_count, attr );
}

/*==============================================================================================
    Frame Finalization

    Performs a second resolve pass over all fields in a frame after all types in that frame
    are registered. This makes forward references work: a struct that appears earlier than
    the type it references will still resolve correctly because by finalize time the entire
    frame's type table is populated.

    Must be called exactly once per frame, after the last rs_register_* call and before
    any lookup or serialization on types from that frame. rs_register_module() calls it
    automatically; manual registration code must call it explicitly.
==============================================================================================*/

bool
rs_finalize_frame( uint16_t frame_id )
{
    if ( frame_id >= g_rs.frame_count ) { assert( 0 && "rs_finalize_frame: bad frame id" ); return false; }

    const rs_frame_t* f = &g_rs.frames[ frame_id ];

    /* Determine the end of this frame's field block. The topmost frame owns everything
       from first_field to field_count; earlier frames end where the next one starts. */
    uint16_t field_end =
        ( frame_id + 1 == g_rs.frame_count ) ? g_rs.field_count : g_rs.frames[ frame_id + 1 ].first_field;

    /* First pass: resolve any field that still has type_id == RS_TYPE_INVALID.
       These are cross-frame references (e.g. fields using types from earlier frames)
       or within-frame forward refs not caught during registration. */
    for ( uint16_t i = f->first_field; i < field_end; i++ )
    {
        rs_field_t* fld = &g_rs.fields[ i ];
        if ( fld->type_id != RS_TYPE_INVALID ) continue;   /* already resolved */
        if ( fld->type_hash == 0 ) continue;               /* no base type (void) */

        uint16_t resolved = rs_hash_find( fld->type_hash );
        if ( resolved != RS_TYPE_INVALID )
        {
            fld->type_id = resolved;
            fld->kind    = g_rs.types[ resolved ].kind;
        }
    }

    /* Second pass: report any fields that are still unresolved as hard errors.
       This indicates a type referenced by name but never registered in any frame. */
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
    Module Integration

    These two functions are the DLL-load / DLL-unload hooks that wire the mod system to the
    reflection registry. The host calls rs_wire_mod_callbacks() once at startup, which
    registers rs_register_module and rs_unregister_module as mod-load / mod-unload events.

    On DLL load:
      1. rs_ensure_init() guards against unusual startup order.
      2. A new frame is pushed for the module.
      3. The module's rs_register callback (from mod_desc_t.rs_register) is invoked with a
         vtable of registration functions -- this is how generated code stays decoupled from
         the rs_ internals.
      4. rs_finalize_frame() resolves any forward references.

    On DLL unload:
      The frame is simply popped, which bulk-frees all types/fields/attrs/enums for that
      module in O(1). The next hot-reload re-registers everything fresh.
==============================================================================================*/

uint16_t
rs_register_module( const char* name, const mod_desc_t* desc )
{
    if ( !name || !desc || !desc->rs_register )
        return RS_FRAME_INVALID;

    /* Lazy init guard: registration callbacks can fire before rs_mod_init if load order
       varies, so we must ensure the registry is ready before touching any pool. */
    rs_ensure_init();

    typedef void ( *rs_register_fn )( const rs_reg_api_t* api );
    rs_register_fn rs_reg = ( rs_register_fn )desc->rs_register;

    uint16_t frame = rs_push_frame( name );
    if ( frame == RS_FRAME_INVALID ) return RS_FRAME_INVALID;

    /* Build the registration vtable that generated DLL code calls into.
       This keeps the generated code independent of the internal rs_ implementation. */
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
rs_unregister_module( const char* name )
{
    if ( !name || g_rs.frame_count <= 1 )
        return;

    uint16_t top = g_rs.frame_count - 1;
    if ( strcmp( rs_cstr( g_rs.frames[ top ].name_id ), name ) == 0 )
    {
        rs_pop_frame( top );
        return;
    }

    /* Module had no frame (no rs_register in its mod_desc) -- silently ignore.
       Only warn if the name exists deeper in the stack, which would indicate a real LIFO
       violation that could corrupt the pool watermarks. */
    if ( rs_debug )
    {
        for ( uint16_t i = 1; i < g_rs.frame_count; ++i )
        {
            if ( strcmp( rs_cstr( g_rs.frames[ i ].name_id ), name ) == 0 )
            {
                printf( "rs: WARNING rs_unregister_module: '%s' is not the topmost frame (top='%s'); skipped\n",
                        name, rs_cstr( g_rs.frames[ top ].name_id ) );
                return;
            }
        }
    }
}

// clang-format off
/*============================================================================================*/