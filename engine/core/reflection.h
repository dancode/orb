/*==============================================================================================

    reflection.h

==============================================================================================*/

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "orb.h"

// include "base/base.h"

/*==============================================================================================

    uses fixed size array but dynamically populated

==============================================================================================*/

#define MAX_TYPES   1024    // Fixed limit - but 1024 types is huge
#define MAX_MODULES 16

// Field descriptor - minimal but enough for editors

// typedef uint16_t type_id;    // Index into type array
// typedef uint32_t hash_id;    // Simple hash for lookup

typedef struct field_t
{
    const char* name;       // Points to static string in DLL
    int16_t     offset;     // Byte offset in struct
    int16_t     size;       // Size in bytes
    int16_t     type_id;    // Type of this field
    int16_t     flags;      // Serializable, editable, etc.

} field_t;

// Type descriptor - everything needed for tooling

typedef struct type_t
{
    // Identity
    const char* name;    // Human readable name
    hash_id     hash;    // Hash of name (for lookup)
    type_id     id;      // Index in array (stable across reloads)

    // Layout
    uint16_t size;         // sizeof(Type)
    uint16_t alignment;    // alignof(Type)

    // Module ownership
    uint8_t module_id;    // Which DLL owns this type
    uint8_t version;      // Type version for hot reload

    // Fields
    field_t* fields;
    uint8_t  field_count;

} type_t;

// The global type registry.

typedef struct registry_t
{
    type_t   type_array[ MAX_TYPES ];    // All types
    uint16_t type_count;                 // How many registered

    // Fast lookup table (single global registry)
    struct
    {
        hash_id hash;
        type_id id;

    } hash_map[ MAX_TYPES * 2 ];    // 2x size for good distribution

    // Module tracking for hot reload
    struct
    {
        void*       handle;        // DLL handle
        const char* path;          // DLL path
        type_id     type_start;    // First type ID for this module
        uint16_t    type_count;    // Number of types in module
    } modules[ MAX_MODULES ];

    uint8_t module_count;

} registry_t;

// Global registry - single instance in main.exe
extern registry_t g_registry;

/*============================================================================================*/
