/*============================================================================================*/
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>


#include "orb.h"
#include "reflection.h"
#include "str_intern.h"

/*============================================================================================*/

static registry_t g_registry;

/*============================================================================================*/

void
rf_init( void )
{
    sid_init();
    memset( &g_registry, 0, sizeof( g_registry ) );

    for ( int i = 0; i < TYPE_HASH_SIZE; i++ ) g_registry.type_hash[ i ] = TYPE_INVALID;

    const char* builtin_names[] = {
        "invalid", "bool", "int", "float", "double",
    };

    uint16_t builtin_sizes[] = {
        0, 1, 4, 4, 8,
    };

    for ( uint16_t i = 0; i < RF_TYPE_BUILT_IN; ++i )
    {
        type_t t      = { 0 };
        t.name_sid    = sid_intern_cstr( builtin_names[ i ] );
        t.hash        = sid_hash( builtin_names[ i ] );
        t.size        = builtin_sizes[ i ];
        t.field_index = 0;
        t.field_count = 0;
        t.next        = TYPE_INVALID;
        t.module_id   = 0;
        t.valid       = 1;

        // add to type array
        g_registry.type_array[ i ] = t;

        // add to hash table
        uint32_t slot                   = t.hash & ( TYPE_HASH_SIZE - 1 );
        uint16_t old_head               = g_registry.type_hash[ slot ];
        g_registry.type_array[ i ].next = old_head;
        g_registry.type_hash[ slot ]    = i;
    }

    g_registry.type_count  = RF_TYPE_BUILT_IN;
    g_registry.field_count = 0;
}

/*============================================================================================*/

void
rf_exit( void )
{
    memset( &g_registry, 0, sizeof( g_registry ) );
    sid_shutdown();
}

/*============================================================================================*/
// Register fields (copy from static table)

static uint16_t
rf_add_fields( const field_t* fields, uint16_t count )
{
    if ( count == 0 )
        return 0;    // 0 fields = 0 index. This is fine.

    if ( !fields )
    {
        assert( 0 && "Invalid field registration! count > 0 but fields is NULL." );
        return TYPE_INVALID;
    }
    if ( g_registry.field_count + count > MAX_FIELDS )
    {
        assert( 0 && "Field registry full!" );
        return TYPE_INVALID;
    }

    /* copy memory in one go */
    uint16_t start = g_registry.field_count;
    memcpy( &g_registry.field_array[ start ], fields, ( size_t )count * sizeof( field_t ) );
    g_registry.field_count += count;

    return start;    // first field index
}

/*============================================================================================*/
static uint16_t
rf_find_or_create_type_slot( const type_t* src_type )
{
    // 1. Try to find an existing slot (valid or invalid)

    uint32_t hash = src_type->hash;
    uint16_t id   = rf_get_tid_from_hash( hash );

    if ( id != TYPE_INVALID )
    {
        return id;    // Found existing type -- return the stable ID.
    }

    // 2. Type not found -- Create a new one.

    if ( g_registry.type_count >= MAX_TYPES )
    {
        assert( 0 && "Type registry full!" );
        return TYPE_INVALID;
    }

    id                    = g_registry.type_count++;         // Allocate new id
    type_t* new_type      = &g_registry.type_array[ id ];    // Get pointer to new slot
    *new_type             = *src_type;                       // Copy the type data

    new_type->field_index = 0;               // Field data is set later
    new_type->field_count = 0;               // Field data is set later
    new_type->next        = TYPE_INVALID;    // Initialize next

    // 3. Insert into hash table

    uint32_t slot                = hash & TYPE_HASH_MASK;
    new_type->next               = g_registry.type_hash[ slot ];
    g_registry.type_hash[ slot ] = id;

    return id;
}

/*============================================================================================*/
// Convenience function to register type with fields (returns id)
// Ensures field index and count are set correctly.

uint16_t
rf_register_type( type_t* type_data, const field_t* fields, uint16_t field_count )
{
    if ( !type_data )
        return TYPE_INVALID;

    // 1. Find or create the type slot (or reuse)

    uint16_t type_id = rf_find_or_create_type_slot( type_data );
    if ( type_id == TYPE_INVALID )
        return TYPE_INVALID;    // Registry is full

    // 2. Get the target slot

    type_t* target_type = &g_registry.type_array[ type_id ];

    // 3. Update the type's data from the new registration
    // This re-validates an old type or sets up a new one.

    target_type->name_sid  = type_data->name_sid;     // (should be same, but good to copy)
    target_type->hash      = type_data->hash;         // (should be same, but good to copy)
    target_type->size      = type_data->size;         // size of the type (could change)
    target_type->module_id = type_data->module_id;    // (should be same, but good to copy)
    target_type->valid     = 1;                       // Mark it as valid!

    // 4. Check if fields need updating

    if ( target_type->field_count != field_count )
    {
        uint16_t field_index = 0;    // Default for zero fields
        if ( field_count > 0 )
        {
            field_index = rf_add_fields( fields, field_count );
            if ( field_index == TYPE_INVALID )
            {
                target_type->valid = 0;    // Failed, mark invalid
                assert( 0 && "Failed to add fields during type update!" );
                return TYPE_INVALID;
            }
        }

        // Point the type to the new field block
        target_type->field_index = field_index;
        target_type->field_count = field_count;
    }

    // Update the caller's struct (in case it's caching)
    type_data->field_index = target_type->field_index;
    type_data->field_count = target_type->field_count;

    return type_id;
}

/*============================================================================================*/
// Resolve all field subtypes (safe to call multiple times).
// This is allowed to fail silently -- unresolved types remain TYPE_INVALID.

void
rf_resolve_fields( void )
{
    for ( uint16_t i = 0; i < g_registry.field_count; i++ )
    {
        /* only attempt resolve if there is a hash and it's currently unresolved */
        field_t* f = &g_registry.field_array[ i ];
        if ( f->type_id == TYPE_INVALID && f->type_hash != 0 )
        {
            // resolve hash to existing type id.
            // Will still be TYPE_INVALID if not registered yet
            f->type_id = rf_get_tid_from_hash( f->type_hash );
        }
    }
}

/*============================================================================================*/
// Ensure all field types are resolved -- Called after all modules are loaded to verify.

bool
rf_ensure_resolve( void )
{
    // Ensure all field types are resolved.
    // Called after all modules are loaded.

    bool failed = false;
    for ( uint16_t i = 0; i < g_registry.field_count; i++ )
    {
        field_t* f = &g_registry.field_array[ i ];
        if ( f->type_id == TYPE_INVALID )
        {
            printf( "...unresolved field: %s :field index %u with type_hash %u\n", sid_cstr( f->name_sid ), i,
                    f->type_hash );
            failed = true;
        }
    }

    if ( failed == true )
    {
        printf( "/**************************************************************/\n" );
        printf( "unresolved types exis!\n" );
        printf( "/**************************************************************/\n" );
        return false;
    }

    return true;    // all types resolved
}

/*============================================================================================*/
// Unregister all types with module_id that may need to be updated on module reload.

void
rf_unregister_module( uint8_t module_id )
{
    /* invalidate types that need to be re-updated after loading again */

    for ( uint16_t i = 0; i < g_registry.type_count; ++i )
    {
        type_t* t = &g_registry.type_array[ i ];
        if ( t->module_id == module_id )
        {
            t->valid = 0; /* mark invalid */
        }
    }
}

/*============================================================================================*/

const type_t*
rf_get_type_from_id( uint32_t id )
{
    /* get type by id */

    if ( id == TYPE_INVALID || id >= g_registry.type_count )
        return &g_registry.type_array[ 0 ];    // safe invalid

    return &g_registry.type_array[ id ];
}

/*============================================================================================*/

const type_t*
rf_get_type_from_name( const char* type_name )
{
    /* get type by name */

    const uint32_t hash    = sid_hash( type_name );
    const uint16_t type_id = rf_get_tid_from_hash( hash );
    return rf_get_type_from_id( type_id );
}

/*============================================================================================*/

uint16_t
rf_get_tid_from_hash( uint32_t hash )
{
    /* get type id by hash */

    if ( hash == 0 )
        return TYPE_INVALID;

    uint32_t slot = ( hash ) & ( TYPE_HASH_SIZE - 1 );
    uint16_t idx  = g_registry.type_hash[ slot ];
    while ( idx != TYPE_INVALID )
    {
        type_t* t = &g_registry.type_array[ idx ];
        if ( t->hash == hash )
        {
            return idx;    // found it!
        }
        idx = t->next;
    }
    return TYPE_INVALID;
}

/*============================================================================================*/

uint16_t
rf_get_tid_from_name( const char* name )
{
    /* get type id by name */

    if ( !name )
        return TYPE_INVALID;

    return rf_get_tid_from_hash( sid_hash( name ) );
}

/*============================================================================================*/

const field_t*
rf_get_first_field( uint32_t type_id )
{
    /* get first field of type by id */

    const type_t* t = rf_get_type_from_id( type_id );
    if ( t == NULL || t->field_count == 0 )
        return NULL;

    return &g_registry.field_array[ t->field_index ];
}

/*============================================================================================*/

const field_t*
rf_get_field( uint32_t field_id )
{
    /* get field by id */

    if ( field_id > g_registry.field_count )
        return NULL;

    field_t* f = &g_registry.field_array[ field_id ];
    assert( f->type_id != TYPE_INVALID && "using invalid type reference" );
    return f;
}

/*============================================================================================*/
// Call cb(field_index, field_ptr, user) for each field of a type_id.
// Returns the number of fields visited.

uint16_t
rf_each_field( u32 type_id, rf_field_cb cb, void* user )
{
    if ( type_id == TYPE_INVALID || type_id >= g_registry.type_count )
        return 0;    // not a valid type

    type_t* t = &g_registry.type_array[ type_id ];
    if ( !t->valid )
        return 0;    // type is invalid

    u16 count = t->field_count;
    u16 start = t->field_index;
    for ( uint16_t i = 0; i < count; ++i )
    {
        cu16 fi = start + i;
        if ( fi >= g_registry.field_count )
            break;
        cb( fi, &g_registry.field_array[ fi ], user );
    }
    return count;
}

/*============================================================================================*/