/*==============================================================================================

    engine/rs/rs.h - Reflection system public API. See rs.md for design and usage.

    Note: rs.h is included by generated registration code.

==============================================================================================*/
#ifndef RS_H
#define RS_H

#include "orb.h"
#include "engine/mod/mod.h"

// clang-format off
/*==============================================================================================
    String Pool

    rs_name_t: opaque u32 byte offset into the rs_ internal string pool.
    Equal strings always map to the same id within a session.
    NOT the same as a hash value — do not compare with rs_hash_str results.
==============================================================================================*/

typedef uint32_t rs_name_t;

/*==============================================================================================
    rs_hash_str — case-insensitive FNV-1a

    Used for intern-free lookup of types, fields, and attributes by name.
    Must remain algorithmically identical to sid_hash.
==============================================================================================*/

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

/*==============================================================================================
    Constants
==============================================================================================*/

/* Sentinels */
#define RS_TYPE_INVALID         ( (uint16_t)0xFFFF )
#define RS_FIELD_INVALID        ( (uint16_t)0xFFFF )
#define RS_ATTR_INVALID         ( (uint16_t)0xFFFF )
#define RS_FRAME_INVALID        ( (uint16_t)0xFFFF )

/* Table limits */
#define RS_MAX_FRAMES           32
#define RS_MAX_TYPES            512
#define RS_MAX_FIELDS           4096
#define RS_MAX_ATTRS            1024
#define RS_MAX_ENUMS            1024

/* Hash table */
#define RS_TYPE_HASH_SIZE       1024                        /* must be power of two */
#define RS_TYPE_HASH_MASK       ( RS_TYPE_HASH_SIZE - 1 )

/*==============================================================================================
    Primitive Type IDs

    Fixed slots 0..(RS_PRIM_COUNT-1) in frame 0 ("reflect").
    type_id == rs_prim_t enum value for each primitive (e.g. RS_PRIM_F32 == type_id for float).
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
    Type Kind

    What the base type IS. Modifier indirection lives in rs_field_t.mods.
==============================================================================================*/

typedef enum rs_kind_e
{
    RS_KIND_PRIM     = 0,       /* any basic rs_prim_t type */
    RS_KIND_STRUCT   = 1,
    RS_KIND_ENUM     = 2,
    RS_KIND_BITSET   = 3,       /* flag-style enum: values OR together */
    RS_KIND_UNION    = 4,   
    RS_KIND_FUNCTION = 5,       /* signature type: field[0]=return, field[1..]=params */

} rs_kind_t;

static inline bool
rs_kind_is_enum( rs_kind_t k )
{
    return k == RS_KIND_ENUM || k == RS_KIND_BITSET;
}

/*==============================================================================================
    Packed Modifier Chain

    16 bits encoding up to four declarator-modifier slots of 4 bits each.
    Slot 0 is the innermost wrapper around the base type; read low->high to walk outward.

    Each slot:
        bits[1:0]  operation NONE = 0, PTR = 1, ARRAY = 2, FUNCTION = 3
        bit[2]     const on this wrapper (e.g. T* const)
        bit[3]     reserved

    See rs.md for encoding examples and the full table.
==============================================================================================*/

typedef enum rs_mod_op_e
{
    RS_MOD_NONE     = 0,
    RS_MOD_PTR      = 1,
    RS_MOD_ARRAY    = 2,
    RS_MOD_FUNCTION = 3,

} rs_mod_op_t;

/* Slot builders -- used to manually construct modifier chains */

#define RS_MOD_SLOT( op, is_const )   ( ((op) & 0x3) | (((is_const) & 0x1) << 2) )
#define RS_MODS( s0, s1, s2, s3 )     ( (uint16_t)((s0) | ((s1) << 4) | ((s2) << 8) | ((s3) << 12)) )
#define RS_M_END                      RS_MOD_SLOT( RS_MOD_NONE,     0 )
#define RS_M_PTR                      RS_MOD_SLOT( RS_MOD_PTR,      0 )
#define RS_M_CONST_PTR                RS_MOD_SLOT( RS_MOD_PTR,      1 )   /* T* const — the pointer is const */
#define RS_M_ARRAY                    RS_MOD_SLOT( RS_MOD_ARRAY,    0 )
#define RS_M_FUNCTION                 RS_MOD_SLOT( RS_MOD_FUNCTION, 0 )
#define RS_NO_MODS                    ( (uint16_t)0 )

/* Slot accessors -- used to extract information from a packed modifier chain */

#define RS_MOD_GET( mods, slot )      ( (uint8_t)(((mods) >> ((slot) * 4)) & 0xF) )
#define RS_MOD_OP( slot_bits )        ( (rs_mod_op_t)((slot_bits) & 0x3) )
#define RS_MOD_IS_CONST( slot_bits )  ( ((slot_bits) >> 2) & 0x1 )

/*==============================================================================================
    Attribute

    Single 4-byte payload entry. Multi-value metadata (@range(0, 100)) uses repeated
    entries with the same name_id. Larger types are split across multiple entries.
    String values are interned into the rs_ string pool.
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
    rs_name_t  name_id;         // interned attribute name
    uint32_t   name_hash;       // rs_hash_str(name) for intern-free lookup
    uint8_t    type;            // rs_attr_type_t
    uint8_t    _pad[ 3 ];       // reserved
    union
    {
        int32_t   i32;
        float     f32;
        rs_name_t str;          // interned string value (u32)
        uint32_t  u32;
    } value;

} rs_attrib_t;

/*==============================================================================================
    Enumerator

    One "name = value" entry inside an enum or bitset type.
    Enum types reuse rs_type_t.field_index/field_count to slice into enums[] storage.
    Underlying integer size is carried by rs_type_t.size.
==============================================================================================*/

typedef struct rs_enum_s
{
    rs_name_t  name_id;         // interned enumerator name
    uint32_t   name_hash;       // rs_hash_str(name) for intern-free lookup
    int64_t    value;           // signed; covers unsigned values up to INT64_MAX

} rs_enum_t;

/*==============================================================================================
    Field Record  (24 bytes)
==============================================================================================*/

typedef struct rs_field_s
{
    rs_name_t  name_id;         // interned field name
    uint32_t   type_hash;       // rs_hash_str(base_type_name) — used during lazy resolution
    uint16_t   type_id;         // resolved base type (RS_TYPE_INVALID until finalize)
    uint16_t   offset;          // byte offset within parent struct
    uint16_t   size;            // sizeof(field), including any inline array
    uint16_t   mods;            // packed modifier chain
    uint16_t   aux;             // array element count, or function signature type_id
    uint8_t    base_const;      // const on the base type itself (`const T x`)
    uint8_t    kind;            // cached rs_kind_t of base, for fast dispatch
    uint16_t   attr_index;      // first attribute index (RS_ATTR_INVALID if none)
    uint16_t   attr_count;      // number of attributes

} rs_field_t;

/*==============================================================================================
    Type Record  (28 bytes)
==============================================================================================*/

typedef struct rs_type_s
{
    rs_name_t  name_id;         // interned type name
    uint32_t   hash;            // rs_hash_str(name) — persistent identity
    uint32_t   schema_hash;     // content hash of field layout (save-game / hot-reload compat)
    uint16_t   field_index;     // first entry in fields[] (struct/union) or enums[] (enum)
    uint16_t   field_count;     // number of fields or enumerators in this type
    uint16_t   attr_index;      // first attribute (RS_ATTR_INVALID if none)
    uint16_t   attr_count;      // number of attributes on the type itself (not counting fields)
    uint16_t   next;            // next type in hash chain (RS_TYPE_INVALID = end)
    uint16_t   size;            // sizeof(T)
    uint8_t    align;           // alignof(T)
    uint8_t    kind;            // rs_kind_t (prim, struct, enum, etc.)
    uint8_t    frame_id;        // owning frame (module) — used for cleanup on unload
    uint8_t    _pad;            // padding for alignment

} rs_type_t;

/*==============================================================================================
    Frame Record

    One per loaded module. Pop truncates all four tables back to the first_* marks.
==============================================================================================*/

typedef struct rs_frame_s
{
    rs_name_t  name_id;         // interned module name; matches the module name passed to rs_push_frame
    uint16_t   first_type;      // start of modules types - pop rewinds to these points
    uint16_t   first_field;     // start of modules fields
    uint16_t   first_attr;      // start of modules attributes
    uint16_t   first_enum;      // start of modules enums

} rs_frame_t;

/*==============================================================================================
    Codegen Helpers : Used by generated reflection code.
==============================================================================================*/

#define RS_OFFSETOF( T, m )    ( (uint16_t)offsetof( T, m ) )
#define RS_SIZEOF( T )         ( (uint16_t)sizeof( T ) )
#define RS_ALIGNOF( T )        ( (uint8_t)_Alignof( T ) )
#define RS_FIELD_SIZE( T, m )  ( (uint16_t)sizeof( ((T*)0)->m ) )
#define RS_ARRAY_COUNT( a )    ( (uint16_t)( sizeof( a ) / sizeof( (a)[0] ) ) )

/*==============================================================================================
    Lifecycle

    Self-bootstraps on first rs_register_module call — hosts need not call rs_init explicitly.
    rs_init is idempotent and available for test setups that need a clean registry.
==============================================================================================*/

void                rs_init                 ( void );
void                rs_exit                 ( void );

/*==============================================================================================
    Registration API

    Vtable passed to generated <name>_rs_register() functions. DLL modules must call through
    this instead of calling rs_register_type etc. directly (those access g_rs in the host).
==============================================================================================*/

typedef struct rs_reg_api_s
{
    rs_name_t           ( *intern             )( const char* );
    uint16_t            ( *rs_register_type   )( const rs_type_t*, const rs_field_t*, uint16_t );
    uint16_t            ( *rs_register_enum   )( const rs_type_t*, const rs_enum_t*,  uint16_t );
    uint16_t            ( *rs_register_bitset )( const rs_type_t*, const rs_enum_t*,  uint16_t );
    bool                ( *rs_type_add_attr   )( uint16_t type_id,  const rs_attrib_t* );
    bool                ( *rs_field_add_attr  )( uint16_t field_id, const rs_attrib_t* );
    const rs_type_t*    ( *rs_get_type        )( uint16_t type_id );

} rs_reg_api_t;

/*==============================================================================================
    Module Integration

    Push/pop a module's reflection frame. Called automatically by rs_wire_mod_callbacks()
    (rs_host.h) — hosts do not need to call these directly.
==============================================================================================*/

uint16_t            rs_register_module      ( const char* name, const mod_desc_t* desc );
void                rs_unregister_module    ( const char* name );

/*==============================================================================================
    Frames
==============================================================================================*/

uint16_t            rs_push_frame           ( const char* name );
void                rs_pop_frame            ( uint16_t frame_id );
bool                rs_finalize_frame       ( uint16_t frame_id );   /* resolve forward refs; false on error */
const rs_frame_t*   rs_get_frame            ( uint16_t frame_id );

/*==============================================================================================
    Registration

    Low-level entry points used by generated code and hand-rolled registration.
    Set field.type_hash = rs_hash_str(base_type_name) before calling.
    Attributes must be added contiguously per owner (all of type A's before type B's).
==============================================================================================*/

uint16_t            rs_register_type        ( const rs_type_t*, const rs_field_t*, uint16_t field_count );
uint16_t            rs_register_enum        ( const rs_type_t*, const rs_enum_t*,  uint16_t count );
uint16_t            rs_register_bitset      ( const rs_type_t*, const rs_enum_t*,  uint16_t count );
uint16_t            rs_register_function    ( const rs_type_t*, const rs_field_t* return_then_params, uint16_t count );
bool                rs_type_add_attr        ( uint16_t type_id,  const rs_attrib_t* );
bool                rs_field_add_attr       ( uint16_t field_id, const rs_attrib_t* );

/*==============================================================================================
    String Pool
==============================================================================================*/

rs_name_t           rs_intern               ( const char* s );  /* intern into pool; generated code calls api->intern */
const char*         rs_cstr                 ( rs_name_t id );   /* direct pointer into pool — no copy */

/*==============================================================================================
    Lookup
==============================================================================================*/

const rs_type_t*    rs_get_type             ( uint16_t type_id );
uint16_t            rs_find_type            ( uint32_t name_hash );
uint16_t            rs_find_type_by_name    ( const char* name );
const rs_field_t*   rs_get_field            ( uint16_t field_id );
const rs_field_t*   rs_find_field           ( uint16_t type_id,  const char* name );
const rs_attrib_t*  rs_type_get_attr        ( uint16_t type_id,  const char* name );
const rs_attrib_t*  rs_field_get_attr       ( uint16_t field_id, const char* name );
const rs_enum_t*    rs_enum_find_by_name    ( uint16_t type_id,  const char* name );
const rs_enum_t*    rs_enum_find_by_value   ( uint16_t type_id,  int64_t value );
const rs_enum_t*    rs_get_enumerator       ( uint16_t enum_id );
void                rs_get_stats            ( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count );

/*==============================================================================================
    Bitset Helpers  (type must have kind == RS_KIND_BITSET)

    Greedy bit-claim: registration order determines priority when flags have overlapping bits.
    Place multi-bit constants before their single-bit components to match them first.
==============================================================================================*/

typedef void ( *rs_enum_cb_t )( uint16_t enum_id, const rs_enum_t* e, void* user );

bool                rs_enum_is_bitset       ( uint16_t type_id );
const rs_enum_t*    rs_bitset_find_flag     ( uint16_t type_id, int64_t mask );
uint16_t            rs_bitset_each_set_flag ( uint16_t type_id, int64_t value, rs_enum_cb_t cb, void* user );
size_t              rs_bitset_describe      ( uint16_t type_id, int64_t value, char* buf, size_t buf_size );

/*==============================================================================================
    Function Signature Accessors  (type must have kind == RS_KIND_FUNCTION)
==============================================================================================*/

const rs_field_t*   rs_function_get_return  ( uint16_t type_id );
uint16_t            rs_function_param_count ( uint16_t type_id );
const rs_field_t*   rs_function_get_param   ( uint16_t type_id, uint16_t param_index );

/*==============================================================================================
    Iteration
==============================================================================================*/

typedef void ( *rs_type_cb_t  ) ( uint16_t type_id,  const rs_type_t*  t, void* user );
typedef void ( *rs_field_cb_t ) ( uint16_t field_id, const rs_field_t* f, void* user );

uint16_t            rs_each_type            ( rs_type_cb_t  cb, void* user );
uint16_t            rs_each_type_in_frame   ( uint16_t frame_id, rs_type_cb_t cb, void* user );
uint16_t            rs_each_field           ( uint16_t type_id,  rs_field_cb_t cb, void* user );
uint16_t            rs_each_enumerator      ( uint16_t type_id,  rs_enum_cb_t  cb, void* user );

/*==============================================================================================
    Walkers

    rs_walk_refs — visits every pointer-bearing slot in an instance. Does NOT follow pointers;
    hands each slot to the visitor. Recurses into nested structs and inline arrays of structs.
    Supported shapes: T, T*, T[N], T*[N], T(*)[N]. Function pointers and deep chains skipped.

    rs_walk — visits every field value. Recurses into nested structs. Function pointers and
    deep chains are visited once as opaque slots. Use field->mods to distinguish shapes.
==============================================================================================*/

typedef void ( *rs_ref_visitor_t )( void** ref_slot, uint16_t pointee_type_id, const rs_field_t*, void* user );
typedef void ( *rs_visitor_t     )( void*  addr,     uint16_t type_id,         const rs_field_t*, void* user );

void                rs_walk_refs            ( void* instance, uint16_t type_id, rs_ref_visitor_t visit, void* user );
void                rs_walk                 ( void* instance, uint16_t type_id, rs_visitor_t     visit, void* user );

/*==============================================================================================
    Serialization

    Format: [ 20-byte header ][ raw sizeof(T) body ]
    Pointer slots and @transient fields are zeroed in the saved body.
    Compatibility is gated on schema_hash — any layout change produces a mismatch.
    Pointers read back as NULL; persist references via stable IDs, not raw pointers.
==============================================================================================*/

#define RS_SAVE_MAGIC        0x31307372u  /* 'rs01' little-endian */
#define RS_SAVE_HEADER_SIZE  20

typedef enum rs_io_status_e
{
    RS_IO_OK        = 0,
    RS_IO_INCOMPAT  = 1,  /* magic / type_hash / schema_hash / body_size mismatch */
    RS_IO_TRUNCATED = 2,  /* buffer too small                                      */
    RS_IO_NO_TYPE   = 3,  /* expected_type_id not registered                       */
    RS_IO_BAD_ARG   = 4,  /* NULL pointer argument                                 */

} rs_io_status_t;

size_t              rs_write                ( const void* instance, uint16_t type_id, uint8_t* buf, size_t cap );
rs_io_status_t      rs_read                 ( void* instance, uint16_t expected_type_id, const uint8_t* buf, size_t cap, size_t* bytes_read );
uint32_t            rs_peek_type_hash       ( const uint8_t* buf, size_t cap );

/*==============================================================================================
    Diagnostics
==============================================================================================*/

void                rs_print_types          ( void );
void                rs_print_type           ( uint16_t type_id );
void                rs_print_frame          ( uint16_t frame_id );
size_t              rs_field_describe       ( const rs_field_t* f, char* buf, size_t buf_size );

/*==============================================================================================
    Tests
==============================================================================================*/

void  rs_run_tests( void );

/*==============================================================================================
    rs_api_t — module accessor (included last; all types above are already declared)
==============================================================================================*/

#include "rs_api.h"

// clang-format on
/*============================================================================================*/
#endif
