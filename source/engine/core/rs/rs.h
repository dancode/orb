#ifndef RS_H
#define RS_H
/*==============================================================================================

    core/rs/rs.h : Reflection System (rs_) - public API.

    A leaner reflection registry built around five design moves:

      1. STACK-FRAME REGISTRY.  Each module pushes a frame; unload pops it. Tables truncate
         back to the frame's starting indices. No 'valid' flags, no version counters, no
         orphan blocks, no validation passes. Hot-reload = pop, recompile, push.

      2. PACKED MODIFIER CHAIN.  A field's pointer/array/const decoration is encoded in a
         single 16-bit packed stream of up to 4 ops, preserving declarator order. This is
         the fix for the order-loss bug in the old flags-based design.

      3. LAZY RESOLUTION.  Fields reference their base type by hash during registration;
         the resolver runs at rs_finalize_frame() time. Steady-state field carries only
         the resolved type_id (the hash array is registration scaffolding).

      4. SCHEMA HASH PER TYPE.  Computed from field layout at register time. Replaces the
         hand-bumped version counter and unlocks safe save-game compatibility checks.

      5. REPEATED FLAT ATTRIBUTES.  Multi-value attributes (e.g. range(0,100)) are stored
         as runs of same-named entries in the contiguous attribute block. No payload pool.

==============================================================================================*/
// clang-format off

#include "orb.h"
#include "sid/sid.h"

/*==============================================================================================
    Constants
==============================================================================================*/

#define RS_TYPE_INVALID         ((uint16_t)0xFFFF)
#define RS_FIELD_INVALID        ((uint16_t)0xFFFF)
#define RS_ATTR_INVALID         ((uint16_t)0xFFFF)
#define RS_FRAME_INVALID        ((uint8_t)0xFF)

#define RS_MAX_FRAMES           32
#define RS_MAX_TYPES            1024
#define RS_MAX_FIELDS           8192
#define RS_MAX_ATTRS            2048
#define RS_MAX_ENUMS            2048

#define RS_TYPE_HASH_SIZE       2048    // must be power of two
#define RS_TYPE_HASH_MASK       (RS_TYPE_HASH_SIZE - 1)

/*==============================================================================================
    Primitive type IDs - reserved slots 0..14 in frame 0
==============================================================================================*/

typedef enum rs_prim_e
{
    RS_PRIM_INVALID = 0,
    RS_PRIM_VOID,
    RS_PRIM_BOOL,
    RS_PRIM_CHAR,
    RS_PRIM_INT8,
    RS_PRIM_UINT8,
    RS_PRIM_INT16,
    RS_PRIM_UINT16,
    RS_PRIM_INT32,
    RS_PRIM_UINT32,
    RS_PRIM_INT64,
    RS_PRIM_UINT64,
    RS_PRIM_FLOAT,
    RS_PRIM_DOUBLE,
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
    RS_KIND_UNION    = 3,
    RS_KIND_FUNCTION = 4,   /* signature type; field[0]=return, field[1..]=params */
} rs_kind_t;

/*==============================================================================================
    Packed modifier chain (16 bits, 4 slots of 4 bits each)

      Each slot:
        bits 0..1 : op (rs_mod_op_t)
        bit  2    : const-qualifies-this-wrapper (the wrapper itself is const, e.g. `T* const`)
        bit  3    : reserved

      Slot 0 is the INNERMOST wrapper around the base type; read low-to-high to walk outward.

      Examples (base type T):
        T                  RS_MODS(0, 0, 0, 0)                              = 0x0000
        T*                 RS_MODS(RS_M_PTR, 0, 0, 0)                       = 0x0001
        T**                RS_MODS(RS_M_PTR, RS_M_PTR, 0, 0)                = 0x0011
        T* const           RS_MODS(RS_M_CONST_PTR, 0, 0, 0)                 = 0x0005
        T*[N]              RS_MODS(RS_M_PTR, RS_M_ARRAY, 0, 0)              = 0x0021,  aux=N
        T(*)[N]            RS_MODS(RS_M_ARRAY, RS_M_PTR, 0, 0)              = 0x0012,  aux=N
        const T*           base_const=1, RS_MODS(RS_M_PTR, 0, 0, 0)         = 0x0001
        const T* const*    base_const=1, RS_MODS(RS_M_PTR, RS_M_CONST_PTR, 0, 0)

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
==============================================================================================*/

typedef enum rs_attr_type_e
{
    RS_ATTR_NONE    = 0,
    RS_ATTR_INT     = 1,
    RS_ATTR_FLOAT   = 2,
    RS_ATTR_BOOL    = 3,
    RS_ATTR_STRING  = 4,
} rs_attr_type_t;

typedef struct rs_attrib_s
{
    sid_t       name_sid;           // attribute name
    uint8_t     type;               // rs_attr_type_t
    uint8_t     _pad[ 3 ];
    union
    {
        int32_t  i32;
        float    f32;
        sid_t    str;
        uint32_t u32;
    } value;
} rs_attrib_t;                      // 12 bytes (4 + 1 + 3 + 4)

/*==============================================================================================
    Enumerator - one entry per "name = value" pair inside an enum type.
    Stored in a dedicated table; an enum-kind rs_type_t reuses field_index/field_count
    to slice into that table (no fields can coexist on an enum type, so the slot is free).
    Underlying integer size is carried by rs_type_t.size (e.g. sizeof(int) for plain enums).
==============================================================================================*/

typedef struct rs_enumerator_s
{
    sid_t       name_sid;           // 4   enumerator name
    int64_t     value;              // 8   signed; covers unsigned values up to INT64_MAX
} rs_enumerator_t;                  // = 12 bytes

/*==============================================================================================
    Field record - 20 bytes
==============================================================================================*/

typedef struct rs_field_s
{
    sid_t       name_sid;           // 4   interned field name
    uint16_t    type_id;            // 2   resolved base type (RS_TYPE_INVALID until finalize)
    uint16_t    offset;             // 2   byte offset within parent struct
    uint16_t    size;               // 2   sizeof(field), including any inline array
    uint16_t    mods;               // 2   packed modifier chain (see above)
    uint16_t    aux;                // 2   array element count, or function signature id
    uint8_t     base_const;         // 1   const on the base type itself (`const T x`)
    uint8_t     kind;               // 1   cached rs_kind_t of base, for fast dispatch
    uint16_t    attr_index;         // 2   first attribute index (RS_ATTR_INVALID if none)
    uint16_t    attr_count;         // 2   number of attributes
} rs_field_t;                       // = 20 bytes

/*==============================================================================================
    Type record - 32 bytes
==============================================================================================*/

typedef struct rs_type_s
{
    sid_t       name_sid;           // 4   interned type name
    uint32_t    hash;               // 4   sid_hash(name) - persistent identity
    uint32_t    schema_hash;        // 4   content hash over fields (for save-game compat)

    uint16_t    field_index;        // 2   first member: index into fields[] (struct/union)
                                    //                       or enums[]  (enum)
    uint16_t    field_count;        // 2   number of members
    uint16_t    attr_index;         // 2   first attribute (RS_ATTR_INVALID if none)
    uint16_t    attr_count;         // 2
    uint16_t    next;               // 2   next index in hash chain (RS_TYPE_INVALID = end)
    uint16_t    size;               // 2   sizeof(T)

    uint8_t     align;              // 1   alignof(T)
    uint8_t     kind;               // 1   rs_kind_t
    uint8_t     frame_id;           // 1   owning frame
    uint8_t     _pad;               // 1
} rs_type_t;                        // = 32 bytes

/*==============================================================================================
    Frame record - one per loaded module
==============================================================================================*/

typedef struct rs_frame_s
{
    sid_t       name_sid;           // 4
    uint32_t    version;            // 4   caller-supplied
    void*       dll_handle;         // 8   platform handle (NULL for system frame)

    uint16_t    first_type;         // 2   table truncation marks
    uint16_t    first_field;        // 2
    uint16_t    first_attr;         // 2
    uint16_t    first_enum;         // 2   reserved for enum table
} rs_frame_t;                       // = 24 bytes

/*==============================================================================================
    Authoring helpers (used by codegen)
==============================================================================================*/

#define RS_OFFSETOF( T, m )         ((uint16_t)offsetof( T, m ))
#define RS_SIZEOF( T )              ((uint16_t)sizeof( T ))
#define RS_ALIGNOF( T )             ((uint8_t)_Alignof( T ))
#define RS_FIELD_SIZE( T, m )       ((uint16_t)sizeof( ((T*)0)->m ))
#define RS_ARRAY_COUNT( a )         ((uint16_t)(sizeof(a) / sizeof((a)[0])))

/*==============================================================================================
    Lifecycle
==============================================================================================*/

void        rs_init                 ( void );
void        rs_exit                 ( void );

/*==============================================================================================
    Frames
==============================================================================================*/

uint8_t     rs_push_frame           ( const char* name, uint32_t version );
void        rs_pop_frame            ( uint8_t frame_id );
bool        rs_finalize_frame       ( uint8_t frame_id );   // resolve fields, return false on error

const rs_frame_t* rs_get_frame      ( uint8_t frame_id );

/*==============================================================================================
    Registration

    `fields` and `field_type_hashes` are PARALLEL arrays of the same length.
    `field_type_hashes[i]` is the sid_hash() of the base type name for fields[i].
    The hash array is consumed during registration; it does not need to outlive the call.
==============================================================================================*/

uint16_t    rs_register_type        ( const rs_type_t* type,
                                      const rs_field_t* fields,
                                      const uint32_t* field_type_hashes,
                                      uint16_t field_count );

bool        rs_type_add_attr        ( uint16_t type_id,  const rs_attrib_t* attr );
bool        rs_field_add_attr       ( uint16_t field_id, const rs_attrib_t* attr );

                                    /* Register an enum type. `type->kind` is forced to RS_KIND_ENUM. */
uint16_t    rs_register_enum        ( const rs_type_t* type,
                                      const rs_enumerator_t* enumerators,
                                      uint16_t count );

                                    /* Register a function signature. `type->kind` is forced to
                                       RS_KIND_FUNCTION. `fields[0]` is the return type; `fields[1..]`
                                       are parameters in declaration order. `type_hashes` is parallel.
                                       A function-pointer FIELD references this signature by storing
                                       its type_id in the field's `aux` slot, with RS_MOD_FUNCTION
                                       in the modifier chain. */
uint16_t    rs_register_function    ( const rs_type_t* type,
                                      const rs_field_t* return_then_params,
                                      const uint32_t* type_hashes,
                                      uint16_t count );

/*==============================================================================================
    Lookup
==============================================================================================*/

const rs_type_t*    rs_get_type         ( uint16_t type_id );
uint16_t            rs_find_type        ( uint32_t name_hash );
uint16_t            rs_find_type_by_name( const char* name );

const rs_field_t*   rs_get_field        ( uint16_t field_id );
const rs_field_t*   rs_find_field       ( uint16_t type_id, const char* name );

const rs_attrib_t*  rs_type_get_attr    ( uint16_t type_id,  const char* name );
const rs_attrib_t*  rs_field_get_attr   ( uint16_t field_id, const char* name );

const rs_enumerator_t* rs_enum_find_by_name ( uint16_t type_id, const char* name );
const rs_enumerator_t* rs_enum_find_by_value( uint16_t type_id, int64_t value );
const rs_enumerator_t* rs_get_enumerator    ( uint16_t enum_id );

                                            /* Function-signature convenience accessors. */
const rs_field_t*      rs_function_get_return( uint16_t type_id );
uint16_t               rs_function_param_count( uint16_t type_id );
const rs_field_t*      rs_function_get_param ( uint16_t type_id, uint16_t param_index );

void                rs_get_stats        ( uint16_t* type_count, uint16_t* field_count, uint8_t* frame_count );

/*==============================================================================================
    Iteration
==============================================================================================*/

typedef void ( *rs_type_cb_t  )( uint16_t type_id,  const rs_type_t*       t, void* user );
typedef void ( *rs_field_cb_t )( uint16_t field_id, const rs_field_t*      f, void* user );
typedef void ( *rs_enum_cb_t  )( uint16_t enum_id,  const rs_enumerator_t* e, void* user );

uint16_t    rs_each_type            ( rs_type_cb_t cb, void* user );
uint16_t    rs_each_type_in_frame   ( uint8_t frame_id, rs_type_cb_t cb, void* user );
uint16_t    rs_each_field           ( uint16_t type_id, rs_field_cb_t cb, void* user );
uint16_t    rs_each_enumerator      ( uint16_t type_id, rs_enum_cb_t  cb, void* user );

/*==============================================================================================
    Diagnostics
==============================================================================================*/

void        rs_print_types          ( void );
void        rs_print_type           ( uint16_t type_id );
void        rs_print_frame          ( uint8_t frame_id );

                                    // Pretty-print a field's full type (e.g. "const vec3_t*[8]")
                                    // into `buf`. Returns characters written (excl. terminator).
size_t      rs_field_describe       ( const rs_field_t* f, char* buf, size_t buf_size );

/*==============================================================================================
    Test entry
==============================================================================================*/

void        rs_run_tests            ( void );

/*============================================================================================*/
#endif    // RS_H
