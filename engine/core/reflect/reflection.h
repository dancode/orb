#ifndef REFLECTION_H
#define REFLECTION_H

/*==============================================================================================

    reflection.h

==============================================================================================*/

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "orb.h"
#include "str_intern.h"

/*==============================================================================================

    A global string interner stores one copy of each name:

==============================================================================================*/


/*==============================================================================================

    uses fixed size array but dynamically populated

==============================================================================================*/

typedef enum rf_prim_e
{
    RF_PRIM_BOOL  = BIT( 0 ),    // Primitive boolean type
    RF_PRIM_INT   = BIT( 1 ),    // Primitive integer type
    RF_PRIM_FLOAT = BIT( 2 ),    // Primitive float type
} rf_prim_t;

#define MAX_TYPES   1024    // Fixed limit - but 1024 types
#define MAX_FIELDS  2048    // Fixed limit - but 2048 fields
#define MAX_MODULES 16

// Field descriptor - minimal but enough for editors

typedef uint16_t type_id;    // Index into type array
typedef uint32_t hash_id;    // Simple hash for lookup

typedef struct field_t
{
    uint32_t name_sid;         // Interned string ID for name
    uint16_t offset;           // Byte offset in struct
    uint16_t size;             // Size in bytes
    uint16_t kind;             // Basic/Struct/Array
    uint16_t sub_type_id;      // resolved type index (0 = none)
    uint32_t sub_type_hash;    // unresolved type hash

    // int16_t  flag;    // Serializable, editable, etc.

} field_t;

// Type descriptor - everything needed for tooling

typedef struct type_s
{
    uint32_t name_sid;    // id: interned name string ID

    uint16_t size;    // layout: sizeof()
                      // uint16_t alignment;    // layout: alignof()

    uint16_t field_index;    // field: index into array of fields
    uint16_t field_count;    // field: Number of fields

    uint8_t  module_id;    // module: Which DLL owns this type
    uint8_t  valid;        // module: Type version for hot reload

    uint8_t  padding[ 2 ];

} type_t;

// The global type registry.

typedef struct registry_s
{
    type_t   type_array[ MAX_TYPES ];
    field_t  field_array[ MAX_FIELDS ];
    uint16_t type_count;
    uint16_t field_count;

} registry_t;

/*============================================================================================*/

// extern registry_t g_registry;

/*============================================================================================*/

// Initialize registry and string pool
void registry_init( void );

// Register types and fields from static data
uint32_t registry_register_types( uint8_t         module_id,
                                  const char**    type_names,
                                  const uint16_t* type_sizes,
                                  const uint16_t* field_counts,
                                  const field_t*  fields,
                                  uint16_t        type_count );

// Resolve cross-type dependencies (sub_type_hash -> sub_type_id)
void          registry_resolve_dependencies( void );
int           registry_find_type_by_hash( uint32_t hash );    // Lookup
const type_t* registry_find_type( const char* name );

/*============================================================================================*/

// typedef struct registry_s
// {
//     type_t   type_array[ MAX_TYPES ];    // All types
//     uint16_t type_count;                 // How many registered
//
//     // Fast lookup table (single global registry)
//     struct
//     {
//         hash_id hash;
//         type_id id;
//
//     } hash_map[ MAX_TYPES * 2 ];    // 2x size for good distribution
//
//     // Module tracking for hot reload
//     struct
//     {
//         void*       handle;        // DLL handle
//         const char* path;          // DLL path
//         type_id     type_start;    // First type ID for this module
//         uint16_t    type_count;    // Number of types in module
//
//     } modules[ MAX_MODULES ];
//
//     uint8_t module_count;
//
// } registry_t;

/*============================================================================================*/
#endif    // REFLECTION_H