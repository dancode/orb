/*==============================================================================================

    engine/rs/rs.h - Reflection types, enums, structs, constants, and callback typedefs.

    Pure types only -- no vtable, no function declarations.
    Include engine/rs/rs_import.h for the registration vtable (generated code).
    Include engine/rs/rs_api.h for the runtime vtable (DLL modules).
    Include engine/rs/rs_host.h for direct function calls (host/tests).

==============================================================================================*/
#ifndef RS_H
#define RS_H

#include "orb.h"

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
#define RS_TYPE_HASH_SIZE       1024                            /* must be power of two */
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

    T              [0000|0000|0000|0000]   all NONE
    T*             [0000|0000|0000|0001]   s0=PTR
    T**            [0000|0000|0001|0001]   s0=PTR, s1=PTR
    T* const       [0000|0000|0000|0101]   s0=PTR(const_bit set)
    T[N]           [0000|0000|0000|0010]   s0=ARRAY
    T*[N]          [0000|0000|0010|0001]   s0=PTR, s1=ARRAY
    T(*)[N]        [0000|0000|0001|0010]   s0=ARRAY, s1=PTR
    const T*       [0000|0000|0000|0001]   s0=PTR, base_const separate

    T              — value of type T
    T*             — pointer to T
    T**            — pointer to a pointer to T
    T* const       — constant pointer to T (the pointer cannot be reassigned)
    T[N]           — inline array of N T values
    T*[N]          — inline array of N pointers to T
    T(*)[N]        — pointer TO AN array of N T values
    const T*       — pointer to a constant T (the pointed-at value cannot be modified)

==============================================================================================*/

typedef enum rs_mod_op_e
{
    RS_MOD_NONE     = 0,
    RS_MOD_PTR      = 1,
    RS_MOD_ARRAY    = 2,
    RS_MOD_FUNCTION = 3,

} rs_mod_op_t;

/* Slot accessors -- used to extract information from a packed modifier chain */
/* Example: to check if the outermost modifier is a const pointer:

    uint16_t mods = field->mods;
    rs_mod_op_t op = RS_MOD_OP( RS_MOD_GET( mods, 0 ) );
    bool is_const = RS_MOD_IS_CONST( mods, 0 );
    if ( op == RS_MOD_PTR && is_const )
        printf( "field is a const pointer\n" );
*/

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
    Serialization Status

    rs_io_status_t is part of the rs_api_t vtable surface; kept here so DLL modules can
    inspect the return value of rs()->read() without including rs_host.h.
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

/*==============================================================================================
    Callback Types

    Typedefs shared between the rs_api_t vtable and host-side direct-call functions.
    Declared here so DLL modules can define conforming callbacks without including rs_host.h.
==============================================================================================*/

typedef void ( *rs_type_cb_t      ) ( uint16_t type_id,  const rs_type_t*  t, void* user );
typedef void ( *rs_field_cb_t     ) ( uint16_t field_id, const rs_field_t* f, void* user );
typedef void ( *rs_enum_cb_t      ) ( uint16_t enum_id,  const rs_enum_t*  e, void* user );
typedef void ( *rs_ref_visitor_t  ) ( void** ref_slot, uint16_t pointee_type_id, const rs_field_t*, void* user );
typedef void ( *rs_visitor_t      ) ( void*  addr,     uint16_t type_id,         const rs_field_t*, void* user );

// clang-format on
/*============================================================================================*/
#endif    // RS_H
