/*==============================================================================================

    engine/ref/ref_registry.c - Frame lifecycle, registration, lazy type resolution

==============================================================================================*/
/*==============================================================================================
    Type Hashing

    Open-addressed bucket hash table for O(1) type lookup by name hash.

    The chain links are stored intrinsically in ref_type_t.next rather than in a separate
    linked-list node pool. This keeps the type record self-contained and avoids a secondary
    allocation. Collision chains are singly-linked and sorted newest-first (prepend on insert).

    REF_TYPE_INVALID (0xFFFF) is the end-of-chain sentinel, so the table must be initialized
    with that value rather than zero before first use.
==============================================================================================*/
// clang-format off

static void
ref_hash_insert( uint16_t type_id )
{
    ref_type_t* t           = &g_ref.types[ type_id ];
    uint32_t   slot        = t->name_hash & REF_TYPE_HASH_MASK;

    /* Prepend this type to the front of the bucket chain. */
    t->next                = g_ref.type_hash[ slot ];
    g_ref.type_hash[ slot ] = type_id;
}

static void
ref_hash_remove( uint16_t type_id )
{
    ref_type_t* t    = &g_ref.types[ type_id ];
    uint32_t   slot = t->name_hash & REF_TYPE_HASH_MASK;
    uint16_t   cur  = g_ref.type_hash[ slot ];
    uint16_t   prev = REF_TYPE_INVALID;

    /* Walk the chain to find the node and stitch around it. */
    while ( cur != REF_TYPE_INVALID && cur != type_id )
    {
        prev = cur;
        cur  = g_ref.types[ cur ].next;
    }
    if ( cur == type_id )
    {
        if ( prev == REF_TYPE_INVALID )
            g_ref.type_hash[ slot ] = t->next;      /* was the head */
        else
            g_ref.types[ prev ].next = t->next;     /* middle of chain */
    }
}

static uint16_t
ref_hash_find( uint32_t hash )
{
    /* Walk the bucket chain; compare full 32-bit hash to handle collisions. */
    uint16_t idx = g_ref.type_hash[ hash & REF_TYPE_HASH_MASK ];
    while ( idx != REF_TYPE_INVALID )
    {
        if ( g_ref.types[ idx ].name_hash == hash )
            return idx;
        idx = g_ref.types[ idx ].next;
    }
    return REF_TYPE_INVALID;
}

/*==============================================================================================
    Built-in Primitives

    Registers the fundamental C types in the "ref" frame (frame 0) so that field type_hash
    values for int, float, etc. can be resolved during module registration.

    Critical invariant: the type_id assigned to each primitive equals its ref_prim_t enum
    value (e.g. REF_PRIM_F32 == the type_id for float). This is guaranteed because
    ref_install_builtins fills slots 0..REF_PRIM_COUNT-1 in order before any module type
    is registered, and the type table always starts empty at init.
==============================================================================================*/

static const struct { const char* name; uint16_t size; uint8_t align; }
REF_BUILTINS[ REF_PRIM_COUNT ] = {
    [REF_PRIM_INVALID] = {"invalid",  0,               0              },
    [REF_PRIM_VOID]    = {"void",     0,               0              },
    [REF_PRIM_BOOL]    = {"bool",     1,               1              },
    [REF_PRIM_CHAR]    = {"char",     1,               1              },
    [REF_PRIM_I8]      = {"int8_t",   1,               1              },
    [REF_PRIM_U8]      = {"uint8_t",  1,               1              },
    [REF_PRIM_I16]     = {"int16_t",  2,               2              },
    [REF_PRIM_U16]     = {"uint16_t", 2,               2              },
    [REF_PRIM_I32]     = {"int32_t",  4,               4              },
    [REF_PRIM_U32]     = {"uint32_t", 4,               4              },
    [REF_PRIM_I64]     = {"int64_t",  8,               8              },
    [REF_PRIM_U64]     = {"uint64_t", 8,               8              },
    [REF_PRIM_F32]     = {"float",    4,               4              },
    [REF_PRIM_F64]     = {"double",   8,               8              },
    [REF_PRIM_STRING]  = {"string",   sizeof( char* ), sizeof( char* )},
};

static void
ref_install_builtins( void )
{
    for ( uint16_t i = 0; i < REF_PRIM_COUNT; i++ )
    {
        ref_type_t* t   = &g_ref.types[ i ];
        t->name_id     = ref_intern( REF_BUILTINS[ i ].name );
        t->name_hash   = ref_hash_str( REF_BUILTINS[ i ].name );
        t->schema_hash = 0;
        t->field_index = 0;
        t->field_count = 0;
        t->attr_index  = REF_ATTR_INVALID;
        t->attr_count  = 0;
        t->next        = REF_TYPE_INVALID;
        t->size        = REF_BUILTINS[ i ].size;
        t->align       = REF_BUILTINS[ i ].align;
        t->kind        = REF_KIND_PRIM;
        t->frame_id    = 0;
        ref_hash_insert( i );
    }
    /* type_id == ref_prim_t enum value for each primitive (e.g. REF_PRIM_F32 == type_id for float) */
    g_ref.type_count = REF_PRIM_COUNT;
}

/*==============================================================================================
    Lifecycle

    Global init/exit for the reflection system.

    ref_init is normally called once at program start through ref_wire_mod_callbacks() in
    the host, but ref_ensure_init() provides a lazy guard so registration callbacks fired
    during module load are safe even if the call order is unusual.
==============================================================================================*/

static bool g_ref_initialized = false;

void
ref_init( void )
{
    g_ref_str_top = 0;
    memset( &g_ref, 0, sizeof( g_ref ) );

    /* Hash buckets must be REF_TYPE_INVALID (0xFFFF), not 0, which is a valid type_id. */
    for ( int i = 0; i < REF_TYPE_HASH_SIZE; i++ ) g_ref.type_hash[ i ] = REF_TYPE_INVALID;

    /* Frame 0 is owned by the reflection system itself and holds only the built-in prims.
       It is never popped; ref_exit tears it down by resetting the whole registry. */
    uint16_t sys = ref_push_frame( "ref" );
    ( void )sys;
    ref_install_builtins();

    g_ref_initialized = true;
}

static inline void
ref_ensure_init( void )
{
    /* Module DLL load callbacks may fire before the host has called ref_init explicitly,
       so every public entry point that touches g_ref must call this guard first. */
    if ( !g_ref_initialized )
        ref_init();
}

void
ref_exit( void )
{
    if ( !g_ref_initialized )
        return;

    /* Pop all module frames in LIFO order before wiping the registry so that pop-callbacks
       (if any) see a consistent table during teardown. Frame 0 ("ref") is destroyed by the
       memset below rather than by ref_pop_frame, since frame 0 can never be popped. */
    while ( g_ref.frame_count > 1 ) ref_pop_frame( g_ref.frame_count - 1 );
    memset( &g_ref, 0, sizeof( g_ref ) );
    g_ref_str_top     = 0;
    g_ref_initialized = false;
}

void
ref_get_stats( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count )
{
    if ( type_count )  *type_count  = g_ref.type_count;
    if ( field_count ) *field_count = g_ref.field_count;
    if ( frame_count ) *frame_count = g_ref.frame_count;
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
ref_push_frame( const char* name )
{
    if ( !name )
    {
        assert( 0 && "ref_push_frame: name is NULL" );
        return REF_FRAME_INVALID;
    }
    if ( g_ref.frame_count >= REF_MAX_FRAMES )
    {
        fprintf( stderr, "ref: FATAL frame table full (%u/%u) -- increase REF_MAX_FRAMES in ref.h\n",
                 (unsigned)g_ref.frame_count, (unsigned)REF_MAX_FRAMES );
        assert( 0 && "ref: frame table full -- increase REF_MAX_FRAMES in ref.h" );
        return REF_FRAME_INVALID;
    }

    uint16_t    id = g_ref.frame_count++;
    ref_frame_t* f  = &g_ref.frames[ id ];

    f->name_id = ref_intern( name );

    /* Snapshot the current pool tops; pop rewinds back to these exact values. */
    f->first_type  = g_ref.type_count;
    f->first_field = g_ref.field_count;
    f->first_attr  = g_ref.attr_count;
    f->first_enum  = g_ref.enum_count;

    if ( ref_debug )
        printf( "ref: push frame[%u] '%s'\n", id, name );

    return id;
}

void
ref_pop_frame( uint16_t frame_id )
{
    if ( frame_id == REF_FRAME_INVALID || frame_id != g_ref.frame_count - 1 )
    {
        assert( 0 && "ref_pop_frame: must pop in LIFO order" );
        return;
    }
    if ( frame_id == 0 )
    {
        assert( 0 && "ref_pop_frame: cannot pop the reflect frame" );
        return;
    }

    ref_frame_t* f = &g_ref.frames[ frame_id ];

    /* Remove every type owned by this frame from the hash table before rewinding the count,
       so stale type_ids are never returned by ref_hash_find. */
    for ( uint16_t i = f->first_type; i < g_ref.type_count; i++ ) ref_hash_remove( i );

    /* Rewind the pool counters -- the records themselves are simply abandoned in place;
       the next push will overwrite them without any explicit free. */
    g_ref.type_count  = f->first_type;
    g_ref.field_count = f->first_field;
    g_ref.attr_count  = f->first_attr;
    g_ref.enum_count  = f->first_enum;
    g_ref.frame_count--;

    memset( f, 0, sizeof( *f ) );

    if ( ref_debug )
        printf( "ref: popped frame[%u]\n", frame_id );
}

const ref_frame_t*
ref_get_frame( uint16_t frame_id )
{
    if ( frame_id >= g_ref.frame_count )
        return NULL;
    return &g_ref.frames[ frame_id ];
}

/*==============================================================================================
    Schema Hashing

    Computes a deterministic hash of the physical field layout for a type. This hash is
    stored in ref_type_t.schema_hash and used in two places:

      1. Hot-reload ABI safety: if a reloaded DLL produces a different schema_hash for the
         same type name, the host knows the struct layout changed and can refuse to
         reuse old state or save data.

      2. Save-game compatibility: ref_write embeds schema_hash in the file header; ref_read
         rejects a buffer whose hash does not match, preventing silent data corruption
         when a type's layout changes between game versions.

    Fields included in the hash: name_id, offset, size, mods, aux, type_hash.
    type_flags and attr data are intentionally excluded -- they are editor/runtime hints
    and changing them should not invalidate saves.
==============================================================================================*/

static uint32_t
ref_fnv1a_step( uint32_t h, const void* data, size_t len )
{
    const uint8_t* p = ( const uint8_t* )data;
    for ( size_t i = 0; i < len; i++ ) { h ^= p[ i ]; h *= 16777619u; }
    return h;
}

static uint32_t
ref_compute_schema_hash( const ref_field_t* fields, uint16_t count )
{
    uint32_t h = 2166136261u;
    for ( uint16_t i = 0; i < count; i++ )
    {
        const ref_field_t* f = &fields[ i ];
        h = ref_fnv1a_step( h, &f->name_id,   sizeof( f->name_id ) );
        h = ref_fnv1a_step( h, &f->offset,    sizeof( f->offset ) );
        h = ref_fnv1a_step( h, &f->size,      sizeof( f->size ) );
        h = ref_fnv1a_step( h, &f->mods,      sizeof( f->mods ) );
        h = ref_fnv1a_step( h, &f->aux,       sizeof( f->aux ) );
        h = ref_fnv1a_step( h, &f->type_hash, sizeof( f->type_hash ) );
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
    Attributes are also required to be contiguous per owner (see ref_append_attr).
==============================================================================================*/

static uint16_t ref_alloc_type_slot( void )
{
    if ( g_ref.type_count >= REF_MAX_TYPES )
    {
        fprintf( stderr, "ref: FATAL type table full (%u/%u) -- increase REF_MAX_TYPES in ref.h\n",
                 (unsigned)g_ref.type_count, (unsigned)REF_MAX_TYPES );
        assert( 0 && "ref: type table full -- increase REF_MAX_TYPES in ref.h" );
        return REF_TYPE_INVALID;
    }
    return g_ref.type_count++;
}

static uint16_t ref_alloc_field_block( uint16_t count )
{
    if ( count == 0 ) return 0;
    if ( g_ref.field_count + count > REF_MAX_FIELDS )
    {
        fprintf( stderr, "ref: FATAL field table full (used %u + need %u > limit %u) -- increase REF_MAX_FIELDS in ref.h\n",
                 (unsigned)g_ref.field_count, (unsigned)count, (unsigned)REF_MAX_FIELDS );
        assert( 0 && "ref: field table full -- increase REF_MAX_FIELDS in ref.h" );
        return REF_FIELD_INVALID;
    }
    uint16_t start = g_ref.field_count;
    g_ref.field_count += count;
    return start;
}

static uint16_t ref_alloc_enum_block( uint16_t count )
{
    if ( count == 0 ) return 0;
    if ( g_ref.enum_count + count > REF_MAX_ENUMS )
    {
        fprintf( stderr, "ref: FATAL enum table full (used %u + need %u > limit %u) -- increase REF_MAX_ENUMS in ref.h\n",
                 (unsigned)g_ref.enum_count, (unsigned)count, (unsigned)REF_MAX_ENUMS );
        assert( 0 && "ref: enum table full -- increase REF_MAX_ENUMS in ref.h" );
        return REF_TYPE_INVALID;
    }
    uint16_t start = g_ref.enum_count;
    g_ref.enum_count += count;
    return start;
}

static uint16_t ref_alloc_attr_slot( void )
{
    if ( g_ref.attr_count >= REF_MAX_ATTRS )
    {
        fprintf( stderr, "ref: FATAL attr table full (%u/%u) -- increase REF_MAX_ATTRS in ref.h\n",
                 (unsigned)g_ref.attr_count, (unsigned)REF_MAX_ATTRS );
        assert( 0 && "ref: attr table full -- increase REF_MAX_ATTRS in ref.h" );
        return REF_ATTR_INVALID;
    }
    return g_ref.attr_count++;
}

/*==============================================================================================
    Type Registration

    Registers a struct, union, or function-signature type into the current (topmost) frame.
    The caller provides a fully populated ref_type_t and an array of ref_field_t descriptors
    generated by the code-gen macros in ref_import.h.

    Two-phase design: fields carry a type_hash (FNV-1a of the base type name) instead of a
    resolved type_id because the referenced type may not yet be registered when this call
    is made. ref_register_type does a best-effort immediate resolve from the hash table;
    anything that remains unresolved is patched in ref_finalize_frame() after all types in
    the frame are registered.
==============================================================================================*/

uint16_t
ref_register_type( const ref_type_t* type, const ref_field_t* fields, uint16_t field_count )
{
    if ( !type )                       { assert( 0 ); return REF_TYPE_INVALID; }
    if ( field_count > 0 && !fields )  { assert( 0 ); return REF_TYPE_INVALID; }
    if ( g_ref.frame_count <= 1 )      { assert( 0 ); return REF_TYPE_INVALID; }

    /* The topmost frame owns types registered from this point; frame_count was already
       incremented by ref_push_frame, so the current frame index is frame_count - 1. */
    uint16_t frame_id = g_ref.frame_count - 1;
    uint16_t type_id  = ref_alloc_type_slot();
    if ( type_id == REF_TYPE_INVALID ) return REF_TYPE_INVALID;

    /* Copy the caller-supplied descriptor, then overwrite the internal bookkeeping fields
       (frame_id, attr_index, next, schema_hash) which the caller must not pre-fill. */
    ref_type_t* t   = &g_ref.types[ type_id ];
    *t             = *type;
    t->frame_id    = ( uint8_t )frame_id;
    t->attr_index  = REF_ATTR_INVALID;
    t->attr_count  = 0;
    t->next        = REF_TYPE_INVALID;
    t->schema_hash = ref_compute_schema_hash( fields, field_count );

    if ( field_count > 0 )
    {
        uint16_t first = ref_alloc_field_block( field_count );
        if ( first == REF_FIELD_INVALID ) return REF_TYPE_INVALID;

        t->field_index = first;
        t->field_count = field_count;

        for ( uint16_t i = 0; i < field_count; i++ )
        {
            ref_field_t* dst = &g_ref.fields[ first + i ];
            *dst            = fields[ i ];
            dst->attr_index = REF_ATTR_INVALID;
            dst->attr_count = 0;

            /* Best-effort immediate resolve from the hash table; catches types that were
               already registered (e.g. primitives, earlier types in the same frame).
               Remaining unresolved fields are patched in ref_finalize_frame(). */
            uint16_t resolved = ref_hash_find( dst->type_hash );
            dst->type_id      = resolved;
            if ( resolved != REF_TYPE_INVALID )
                dst->kind = g_ref.types[ resolved ].kind;   /* cache kind for fast dispatch */
        }
    }
    else
    {
        t->field_index = 0;
        t->field_count = 0;
    }

    /* Insert into the name hash table so subsequent cross-type references can find it. */
    ref_hash_insert( type_id );

    if ( ref_debug )
        printf( "ref: registered [%u] %s (frame %u, %u fields)\n", type_id, ref_cstr( t->name_id ), frame_id, field_count );

    return type_id;
}

/*==============================================================================================
    Enum & Bitset Registration

    Enums and bitsets are structurally identical (name/value pairs) but interpreted
    differently. Both reuse ref_type_t.field_index / field_count, which normally index into
    fields[], but for enum kinds they index into enums[] instead. ref_each_field() excludes
    enum kinds specifically to avoid mixing these two tables up.

    Bitsets use the same registration path as enums and are differentiated only by
    ref_type_t.kind == REF_KIND_BITSET. ref_bitset_describe() uses greedy bit-claim decoding,
    so registration order of the enumerators controls precedence for overlapping masks.
==============================================================================================*/

static uint32_t
ref_compute_enum_schema_hash( const ref_enum_t* e, uint16_t count )
{
    uint32_t h = 2166136261u;
    for ( uint16_t i = 0; i < count; i++ )
    {
        h = ref_fnv1a_step( h, &e[ i ].name_id, sizeof( e[ i ].name_id ) );
        h = ref_fnv1a_step( h, &e[ i ].value,   sizeof( e[ i ].value ) );
    }
    return h;
}

uint16_t
ref_register_enum( const ref_type_t* type, const ref_enum_t* enums, uint16_t count )
{
    if ( !type )                  { assert( 0 ); return REF_TYPE_INVALID; }
    if ( count > 0 && !enums )    { assert( 0 ); return REF_TYPE_INVALID; }
    if ( g_ref.frame_count <= 1 ) { assert( 0 ); return REF_TYPE_INVALID; }

    uint16_t frame_id = g_ref.frame_count - 1;
    uint16_t type_id  = ref_alloc_type_slot();
    if ( type_id == REF_TYPE_INVALID )
        return REF_TYPE_INVALID;

    /* Copy the caller descriptor and overwrite internal fields. Force kind=ENUM regardless
       of what the caller supplied, since enums[] vs. fields[] dispatch is keyed on kind. */
    ref_type_t* t   = &g_ref.types[ type_id ];
    *t             = *type;
    t->kind        = REF_KIND_ENUM;
    t->frame_id    = ( uint8_t )frame_id;
    t->attr_index  = REF_ATTR_INVALID;
    t->attr_count  = 0;
    t->next        = REF_TYPE_INVALID;
    t->schema_hash = ref_compute_enum_schema_hash( enums, count );

    if ( count > 0 )
    {
        uint16_t first = ref_alloc_enum_block( count );
        if ( first == REF_TYPE_INVALID ) return REF_TYPE_INVALID;
        memcpy( &g_ref.enums[ first ], enums, count * sizeof( ref_enum_t ) );

        /* field_index/field_count reused to index into enums[] when kind == REF_KIND_ENUM.
           Callers should use ref_each_enumerator() rather than ref_each_field() for enum types. */
        t->field_index = first;
        t->field_count = count;
    }
    else
    {
        t->field_index = 0;
        t->field_count = 0;
    }

    ref_hash_insert( type_id );

    if ( ref_debug )
        printf( "ref: registered enum [%u] %s (frame %u, %u enumerators)\n", type_id, ref_cstr( t->name_id ), frame_id, count );

    return type_id;
}

uint16_t
ref_register_bitset( const ref_type_t* type, const ref_enum_t* enums, uint16_t count )
{
    /* Enums and bitsets share the same registration path since they are structurally
       identical. Register as an enum first, then fix the kind so bitset-specific helpers
       (ref_bitset_describe, ref_bitset_each_set_flag) can find it by kind. */
    if ( !type ) { assert( 0 ); return REF_TYPE_INVALID; }
    uint16_t type_id = ref_register_enum( type, enums, count );
    if ( type_id != REF_TYPE_INVALID )
        g_ref.types[ type_id ].kind = REF_KIND_BITSET;
    return type_id;
}

/*==============================================================================================
    Function Signature Registration

    Function signatures are stored as a special struct-like type with REF_KIND_FUNCTION.
    The fields array follows a fixed convention: field[0] is the return type descriptor,
    field[1..count-1] are the parameter descriptors in declaration order.

    This lets code that holds a field with REF_MODS_FUNCTION look up the signature type
    via field.aux (which holds the sig type_id) and use ref_function_get_return /
    ref_function_get_param to inspect it without any special-casing.
==============================================================================================*/

uint16_t
ref_register_function( const ref_type_t* type, const ref_field_t* return_then_params, uint16_t count )
{
    if ( !type || count == 0 ) { assert( 0 && "ref_register_function: must have at least a return type" ); return REF_TYPE_INVALID; }

    /* Force kind to FUNCTION regardless of caller's setting, then delegate to ref_register_type. */
    ref_type_t patched = *type;
    patched.kind      = REF_KIND_FUNCTION;
    return ref_register_type( &patched, return_then_params, count );
}

/*==============================================================================================
    Attribute Management

    Attributes (e.g. @range, @tooltip) are stored in a flat attrs[] array. A type or field
    owns a contiguous block: [attr_index .. attr_index + attr_count). This contiguity is
    enforced by the assertion in ref_append_attr and is required by ref_find_attr_in_block.

    Multi-value attributes (such as @range which needs a min and max entry) are represented
    as two consecutive entries with the same name_id. The ref_attrib_t.ci field encodes
    total group count and this entry's index within the group (see REF_ATTR_CI macros).
==============================================================================================*/

static bool
ref_append_attr( uint16_t* owner_index, uint16_t* owner_count, const ref_attrib_t* attr )
{
    if ( !attr ) { assert( 0 ); return false; }

    uint16_t slot = ref_alloc_attr_slot();
    if ( slot == REF_ATTR_INVALID ) return false;

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
        uint16_t expected = ( uint16_t )( *owner_index + *owner_count );
        if ( slot != expected )
        {
            fprintf( stderr,
                     "ref: FATAL attr contiguity violation: owner at attr_index=%u has %u attr(s), "
                     "expected next slot %u but pool allocated slot %u -- "
                     "all attrs for a type/field must be added before moving to the next owner\n",
                     (unsigned)*owner_index, (unsigned)*owner_count,
                     (unsigned)expected, (unsigned)slot );
            assert( 0 && "ref: attr contiguity violation -- see stderr for details" );
            g_ref.attr_count--; /* undo the allocation so the pool does not drift further */
            return false;
        }
        ( *owner_count )++;
    }
    g_ref.attrs[ slot ] = *attr;
    return true;
}

bool ref_type_add_attr( uint16_t type_id, const ref_attrib_t* attr )
{
    if ( type_id >= g_ref.type_count ) { assert( 0 ); return false; }
    ref_type_t* t = &g_ref.types[ type_id ];
    return ref_append_attr( &t->attr_index, &t->attr_count, attr );
}

bool ref_field_add_attr( uint16_t field_id, const ref_attrib_t* attr )
{
    if ( field_id >= g_ref.field_count ) { assert( 0 ); return false; }
    ref_field_t* f = &g_ref.fields[ field_id ];
    return ref_append_attr( &f->attr_index, &f->attr_count, attr );
}

/*==============================================================================================
    Frame Finalization

    Performs a second resolve pass over all fields in a frame after all types in that frame
    are registered. This makes forward references work: a struct that appears earlier than
    the type it references will still resolve correctly because by finalize time the entire
    frame's type table is populated.

    Must be called exactly once per frame, after the last ref_register_* call and before
    any lookup or serialization on types from that frame. ref_register_module() calls it
    automatically; manual registration code must call it explicitly.
==============================================================================================*/

bool
ref_finalize_frame( uint16_t frame_id )
{
    if ( frame_id >= g_ref.frame_count ) { assert( 0 && "ref_finalize_frame: bad frame id" ); return false; }

    const ref_frame_t* f = &g_ref.frames[ frame_id ];

    /* Determine the end of this frame's field block. The topmost frame owns everything
       from first_field to field_count; earlier frames end where the next one starts. */
    uint16_t field_end =
        ( frame_id + 1 == g_ref.frame_count ) ? g_ref.field_count : g_ref.frames[ frame_id + 1 ].first_field;

    /* First pass: resolve any field that still has type_id == REF_TYPE_INVALID.
       These are cross-frame references (e.g. fields using types from earlier frames)
       or within-frame forward refs not caught during registration. */
    for ( uint16_t i = f->first_field; i < field_end; i++ )
    {
        ref_field_t* fld = &g_ref.fields[ i ];
        if ( fld->type_id != REF_TYPE_INVALID ) continue;   /* already resolved */
        if ( fld->type_hash == 0 ) continue;               /* no base type (void) */

        uint16_t resolved = ref_hash_find( fld->type_hash );
        if ( resolved != REF_TYPE_INVALID )
        {
            fld->type_id = resolved;
            fld->kind    = g_ref.types[ resolved ].kind;
        }
    }

    /* Second pass: report any fields that are still unresolved as hard errors.
       This indicates a type referenced by name but never registered in any frame. */
    bool ok = true;
    for ( uint16_t i = f->first_field; i < field_end; i++ )
    {
        const ref_field_t* fld = &g_ref.fields[ i ];
        if ( fld->type_id == REF_TYPE_INVALID && fld->type_hash != 0 )
        {
            printf( "ref: ERROR unresolved field '%s' (type hash 0x%08x) in frame %u\n",
                    ref_cstr( fld->name_id ), fld->type_hash, frame_id );
            ok = false;
        }
    }
    return ok;
}

/*==============================================================================================
    Module Integration

    These two functions are the DLL-load / DLL-unload hooks that wire the mod system to the
    reflection registry. The host calls ref_wire_mod_callbacks() once at startup, which
    registers ref_register_module and ref_unregister_module as mod-load / mod-unload events.

    On DLL load:
      1. ref_ensure_init() guards against unusual startup order.
      2. A new frame is pushed for the module.
      3. The module's ref_register callback (from mod_desc_t.ref_register) is invoked with a
         vtable of registration functions -- this is how generated code stays decoupled from
         the ref_ internals.
      4. ref_finalize_frame() resolves any forward references.

    On DLL unload:
      The frame is simply popped, which bulk-frees all types/fields/attrs/enums for that
      module in O(1). The next hot-reload re-registers everything fresh.
==============================================================================================*/

uint16_t
ref_register_module( const char* name, const mod_desc_t* desc )
{
    if ( !name || !desc || !desc->ref_register )
        return REF_FRAME_INVALID;

    /* Lazy init guard: registration callbacks can fire before ref_mod_init if load order
       varies, so we must ensure the registry is ready before touching any pool. */
    ref_ensure_init();

    typedef void ( *ref_register_fn )( const ref_reg_api_t* api );
    ref_register_fn ref_reg = ( ref_register_fn )desc->ref_register;

    uint16_t frame = ref_push_frame( name );
    if ( frame == REF_FRAME_INVALID ) return REF_FRAME_INVALID;

    /* Build the registration vtable that generated DLL code calls into.
       This keeps the generated code independent of the internal ref_ implementation. */
    ref_reg_api_t api = {
        .intern                  = ref_intern,
        .ref_register_type       = ref_register_type,
        .ref_register_enum       = ref_register_enum,
        .ref_register_bitset     = ref_register_bitset,
        .ref_register_function   = ref_register_function,
        .ref_type_add_attr       = ref_type_add_attr,
        .ref_field_add_attr      = ref_field_add_attr,
        .ref_get_type            = ref_get_type,
    };

    ref_reg( &api );
    ref_finalize_frame( frame );
    return frame;
}

void
ref_unregister_module( const char* name )
{
    if ( !name || g_ref.frame_count <= 1 )
        return;

    uint16_t top = g_ref.frame_count - 1;
    if ( strcmp( ref_cstr( g_ref.frames[ top ].name_id ), name ) == 0 )
    {
        ref_pop_frame( top );
        return;
    }

    /* Check whether the name exists deeper in the stack. If it does, that is a real LIFO
       violation -- the pool watermark design cannot remove a middle frame without corrupting
       every higher frame's type/field/enum/attr indices. This means the mod system unloaded
       modules out of dependency order, which is always a bug in the caller. */
    for ( uint16_t i = 1; i < g_ref.frame_count; ++i )
    {
        if ( strcmp( ref_cstr( g_ref.frames[ i ].name_id ), name ) == 0 )
        {
            fprintf( stderr,
                     "ref: FATAL ref_unregister_module: '%s' is not the topmost frame (top='%s') -- "
                     "modules must unload in reverse dependency order\n",
                     name, ref_cstr( g_ref.frames[ top ].name_id ) );
            assert( 0 && "ref: out-of-order module unload -- modules must unload in reverse dependency order" );
            return;
        }
    }
    /* Name not found in any frame -- module registered no reflection types; silently ignore. */
}

// clang-format off
/*============================================================================================*/