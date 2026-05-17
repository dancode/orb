#ifndef RS_H
#define RS_H
/*==============================================================================================

    core/rs/rs.h : Reflection System (rs_) - public API.

    A lean reflection registry built around five core design principles:

    1. STACK-FRAME REGISTRY

       Each module pushes a registry frame on load and pops it on unload.
       Types, fields, and attributes are appended into contiguous tables;
       unloading simply rewinds the tables back to the frame's starting indices.

       This removes the need for:
           - validity flags
           - generation/version counters
           - tombstones or orphan records
           - deferred cleanup passes
           - full registry rebuilds

       Hot-reload becomes:
           push -> register -> finalize
           pop  -> truncate tables

       The registry behaves like a memory stack:
           - modules append data linearly
           - unload restores the previous top
           - no stale reflection entries survive reload
           - registration and teardown are O(1)

    2. PACKED MODIFIER CHAIN

       C declarators are recursive and order-sensitive:

           const int*
           int* const
           int (*ptr)[4]

       The registry preserves declarator order using a compact packed
       modifier stream instead of heap-allocated type trees.

       Each field stores up to four modifier operations inside a single
       16-bit value:
           - pointer
           - array
           - const
           - etc.

       Modifiers are encoded in declaration order, allowing the resolver
       to reconstruct the exact type shape without ambiguity.

       This avoids the common failure where systems collapse distinct
       declarations into the same representation.

    3. LAZY TYPE RESOLUTION

       During registration, fields reference types by hash rather than by
       immediate type ID. Resolution occurs later during
       rs_finalize_frame().

       This allows registration to proceed without dependency ordering:

           StructA -> StructB
           StructB -> StructA

       Types may freely reference types that have not yet been registered.

       After all registrations complete, the finalization pass resolves
       hashes into stable type IDs and patches all references in-place.

       Runtime field metadata therefore carries only resolved IDs;
       hash tables exist purely as registration scaffolding.

    4. SCHEMA HASH PER TYPE

       Every registered type generates a deterministic schema hash derived
       from its reflected layout:
           - field names
           - field offsets
           - field sizes
           - field types
           - modifiers
           - array counts

       Any structural change produces a different schema hash:
           - adding/removing fields
           - changing field types
           - reordering layout
           - modifying indirection

       This replaces manually maintained version numbers and enables
       automatic compatibility validation for:
           - save games
           - serialized assets
           - network replication
           - hot-reload safety

       Invalid layouts fail deterministically instead of corrupting memory.

    5. REPEATED FLAT ATTRIBUTES

       Reflection attributes are stored as contiguous flat entries rather
       than nested payload structures.

       Multi-value metadata is represented by repeated runs of the same
       attribute identifier:

           @range(0, 100)

       becomes:

           range -> 0
           range -> 100

       This keeps attribute storage:
           - linear
           - cache-friendly
           - trivially serializable
           - allocation-free

       The registry avoids secondary payload pools, variable-sized blobs,
       and pointer-heavy metadata graphs.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"

/*==============================================================================================
    rs_name_t  -  opaque u32 offset into the rs_ internal string pool.  Equal strings always
    map to the same id within a session.  rs_hash_str is a separate FNV-1a hash used for
    fast lookup; it is NOT the same value as an rs_name_t.

    rs_hash_str must remain algorithmically identical to sid_hash (case-insensitive FNV-1a).
==============================================================================================*/

typedef uint32_t rs_name_t;

static inline uint32_t
rs_hash_str( const char* s )
{
    uint32_t h = 2166136261u;
    while ( *s )
    {
        unsigned char c = ( unsigned char )*s++;
        if ( c >= 'A' && c <= 'Z' )
            c = ( unsigned char )( c + 32 );
        h = ( h ^ c ) * 16777619u;
    }
    return h;
}

// clang-format off
/*==============================================================================================
    Constants
==============================================================================================*/

#define RS_TYPE_INVALID         ((uint16_t)0xFFFF)
#define RS_FIELD_INVALID        ((uint16_t)0xFFFF)
#define RS_ATTR_INVALID         ((uint16_t)0xFFFF)
#define RS_FRAME_INVALID        ((uint16_t)0xFFFF)

#define RS_MAX_FRAMES           32
#define RS_MAX_TYPES            512
#define RS_MAX_FIELDS           4096
#define RS_MAX_ATTRS            1024
#define RS_MAX_ENUMS            1024

#define RS_TYPE_HASH_SIZE       1024                    // must be power of two
#define RS_TYPE_HASH_MASK       (RS_TYPE_HASH_SIZE - 1) // for modulo

/*==============================================================================================
    Primitive type IDs - reserved slots 0.. (RS_PRIM_COUNT - 1) in frame 0
==============================================================================================*/

typedef enum rs_prim_e
{
    RS_PRIM_INVALID = 0,
    RS_PRIM_VOID,
    RS_PRIM_BOOL,
    RS_PRIM_CHAR,
    RS_PRIM_I8,
    RS_PRIM_U8,
    RS_PRIM_I16,
    RS_PRIM_U16,
    RS_PRIM_I32,
    RS_PRIM_U32,
    RS_PRIM_I64,
    RS_PRIM_U64,
    RS_PRIM_F32,
    RS_PRIM_F64,
    RS_PRIM_STRING,
    RS_PRIM_COUNT,

} rs_prim_t;

/*==============================================================================================
    Type kind - what the base type IS. Mutually exclusive. Modifiers go in rs_field_t.mods.
==============================================================================================*/

typedef enum rs_kind_e
{
    RS_KIND_PRIM     = 0,
    RS_KIND_STRUCT   = 1,
    RS_KIND_ENUM     = 2,
    RS_KIND_BITSET   = 3,   /* flag-style enum: values OR together (bitmask) */
    RS_KIND_UNION    = 4,
    RS_KIND_FUNCTION = 5,   /* signature type; field[ 0 ] = return, field[ 1.. ] = params */

} rs_kind_t;

static inline bool
rs_kind_is_enum( rs_kind_t k )
{
    return k == RS_KIND_ENUM || k == RS_KIND_BITSET;
}

/*==============================================================================================
    Packed modifier chain (16 bits, 4 slots of 4 bits each)

        [ Bit 3 (Reserved) | Bit 2 (Const) | Bits 1-0 (Operation) ]
        
    Each slot:

        bit 0 and 1 : Operation (rs_mod_op_t) enum (NONE=0, PTR=1, ARRAY=2, FUNCTION=3)
        bit 2       : const-qualifies-this-wrapper (wrapper is const, e.g. `T* const`)
        bit 3       : reserved

        slot 0 is the INNERMOST wrapper around the base type; read low-to-high to walk outward.

        Examples (base type T):

        T               RS_MODS( 0, 0, 0, 0 )                     = 0000|0000|0000|0000  (none)
        T*              RS_MODS( RS_M_PTR, 0, 0, 0 )              = 0000|0000|0000|0001
        T**             RS_MODS( RS_M_PTR, RS_M_PTR, 0, 0 )       = 0000|0000|0001|0001
        T* const        RS_MODS( RS_M_CONST_PTR, 0, 0, 0 )        = 0000|0000|0000|0101
        T*[N]           RS_MODS( RS_M_PTR, RS_M_ARRAY, 0, 0 )     = 0000|0000|0010|0001  aux=N
        T(*)[N]         RS_MODS( RS_M_ARRAY, RS_M_PTR, 0, 0 )     = 0000|0000|0001|0010  aux=N
        const T*        RS_MODS( RS_M_PTR, 0, 0, 0 )              = 0000|0000|0000|0001  base_const=1
        const T* const* RS_MODS( RS_M_PTR, RS_M_CONST_PTR, 0, 0 ) = 0000|0000|0001|0101  base_const=1

        Multi-dimensional arrays (e.g. T[A][B]) are intentionally NOT supported:
        wrap inner dimension in a struct or typedef and reflect that.
==============================================================================================*/

typedef enum rs_mod_op_e
{
    RS_MOD_NONE     = 0,
    RS_MOD_PTR      = 1,
    RS_MOD_ARRAY    = 2,
    RS_MOD_FUNCTION = 3,

} rs_mod_op_t;

/* Slot packing macros */
#define RS_MOD_SLOT( op, is_const )  (((op) & 0x3) | (((is_const) & 0x1) << 2))
#define RS_MODS( s0, s1, s2, s3 )    ((uint16_t)((s0) | ((s1) << 4) | ((s2) << 8) | ((s3) << 12)))
#define RS_M_END                     RS_MOD_SLOT( RS_MOD_NONE,     0 )
#define RS_M_PTR                     RS_MOD_SLOT( RS_MOD_PTR,      0 )
#define RS_M_CONST_PTR               RS_MOD_SLOT( RS_MOD_PTR,      1 )    // `T* const`  - the pointer itself is const
#define RS_M_ARRAY                   RS_MOD_SLOT( RS_MOD_ARRAY,    0 )
#define RS_M_FUNCTION                RS_MOD_SLOT( RS_MOD_FUNCTION, 0 )
#define RS_NO_MODS                   ((uint16_t)0)

/* Slot accessors */
#define RS_MOD_GET( mods, slot )     ((uint8_t)(((mods) >> ((slot) * 4)) & 0xF))
#define RS_MOD_OP( slot_bits )       ((rs_mod_op_t)((slot_bits) & 0x3))
#define RS_MOD_IS_CONST( slot_bits ) (((slot_bits) >> 2) & 0x1)

/*==============================================================================================
    Attribute payload (single-valued; lists are runs of same-named entries)

    Attribute type value union is always 4 bytes, so larger types (e.g. double) must be
    split across multiple attributes. Strings are interned and stored as sids.
==============================================================================================*/

typedef enum rs_attr_type_e
{
    RS_ATTR_NONE   = 0,
    RS_ATTR_INT    = 1,
    RS_ATTR_FLOAT  = 2,
    RS_ATTR_BOOL   = 3,
    RS_ATTR_STRING = 4,

} rs_attr_type_t;

typedef struct rs_attrib_s
{
    rs_name_t name_id;   // interned attribute name
    uint32_t  name_hash; // rs_hash_str(name) for intern-free lookup
    uint8_t   type;      // rs_attr_type_t
    uint8_t   _pad[ 3 ];

    union
    {
        int32_t   i32;
        float     f32;
        rs_name_t str;   // interned string value
        uint32_t  u32;
    } value;

} rs_attrib_t;

/*==============================================================================================
    Enumerator - one entry per "name = value" pair inside an enum type.

    Stored in a dedicated table; An enum-kind rs_type_t reuses field_index/field_count
    to slice into that table (no fields can coexist on an enum type, so the slot is free).
    Underlying integer size is carried by rs_type_t.size (e.g. sizeof(int) for plain enums).
==============================================================================================*/

typedef struct rs_enum_s
{
    rs_name_t name_id;      // interned enumerator name
    uint32_t  name_hash;    // rs_hash_str(name) for intern-free lookup
    int64_t   value;        // signed; covers unsigned values up to INT64_MAX

} rs_enum_t;

/*==============================================================================================
    Field record - 20 bytes
==============================================================================================*/

typedef struct rs_field_s
{
    rs_name_t name_id;      // interned field name
    uint32_t  name_hash;    // rs_hash_str(name) for intern-free lookup
    uint32_t  type_hash;    // rs_hash_str(base_type_name) for lazy resolution
    uint16_t  type_id;      // resolved base type (RS_TYPE_INVALID until finalize)
    uint16_t  offset;       // byte offset within parent struct
    uint16_t  size;         // sizeof(field), including any inline array
    uint16_t  mods;         // packed modifier chain (see above)
    uint16_t  aux;          // array element count, or function signature id
    uint8_t   base_const;   // const on the base type itself (`const T x`)
    uint8_t   kind;         // cached rs_kind_t of base, for fast dispatch
    uint16_t  attr_index;   // first attribute index (RS_ATTR_INVALID if none)
    uint16_t  attr_count;   // number of attributes

} rs_field_t;

/*==============================================================================================
    Type record - 32 bytes
==============================================================================================*/

typedef struct rs_type_s
{
    rs_name_t name_id;      // interned type name
    uint32_t hash;          // rs_hash_str(name) - persistent identity
    uint32_t schema_hash;   // content hash over fields (for save-game compat)

    uint16_t field_index;   // first member: index into fields[] (struct/union) or enums[] (enum)
    uint16_t field_count;   // number of members/enum values
    uint16_t attr_index;    // first attribute (RS_ATTR_INVALID if none)
    uint16_t attr_count;    // number of attributes
    uint16_t next;          // next index in hash chain (RS_TYPE_INVALID = end)
    uint16_t size;          // sizeof(T)

    uint8_t  align;         // alignof(T)
    uint8_t  kind;          // rs_kind_t
    uint8_t  frame_id;      // owning frame (module)
    uint8_t  _pad;          // 

} rs_type_t;

/*==============================================================================================
    Frame record - one per loaded module
==============================================================================================*/

typedef struct rs_frame_s
{
    rs_name_t name_id;      // interned name of module owning this frame (e.g. "core", "game")
    uint32_t version;       // caller-supplied
    void*    dll_handle;    // platform handle (NULL for system frame)

    uint16_t first_type;    // table truncation marks
    uint16_t first_field;   //
    uint16_t first_attr;    //
    uint16_t first_enum;    // reserved for enum table

} rs_frame_t;

/*==============================================================================================
    Authoring helpers (used by codegen)
==============================================================================================*/

#define RS_OFFSETOF( T, m )   ( ( uint16_t )offsetof( T, m ) )
#define RS_SIZEOF( T )        ( ( uint16_t )sizeof( T ) )
#define RS_ALIGNOF( T )       ( ( uint8_t )_Alignof( T ) )
#define RS_FIELD_SIZE( T, m ) ( ( uint16_t )sizeof( ( ( T* )0 )->m ) )
#define RS_ARRAY_COUNT( a )   ( ( uint16_t )( sizeof( a ) / sizeof( ( a )[ 0 ] ) ) )

/*==============================================================================================
    rs_hash_str - case-insensitive FNV-1a; call sites compute hashes inline with no
    function-pointer indirection.
==============================================================================================*/

/*==============================================================================================
    Lifecycle

    rs_init() zeroes the registry, installs the internal string pool, pushes the system
    frame, and registers all built-in primitive types. It is idempotent and self-invoked
    on first use, so hosts and modules never need to call it explicitly. rs_exit() tears
    the registry down — typically called by rs.mod_exit during mod_system_exit.

        // typical: no explicit init call — first rs_register_module bootstraps the registry
        rs_register_module( name, desc );    // first call triggers internal rs_init
        ...
        rs_exit();                           // optional; mod_system_exit handles it
==============================================================================================*/

void rs_init( void );
void rs_exit( void );

/*==============================================================================================
    Registration API  -  function pointer bundle passed to the generated <name>_rs_register().

    engine_rs is a static library that links only into the host.  DLL modules must not call
    registration functions directly.  rs_register_module fills this struct and passes it
    into the module's generated registrar, which calls back through the pointers.
==============================================================================================*/

typedef struct rs_reg_api_s
{
    rs_name_t        ( *intern            )( const char* );
    uint16_t         ( *rs_register_type   )( const rs_type_t*, const rs_field_t*, uint16_t );
    uint16_t         ( *rs_register_enum   )( const rs_type_t*, const rs_enum_t*, uint16_t );
    uint16_t         ( *rs_register_bitset )( const rs_type_t*, const rs_enum_t*, uint16_t );
    bool             ( *rs_type_add_attr   )( uint16_t type_id, const rs_attrib_t* );
    bool             ( *rs_field_add_attr  )( uint16_t field_id, const rs_attrib_t* );
    const rs_type_t* ( *rs_get_type        )( uint16_t type_id );

} rs_reg_api_t;

/*==============================================================================================
    Module reflection integration

    One entry point for both static and dynamic modules. The mod_desc_t.rs_register slot
    carries the generated registrar function pointer, which lives in the same address
    space as the desc (the exe for statics, the DLL image for dynamics). No symbol
    lookup, no DLL-vs-static split.

    rs_register_module    - if desc->rs_register is non-NULL: pushes a frame named `name`,
                            builds an rs_reg_api_t from the live registry functions, calls
                            desc->rs_register through that, then finalizes the frame.
                            Returns the new frame id, or RS_FRAME_INVALID if no reflection
                            was registered.
    rs_unregister_module  - pops the frame owned by `name` (must be the topmost frame).

    Designed to be wired as mod_set_pre_init_cb / mod_set_post_exit_cb targets so
    reflection tracks every module — static or dynamic — without engine_mod knowing
    about rs_. See rs_host.h.
==============================================================================================*/

uint16_t rs_register_module  ( const char* name, const mod_desc_t* desc );
void     rs_unregister_module( const char* name );

/*==============================================================================================
    Frames
==============================================================================================*/

uint16_t          rs_push_frame         ( const char* name, uint32_t version );
void              rs_pop_frame          ( uint16_t frame_id );
bool              rs_finalize_frame     ( uint16_t frame_id );    // resolve fields, return false on error
const rs_frame_t* rs_get_frame          ( uint16_t frame_id );

/*==============================================================================================
    Low-level registration  (used by generated code; also available
    for hand-rolled registration).

    Each rs_field_t carries its own type_hash (rs_hash_str of the base type name).
    Set field.type_hash before calling; no separate hash array is needed.
==============================================================================================*/

uint16_t rs_register_type( const rs_type_t*  type,
                           const rs_field_t* fields,
                           uint16_t          field_count );

bool rs_type_add_attr( uint16_t type_id, const rs_attrib_t* attr );
bool rs_field_add_attr( uint16_t field_id, const rs_attrib_t* attr );

/* Register an enum type. `type->kind` is forced to RS_KIND_ENUM. */
uint16_t rs_register_enum( const rs_type_t* type, const rs_enum_t* enums, uint16_t count );

/* Register a bitset enum (flag-style: values OR together).
   `type->kind` is forced to RS_KIND_BITSET. */
uint16_t rs_register_bitset( const rs_type_t* type, const rs_enum_t* enums, uint16_t count );

/* Register a function signature. `type->kind` is forced to
   RS_KIND_FUNCTION. `fields[0]` is the return type; `fields[1..]`
   are parameters in declaration order. Each field carries its own type_hash.
   A function-pointer FIELD references this signature by storing
   its type_id in the field's `aux` slot, with RS_MOD_FUNCTION
   in the modifier chain. */
uint16_t rs_register_function( const rs_type_t*  type,
                               const rs_field_t* return_then_params,
                               uint16_t          count );

/*==============================================================================================
    Lookup
==============================================================================================*/

/* Intern a string into the rs_ string pool and return its rs_name_t.
   Hand-rolled registration uses this directly; generated code calls api->intern. */
rs_name_t           rs_intern( const char* s );

/* Recover the string for a name_id (reads from the rs_ internal string pool). */
const char*         rs_cstr( rs_name_t id );

const rs_type_t*    rs_get_type( uint16_t type_id );
uint16_t            rs_find_type( uint32_t name_hash );
uint16_t            rs_find_type_by_name( const char* name );

const rs_field_t*   rs_get_field( uint16_t field_id );
const rs_field_t*   rs_find_field( uint16_t type_id, const char* name );

const rs_attrib_t*  rs_type_get_attr( uint16_t type_id, const char* name );
const rs_attrib_t*  rs_field_get_attr( uint16_t field_id, const char* name );

const rs_enum_t*    rs_enum_find_by_name( uint16_t type_id, const char* name );
const rs_enum_t*    rs_enum_find_by_value( uint16_t type_id, int64_t value );
const rs_enum_t*    rs_get_enumerator( uint16_t enum_id );

/* Bitset-style enum helpers (type must have kind == RS_KIND_BITSET). */
typedef void ( *rs_enum_cb_t )( uint16_t enum_id, const rs_enum_t* e, void* user );

bool                rs_enum_is_bitset( uint16_t type_id );
const rs_enum_t*    rs_bitset_find_flag( uint16_t type_id, int64_t mask );
uint16_t            rs_bitset_each_set_flag( uint16_t type_id, int64_t value, rs_enum_cb_t cb, void* user );

                    /* "FLAG_A | FLAG_C" into buf; unrecognized bits emitted as hex. */
size_t              rs_bitset_describe( uint16_t type_id, int64_t value, char* buf, size_t buf_size );

                    /* Function-signature convenience accessors. */
const rs_field_t*   rs_function_get_return( uint16_t type_id );
uint16_t            rs_function_param_count( uint16_t type_id );
const rs_field_t*   rs_function_get_param( uint16_t type_id, uint16_t param_index );

void                rs_get_stats( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count );

/*==============================================================================================
    Iteration
==============================================================================================*/

typedef void ( *rs_type_cb_t )( uint16_t type_id, const rs_type_t* t, void* user );
typedef void ( *rs_field_cb_t )( uint16_t field_id, const rs_field_t* f, void* user );

/* rs_enum_cb_t is forward-declared in the bitset block above. */

uint16_t            rs_each_type            ( rs_type_cb_t cb, void* user );
uint16_t            rs_each_type_in_frame   ( uint16_t frame_id, rs_type_cb_t cb, void* user );
uint16_t            rs_each_field           ( uint16_t type_id, rs_field_cb_t cb, void* user );
uint16_t            rs_each_enumerator      ( uint16_t type_id, rs_enum_cb_t cb, void* user );

/*==============================================================================================
    Reference walker

    Visits each pointer-bearing slot inside an instance. The walker DOES NOT follow
    pointers; it discovers them and hands them to the visitor, which decides whether
    to chase, mark, fix up, or ignore. Recurses into nested structs and inline arrays
    of structs automatically.

    Supported field shapes:
        T  field                value of base type   -> recurse if base is struct/union
        T* field                single pointer       -> one visit
        T  field[N]             inline array         -> recurse N times if base is struct
        T* field[N]             array of pointers    -> N visits
        T (*field)[N]           pointer to array     -> one visit

    Function pointers and >2-level modifier chains are skipped.
==============================================================================================*/

typedef void ( *rs_ref_visitor_t )( void** ref_slot, uint16_t pointee_type_id, const rs_field_t* field, void* user );

void rs_walk_refs( void* instance, uint16_t type_id, rs_ref_visitor_t visit, void* user );

/*==============================================================================================
    Value walker

    Visits every field of an instance, recursing into nested structs and each element of
    inline arrays. The visitor is called once per logical value with its address and type.

    Supported field shapes:
        T  field                bare value           -> one visit; recurse if struct/union
        T* field                pointer              -> one visit  (addr of the pointer var)
        T  field[N]             inline array         -> N visits;  recurse if struct/union
        T* field[N]             array of pointers    -> N visits  (addr of each pointer slot)
        T (*field)[N]           pointer to array     -> one visit

    Function pointers and >2-level modifier chains are visited once as opaque slots.
    Use field->mods to distinguish value shapes inside the visitor.
==============================================================================================*/

typedef void ( *rs_visitor_t )( void* addr, uint16_t type_id, const rs_field_t* field, void* user );

void rs_walk( void* instance, uint16_t type_id, rs_visitor_t visit, void* user );

/*==============================================================================================
    Serialization

      Format:  [ 20-byte header ] [ body bytes ]
        u32 magic        = RS_SAVE_MAGIC ('rs01' little-endian)
        u32 type_hash    = identity (sid_hash of type name)
        u32 schema_hash  = layout content hash
        u32 body_size    = sizeof(T)  (must equal current sizeof(T))
        u32 reserved     = 0

      Body is the raw struct bytes. Pointer slots and fields carrying the @transient
      attribute are zeroed in the saved body. Compatibility is gated on schema_hash:
      if it differs from the current registered schema, the read is rejected.

      Pointers are NOT followed. A T* field on read returns to NULL. Save formats
      that need to persist references should use stable IDs/handles, not raw pointers.
==============================================================================================*/

#define RS_SAVE_MAGIC       0x31307372u /* 'rs01' little-endian */
#define RS_SAVE_HEADER_SIZE 20

typedef enum rs_io_status_e
{
    RS_IO_OK        = 0,
    RS_IO_INCOMPAT  = 1, /* magic / type_hash / schema_hash / body_size mismatch */
    RS_IO_TRUNCATED = 2, /* buffer ran out */
    RS_IO_NO_TYPE   = 3, /* expected_type_id not registered */
    RS_IO_BAD_ARG   = 4,
} rs_io_status_t;

/* Serialize. Returns bytes written, or 0 on error. */
size_t rs_write( const void* instance, uint16_t type_id, uint8_t* buf, size_t cap );

/* Deserialize. *bytes_read is set on RS_IO_OK. */
rs_io_status_t rs_read( void* instance, uint16_t expected_type_id, const uint8_t* buf, size_t cap, size_t* bytes_read );

/* Peek the type_hash from a saved blob without reading. */
uint32_t rs_peek_type_hash( const uint8_t* buf, size_t cap );

/*==============================================================================================
    Diagnostics
==============================================================================================*/

void rs_print_types( void );
void rs_print_type( uint16_t type_id );
void rs_print_frame( uint16_t frame_id );

// Pretty-print a field's full type (e.g. "const vec3_t*[8]")
// into `buf`. Returns characters written (excl. terminator).
size_t rs_field_describe( const rs_field_t* f, char* buf, size_t buf_size );

/*==============================================================================================
    Test entry
==============================================================================================*/

void rs_run_tests( void );

/*==============================================================================================
    rs_api_t - runtime API struct (accessible to DLL modules via rs_api())

    Registration functions are NOT exposed here; DLL modules receive an rs_reg_api_t*
    callback vtable during their rs_register() call and must use that instead.
==============================================================================================*/

typedef struct rs_api_s
{
    /* Lookup */
    uint16_t           ( *find_type_by_name  )( const char* name );
    const rs_type_t*   ( *get_type           )( uint16_t type_id );
    const rs_field_t*  ( *get_field          )( uint16_t field_id );
    const rs_field_t*  ( *find_field         )( uint16_t type_id, const char* name );
    const rs_attrib_t* ( *type_get_attr      )( uint16_t type_id, const char* key );
    const rs_attrib_t* ( *field_get_attr     )( uint16_t field_id, const char* key );
    rs_name_t          ( *intern             )( const char* s );
    const char*        ( *cstr              )( rs_name_t id );

    /* Iteration */
    uint16_t           ( *each_type          )( rs_type_cb_t cb, void* user );
    uint16_t           ( *each_type_in_frame )( uint16_t frame_id, rs_type_cb_t cb, void* user );
    uint16_t           ( *each_field         )( uint16_t type_id, rs_field_cb_t cb, void* user );
    uint16_t           ( *each_enumerator    )( uint16_t type_id, rs_enum_cb_t cb, void* user );

    /* Bitset helpers */
    size_t             ( *bitset_describe    )( uint16_t type_id, int64_t value, char* buf, size_t cap );

    /* Walkers */
    void               ( *walk_refs          )( void* inst, uint16_t type_id, rs_ref_visitor_t fn, void* user );
    void               ( *walk               )( void* inst, uint16_t type_id, rs_visitor_t fn, void* user );

    /* Serialization */
    size_t             ( *write              )( const void* src, uint16_t type_id, uint8_t* out, size_t cap );
    rs_io_status_t     ( *read               )( void* dst, uint16_t type_id, const uint8_t* buf, size_t cap, size_t* consumed );
    uint32_t           ( *peek_type_hash     )( const uint8_t* buf, size_t cap );

    /* Diagnostics */
    size_t             ( *field_describe     )( const rs_field_t* f, char* buf, size_t cap );
    void               ( *print_type         )( uint16_t type_id );
    void               ( *print_types        )( void );

} rs_api_t;

/* rs is always statically linked into the host — RS_STATIC is set by CMake globally. */
#if defined( BUILD_STATIC ) || defined( RS_STATIC )
MOD_GATEWAY_STATIC( rs_api_t, rs )
#else
MOD_GATEWAY_DYNAMIC( rs_api_t, rs )
#endif

/*==============================================================================================
    Reflection annotation macros  -  defined in orb.h (included above via orb.h -> here).

    RS_STRUCT / RS_ENUM / RS_BITSET / RS_PROP / RS_VAR all expand to nothing at compile
    time; rs_gen parses them at build time.  See orb.h for the definitions and usage examples.
==============================================================================================*/

/*==============================================================================================
    Module reflection wire-up

    The generated <name>_rs_register() lives in the same image (exe for statics, DLL for
    dynamics) as the module's mod_desc_t. Modules expose it by setting the rs_register slot:

        #include "<name>.generated.h"     // declares <name>_rs_register

        static mod_desc_t s_<name>_mod_desc = {
            .func_api    = &g_<name>_api_struct,
            .rs_register = MOD_RS_REGISTER( <name> ),
            ...
        };

    The host's rs_wire_mod_callbacks() (rs_host.h) installs a load callback that reads the
    slot and calls rs_register_module(). One mechanism for both static and dynamic builds.
==============================================================================================*/

/*============================================================================================*/
#endif    // RS_H
