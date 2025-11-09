#ifndef REFLECTION_H
#define REFLECTION_H
/*==============================================================================================

    reflection.h

    -- Reflection system for runtime type information (RTTI)
    -- Allows registration and lookup of types and their fields.
    -- Designed for use in tools, serialization, and editors.
    -- Fixed-size arrays for simplicity, can be extended to dynamic if needed.
    -- Safe for hot-reload: copied data and interned strings live in engine memory.

    -- built-in types integrated as real entries in the same table
    -- single-array open-addressing hash table for named type lookups
    -- interned global string pool (one copy of each name) for names.
    -- internal types use indexes and no pointers for hot-reloiad.
    -- registration API copies generated/static data into the registry and interns strings.
    -- reflect_type_id() helper and example of caching the returned id for fast runtime access


==============================================================================================*/

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "orb.h"
#include "str_intern.h"

// clang-format off
/*==============================================================================================

    Reflection : Constants

==============================================================================================*/

#define MAX_TYPES           512                 // Fixed limit - but 1024 types
#define MAX_FIELDS          4096                // Fixed limit - but 2048 fields
#define MAX_MODULES         16                  // Maximum reflected modules.

#define TYPE_HASH_SIZE      1048                // must be power of two
#define TYPE_INVALID        ((uint16_t)0xFFFF)  // invalid type ID

/*==============================================================================================

    Reflection : Types

==============================================================================================*/

/* Built-in primitive types. 0 is invalid sentinel. */

enum
{
    RF_TYPE_INVALID = 0,
    RF_TYPE_BOOL,
    RF_TYPE_INT,
    RF_TYPE_FLOAT,
    RF_TYPE_DOUBLE,
    RF_TYPE_BUILT_IN,       // count
};

// TODO: add uint16_t meta;     // todo: serializable, editable, etc.
/*============================================================================================*/

// note: the first type index is reserved as invalid (but resolves to a non-crashing index).

typedef struct field_t
{
    sid_t       name_sid;       // interned name offset (set at runtime)

    uint16_t    offset;         // byte offset within parent struct (offsetof)
    uint16_t    size;           // element size in bytes

    uint16_t    kind;           // type flags (primitive/struct/array/pointer)
    uint16_t    type_id;        // resolved type id; TYPE_INVALID = unresolved/none

    uint32_t    type_hash;      // hash of type name.

 } field_t;

typedef struct type_s
{
    sid_t       name_sid;       // interned type name (set at runtime
    uint32_t    hash;           // cached name hash (from generator or sid_hash)

    uint16_t    size;           // sizeof()
    uint16_t    field_index;    // field: first index in global field array
    uint16_t    field_count;    // field: number of fields after first index
    uint16_t    next;           // next hash index (TYPE_INVALID = end of chain)

    uint8_t     module_id;      // module: which DLL id created this type.
    uint8_t     valid;          // 1 = valid, 0 = invalid (after unload)
    uint8_t     reserved_a;     // reserved for alignment/padding
    uint8_t     reserved_b;     // reserved for alignment/padding

} type_t;

typedef struct registry_s
{
    uint16_t    type_count;                     // how many registered
    uint16_t    field_count;                    // how many registered fields

    type_t      type_array  [ MAX_TYPES ];      // all registered types    
    field_t     field_array [ MAX_FIELDS ];     // all registered fields
    uint16_t    type_hash   [ TYPE_HASH_SIZE ]; // index hash into type array (next chained)

} registry_t;

// clang-format on

/*============================================================================================*/

// Make it easy to know which hash function to use for type names.
inline uint32_t
reflect_hash_str( const char* str )
{
    return sid_hash( str );
}

// Initialize registry and string pool
void registry_init( void );

// Shutdown registry and string pool
void registry_exit( void );

// Add new type (returns id)
uint16_t      reflect_register_type( const type_t* src );
const type_t* reflect_get_type( uint32_t id );
uint16_t      reflect_get_type_id_hash( uint32_t hash );
uint16_t      reflect_get_type_id( const char* name );
const type_t* registry_find_type( const char* type_name );

uint16_t      reflect_register_type( const type_t* new_type );
uint16_t      reflect_register_fields( const field_t* src, uint16_t count );
void          reflect_resolve_field_types( void );

field_t*      reflect_get_field_by_id( uint32_t type_id );

/*============================================================================================*/
#endif    // REFLECTION_H