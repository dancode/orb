#ifndef REFLECT_H
#define REFLECT_H
/*==============================================================================================

    core/reflect/reflect.h : Public API for the reflection system.

    -- Reflection system for runtime type information (RTTI)
    -- Allows registration and lookup of types and their fields.
    -- Designed for use in tools, serialization, and editors.
    -- Fixed-size arrays for simplicity, can be extended to dynamic if needed.
    -- Safe for hot-reload: copied data and interned strings live in engine memory.

    -- built-in types integrated as real entries in the same table
    -- single-array open-addressing hash table for named type lookups
    -- interned global string pool (one copy of each name) for names.
    -- internal types use indexes and no pointers for hot-reload.
    -- registration API copies generated/static data into the registry and interns strings.
    -- reflect_type_id() helper and example of caching the returned id for fast runtime access

==============================================================================================*/
// clang-format off

#include "orb.h"
#include "sid/sid.h"

/*==============================================================================================
    Reflection : Constants
==============================================================================================*/

#define MODULE_INVALID          ((uint8_t)0xFF)
#define TYPE_INVALID            ((uint16_t)0xFFFF)      // invalid type ID
#define FIELD_INVALID           ((uint16_t)0xFFFF)
#define ATTR_INVALID            ((uint16_t)0xFFFF)

/*==============================================================================================
    Reflection : Types
==============================================================================================*/

/* Primitive type IDs - reserved slots 0-15 */
typedef enum rf_prim_e
{
    RF_TYPE_INVALID = 0,
    RF_TYPE_VOID,
    RF_TYPE_BOOL,
    RF_TYPE_CHAR,
    RF_TYPE_INT8,
    RF_TYPE_UINT8,
    RF_TYPE_INT16,
    RF_TYPE_UINT16,
    RF_TYPE_INT32,
    RF_TYPE_UINT32,
    RF_TYPE_INT64,
    RF_TYPE_UINT64,
    RF_TYPE_FLOAT,
    RF_TYPE_DOUBLE,
    RF_TYPE_STRING,                 // char* or string type
    RF_TYPE_BUILT_IN_COUNT,         // 15 built-in types
} rf_prim_t;

/* Field kind flags - can be combined for composite types */
typedef enum rf_kind_e
{
    RF_KIND_PRIMITIVE   = (1 << 0),     // Basic type (int, float, etc)
    RF_KIND_STRUCT      = (1 << 1),     // Struct/composite type
    RF_KIND_ENUM        = (1 << 2),     // Enumeration
    RF_KIND_UNION       = (1 << 3),     // Union type
    RF_KIND_POINTER     = (1 << 4),     // Pointer to type
    RF_KIND_ARRAY       = (1 << 5),     // Fixed-size array
    RF_KIND_CONST       = (1 << 6),     // Const qualifier
    RF_KIND_VOLATILE    = (1 << 7),     // Volatile qualifier
    RF_KIND_FUNCTION    = (1 << 8),     // Function pointer
} rf_kind_t;

/* Module state tracking */
typedef enum rf_module_state_e
{
    RF_MODULE_UNLOADED  = 0,
    RF_MODULE_LOADING   = 1,            // types being registered
    RF_MODULE_LOADED    = 2,
    RF_MODULE_UNLOADING = 3,
    RF_MODULE_ERROR     = 4,
} rf_module_state_t;

/*==============================================================================================
    Attribute System - Extensible metadata

    Extensible metadata system for types and fields
    Supports int, float, string, and bool attributes
    Use cases:

    [serializable] - Mark types for serialization
    [range(0, 100)] - Editor hints for numeric fields
    [deprecated] - Mark fields scheduled for removal
    [editor_hidden] - Hide from editor UI
    Custom attributes for your build tool.
==============================================================================================*/

typedef enum rf_attr_type_e
{
    RF_ATTR_NONE        = 0,
    RF_ATTR_INT         = 1,
    RF_ATTR_FLOAT       = 2,
    RF_ATTR_STRING      = 3,
    RF_ATTR_BOOL        = 4,
} rf_attr_type_t;

typedef struct rf_attrib_s
{
    sid_t           name_sid;           // attribute name (we check handle based on name) (e.g. min = 0.0f)
    uint8_t         type;               // rf_attr_type_t
    uint8_t         reserved[3];
    union {
        int32_t     i32;
        float       f32;
        sid_t       str;                // string attribute as sid
        uint32_t    u32;
    } value;
} rf_attrib_t;

/*==============================================================================================
    Reflection : Data Structures
==============================================================================================*/

typedef struct rf_field_s               // A type instance
{
    sid_t       name_sid;               // Field name (interned)
    uint32_t    type_hash;              // Hash of type name.
        
    uint16_t    type_id;                // Resolved type ID (TYPE_INVALID = unresolved)
    uint16_t    offset;                 // Byte offset within parent struct (offsetof)
    uint16_t    size;                   // Size in bytes (sizeof)
    uint16_t    kind;                   // rf_kint_t flags
    
    uint16_t    array_count;            // If RF_KIND_ARRAY, element count (0 = not array)
    uint16_t    pointer_depth;          // Levels of indirection (type** = 2) (0 = not pointer)

    uint16_t    attr_index;             // First attribute index (ATTR_INVALID = none)
    uint16_t    attr_count;             // Number of attributes

} rf_field_t;

typedef struct rf_type_s                // A type definition
{
    sid_t       name_sid;               // Type name (interned)
    uint32_t    hash;                   // Cached hash for fast lookup
        
    uint16_t    field_index;            // First field index
    uint16_t    field_count;            // Number of fields

    uint16_t    attr_index;             // First attribute index
    uint16_t    attr_count;             // Number of attributes

    uint16_t    next;                   // next hash index (TYPE_INVALID = end of chain)
    uint16_t    kind;                   // rf_kint_t flags
    uint16_t    size;                   // sizeof()

    uint8_t     align;                  // alignof()
    uint8_t     module_id;              // Module that defined this type
    uint8_t     valid;                  // 1 = valid, 0 = invalidated
    uint8_t     version;                // Version number for hot-reload tracking.    

} rf_type_t;

/*==============================================================================================
    Reflection : Constants
==============================================================================================*/

#define RF_VERSION          1
#define MAX_MODULES         32
#define MAX_TYPES           1024
#define MAX_FIELDS          8192
#define MAX_ATTRIBUTES      2048

#define TYPE_HASH_SIZE      2048                        // power of two
#define TYPE_HASH_MASK      ( TYPE_HASH_SIZE - 1 )

/*==============================================================================================
    Reflect : Setup
==============================================================================================*/

                                            // Initialize reflection system
void                rf_init                 ( void );

                                            // Shutdown and cleanup 
void                rf_exit                 ( void );

                                            // Accessor for registry stats
void                rf_get_stats            ( uint16_t* type_count, uint16_t* field_count );

/*==============================================================================================
    Reflection : Initialization
==============================================================================================*/

                                            // Add attribute to type
bool                rf_type_add_attribute   ( uint16_t type_id, const rf_attrib_t* attr );

                                            // Add attribute to field
bool                rf_field_add_attribute  ( uint16_t field_id, const rf_attrib_t* attr );

                                            // Convenience function to register type with fields (returns id)
uint16_t            rf_register_type        ( rf_type_t* type, const rf_field_t* fields, uint16_t field_count );

                                            // Resolve all field subtype hashes to type ids
void                rf_resolve_fields       ( void );

                                            // Ensure all field types are resolved - report errors
bool                rf_validate_fields        ( void );

                                            // Ensure all types are valid - report errors
bool                rf_validate_types       ( void );

                                            // Validate entire registry integrity (hash lookups)
bool                rf_validate_registry    ( void );

/*==============================================================================================
    Reflection : Module Management
==============================================================================================*/

                                            // Register a new module
uint8_t             rf_module_register      ( const char* name, uint32_t version );

                                            // Begin module unload (mark types invalid)
void                rf_module_begin_unload  ( uint8_t module_id) ;

                                            // Complete module unload (cleanup)
void                rf_module_end_unload    ( uint8_t module_id );

/*==============================================================================================
    Reflection : Type Access
==============================================================================================*/

const rf_type_t*    rf_get_type             ( uint32_t id );
uint16_t            rf_get_type_id          ( const rf_type_t* type );
uint16_t            rf_get_type_id_by_hash  ( uint32_t hash );
const rf_type_t*    rf_get_type_by_name     ( const char* type_name );
uint16_t            rf_get_type_id_by_name  ( const char* type_name );

/*==============================================================================================
    Reflection : Field Access
==============================================================================================*/

typedef void ( *rf_type_cb_t  )( uint16_t type_id,  const rf_type_t*  t, void* user );
typedef void ( *rf_field_cb_t )( uint16_t field_index, const rf_field_t* f, void* user );

                                            // Iterate all valid types (including builtins)
uint16_t            rf_each_type            ( rf_type_cb_t cb, void* user );

                                            // Iterate all valid types owned by a module
uint16_t            rf_each_type_in_module  ( uint8_t module_id, rf_type_cb_t cb, void* user );

const rf_field_t*   rf_get_field            ( uint32_t field_id );
uint16_t            rf_get_field_id         ( const rf_field_t* field );
const rf_field_t*   rf_get_field_by_sid     ( uint16_t type_id, sid_t name_sid );
const rf_field_t*   rf_get_field_by_name    ( uint16_t type_id, const char* name );
uint16_t            rf_each_field           ( u32 type_id, rf_field_cb_t cb, void* user );

/*==============================================================================================
    Reflection : Attribute Access
==============================================================================================*/

const rf_attrib_t*  rf_type_get_attr        ( uint16_t type_id, const char* name );
const rf_attrib_t*  rf_field_get_attr       ( uint16_t field_id, const char* name );
bool                rf_type_has_attr        ( uint16_t type_id, const char* name );
bool                rf_field_has_attr       ( uint16_t field_id, const char* name );

/*==============================================================================================
    Reflection : Diagnostics
==============================================================================================*/

void                rf_print_types          ( void );
void                rf_print_type           ( uint16_t type_id );
void                rf_print_module         ( uint8_t module_id );

/*==============================================================================================
    Reflect : Helper Macros (for generated code)
==============================================================================================*/

#define RF_OFFSETOF(type, member)       ((uint16_t)offsetof(type, member))
#define RF_SIZEOF(type)                 ((uint16_t)sizeof(type))
#define RF_ALIGNOF(type)                ((uint16_t)_Alignof(type))
#define RF_FIELD_SIZE(type, member)     ((uint16_t)sizeof(((type*)0)->member))
#define RF_ARRAY_COUNT(arr)             ((uint16_t)(sizeof(arr)/sizeof((arr)[0])))

/*============================================================================================*/
#endif    // REFLECT_H