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

    uint16_t mods: a flat 16-bit enum encoding the complete declarator shape of a field.
    The full set of supported C-field shapes is enumerated below; invalid combinations
    are unrepresentable. Compare f->mods directly against these named values.

    Bit layout:
        bits [2:0]  primary declarator   (0x1=ptr, 0x2=array, 0x4=fn)
        bit  [3]    internal const       (pointer itself is const: T* const)
        bit  [4]    external const       (base type is const:      const T)
        bits [10:8] secondary declarator (outer wrapper, same encoding as bits [2:0])

==============================================================================================*/

typedef enum rs_mods_e
{
    /* value */
    RS_MODS_VALUE        = 0x0000,   // T           value of type T  
    RS_MODS_CONST_VALUE  = 0x0010,   // const T     const-qualified value

    /* pointer */
    RS_MODS_PTR          = 0x0001,   // T*          pointer to T
    RS_MODS_PTR_TO_CONST = 0x0011,   // const T*    pointer to const T
    RS_MODS_CONST_PTR    = 0x0009,   // T* const    constant pointer to T
    RS_MODS_PTR_PTR      = 0x0101,   // T**         pointer to a pointer to T

    /* array */
    RS_MODS_ARRAY        = 0x0002,   // T[N]        array of values     ( aux = count )
    RS_MODS_PTR_ARRAY    = 0x0201,   // T*[N]       array of pointers   ( aux = count )
    RS_MODS_ARRAY_PTR    = 0x0102,   // T(*)[N]     pointer to an array ( aux = count ) 

    /* function */
    RS_MODS_FUNCTION     = 0x0004,   // T(*)()      function pointer    ( aux = sig type_id )

} rs_mods_t;

/*==============================================================================================
    Attribute

    Single 4-byte payload entry. Multi-value metadata (@range(0, 100)) uses repeated
    entries with the same name_id. Larger types are split across multiple entries.
    String values are interned into the rs_ string pool.

    flags (uint16_t): engine bits 0-7, user-defined bits 8-15.

        Use rs_attr_flag_t for engine bits. 
        Check with (attr->flags & RS_AF_*).

    ci  packed group size and position in a byte.

        High nibble = count (total entries in logical group, 1 = single value).
        Low  nibble = index (this entry's 0-based position within the group).

        Build with:  RS_ATTR_CI( count, index ). 
        Read withL   RS_ATTR_COUNT( ci ), RS_ATTR_INDEX( ci ).

==============================================================================================*/

typedef enum rs_attr_type_e
{
    RS_ATTR_NONE   = 0,
    RS_ATTR_INT    = 1,
    RS_ATTR_FLOAT  = 2,
    RS_ATTR_BOOL   = 3,
    RS_ATTR_STRING = 4,

} rs_attr_type_t;

/* Built-in attribute semantic flags -- bits 0-7 engine, bits 8-15 user-defined.
   Each flag identifies the semantic role of an attrib entry without a strcmp.
   Pair with RS_ANAME_* constants for the canonical interned name strings.*/

typedef enum rs_attr_flag_e
{
    RS_AF_RANGE        = ( 1 << 0 ),   // min/max hard clamp on value and editor edit
    RS_AF_CLAMP_UI     = ( 1 << 1 ),   // min/max soft UI limiter; user can type past
    RS_AF_DISPLAY_NAME = ( 1 << 2 ),   // string override for editor display name    
    RS_AF_TOOLTIP      = ( 1 << 3 ),   // tooltip / helper string shown in editor    

    /* bits 4-7: engine reserved */
    /* bits 8-15: user-defined */

} rs_attr_flag_t;

#define RS_AF_CLAMP             RS_AF_RANGE   /* range and clamp are the same concept */

/* Canonical name strings for built-in attributes -- include in name_id interns */

#define RS_ANAME_RANGE          "range"
#define RS_ANAME_CLAMP          "range"             /* alias */
#define RS_ANAME_CLAMP_UI       "clamp_ui"
#define RS_ANAME_DISPLAY_NAME   "display_name"
#define RS_ANAME_TOOLTIP        "tooltip"

/* Pack count (1-15) and index (0-14) into one byte: high nibble = count, low = index */

#define RS_ATTR_CI( count, index )  ( (uint8_t)( ( (uint8_t)(count) & 0x0F ) << 4 | ( (uint8_t)(index) & 0x0F ) ) )
#define RS_ATTR_COUNT( ci )         ( (uint8_t)( (uint8_t)(ci) >> 4 ) )
#define RS_ATTR_INDEX( ci )         ( (uint8_t)( (uint8_t)(ci) & 0x0F ) )
#define RS_ATTR_CI_SINGLE           RS_ATTR_CI( 1, 0 )   /* shorthand for a lone entry */

typedef struct rs_attrib_s
{
    rs_name_t  name_id;         // interned attribute name
    uint16_t   flags;           // rs_attr_flag_t bitmask; bits 8-15 user-defined
    uint8_t    type;            // rs_attr_type_t
    uint8_t    ci;              // packed: high nibble = count, low nibble = index
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
    rs_name_t  name_id;             // interned enumerator name
    int32_t    value;               // signed; covers unsigned values up to INT32_MAX

} rs_enum_t;

/*==============================================================================================
    Field Flags

    Common field-level semantics stored directly on rs_field_t for O(1) access.
    Bits 0-15 are engine-defined. Bits 16-31 are reserved for project/game use.

==============================================================================================*/

typedef enum rs_field_flag_e
{
    RS_FF_TRANSIENT  = ( 1 << 0 ),  // serialize: exclude from serialization
    RS_FF_EDITOR     = ( 1 << 1 ),  // editor: show in editor inspector
    RS_FF_READONLY   = ( 1 << 2 ),  // editor: display in editor, no edit
    RS_FF_HIDDEN     = ( 1 << 3 ),  // editor: hide from editor
    RS_FF_NETWORK    = ( 1 << 4 ),  // network: replicate over network
    RS_FF_DEPRECATED = ( 1 << 5 ),  // developer: warn on use
    RS_FF_REQUIRED   = ( 1 << 6 ),  // developer: validation: field must be set

    /* bits 16-31: user defined */

} rs_field_flag_t;

/*==============================================================================================
    Field Record  (28 bytes)
==============================================================================================*/

typedef struct rs_field_s
{
    rs_name_t  name_id;             // gen: interned field name
    uint32_t   type_hash;           // gen: rs_hash_str(base_type_name) — used during lazy resolution
    uint16_t   type_id;             // ___: resolved base type (RS_TYPE_INVALID until finalize)
    uint16_t   offset;              // gen: byte offset within parent struct
    uint16_t   size;                // gen: sizeof(field), including any inline array
    uint16_t   mods;                // gen: packed modifier chain
    uint16_t   aux;                 // gen: array element count, or function signature type_id

    uint8_t    kind;                // ___: gcached rs_kind_t of base, for fast dispatch
    uint8_t    _pad;                // ___: reserved for future use

    uint16_t   attr_index;          // ___: first attribute index (RS_ATTR_INVALID if none)
    uint16_t   attr_count;          // ___: number of attributes

    uint32_t   flags;               // gen: rs_field_flag_t bitmask (0 = no flags set)

} rs_field_t;

/*==============================================================================================
    Type Flags

    Common type-level semantics stored directly on rs_type_t for O(1) access.
    Bits 0-3 are engine-defined. Bits 4-7 are reserved for project/game use.
==============================================================================================*/

typedef enum rs_type_flag_e
{
    RS_TF_ABSTRACT   = ( 1 << 0 ),  // not directly instantiable (e.g. interface or base struct)
    RS_TF_SERIALIZE  = ( 1 << 1 ),  // default-serialize all fields
    RS_TF_EDITOR     = ( 1 << 2 ),  // show in editor type browser
    RS_TF_DEPRECATED = ( 1 << 3 ),  // warn on use

    /* bits 4-7: project/game-defined */

} rs_type_flag_t;

/*==============================================================================================
    Type Record  (28 bytes)
==============================================================================================*/

typedef struct rs_type_s
{
    rs_name_t  name_id;             // gen: interned type name
    uint32_t   hash;                // gen: rs_hash_str(name) — persistent identity
    uint32_t   schema_hash;         // ___: content hash of field layout (save-game / hot-reload compat)

    uint16_t   field_index;         // ___: first entry in fields[] (struct/union) or enums[] (enum)
    uint16_t   field_count;         // ___: number of fields or enumerators in this type

    uint16_t   attr_index;          // ___: first attribute (RS_ATTR_INVALID if none)
    uint16_t   attr_count;          // ___: number of attributes on the type itself (not counting fields)

    uint16_t   next;                // ___: next type in hash chain (RS_TYPE_INVALID = end)
    uint16_t   size;                // gen: sizeof(T)

    uint8_t    align;               // gen: alignof(T)
    uint8_t    kind;                // ___: rs_kind_t (prim, struct, enum, etc.)
    uint8_t    frame_id;            // ___: owning frame (module) — used for cleanup on unload
    uint8_t    flags;               // gen: rs_type_flag_t bitmask (0 = no flags set)

} rs_type_t;

/*==============================================================================================
    Frame Record

    One per loaded module. Pop truncates all four tables back to the first_* marks.
==============================================================================================*/

typedef struct rs_frame_s
{
    rs_name_t  name_id;             // interned module name; matches the module name passed to rs_push_frame
    uint16_t   first_type;          // start of modules types - pop rewinds to these points
    uint16_t   first_field;         // start of modules fields
    uint16_t   first_attr;          // start of modules attributes
    uint16_t   first_enum;          // start of modules enums

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
