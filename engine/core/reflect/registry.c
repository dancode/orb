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

uint16_t
rf_add_fields( const field_t* fields, uint16_t count )
{
    if ( !fields || count == 0 )
    {
        assert( 0 && "Invalid field registration!" );
        return TYPE_INVALID; /* not enough room */
    }

    if ( g_registry.field_count + count > MAX_FIELDS )
    {
        assert( 0 && "Field registry full!" );
        return TYPE_INVALID;
    }

    uint16_t start = g_registry.field_count;

    /* copy memory in one go */
    memcpy( &g_registry.field_array[ start ], fields, ( size_t )count * sizeof( field_t ) );
    g_registry.field_count += count;

    return start;    // first field index
}

/*============================================================================================*/
// Add new type (returns id)

uint16_t
rf_add_type( const type_t* src_type )
{
    if ( !src_type )
        return TYPE_INVALID;

    if ( g_registry.type_count >= MAX_TYPES )
    {
        assert( 0 && "Type registry full!" );
        return TYPE_INVALID;
    }

    // allocate a new type id
    uint16_t id = g_registry.type_count++;

    // copy type into registry
    memset( &g_registry.type_array[ id ], 0, sizeof( type_t ) );
    g_registry.type_array[ id ] = *src_type;

    // ake sure next is initialized
    g_registry.type_array[ id ].next = TYPE_INVALID;

    // insert into hash table
    uint32_t slot                    = ( src_type->hash ) & ( TYPE_HASH_SIZE - 1 );
    uint16_t old_head                = g_registry.type_hash[ slot ];
    g_registry.type_array[ id ].next = old_head;
    g_registry.type_hash[ slot ]     = id;

    return id;    // return new type id
}

/*============================================================================================*/
// Convenience function to register type with fields (returns id)
// Ensures field index and count are set correctly.

uint16_t
rf_add_type_with_fields( type_t* type, const field_t* fields, uint16_t field_count )
{
    if ( !type )
        return TYPE_INVALID;

    /* register fields first */
    uint16_t field_index = rf_add_fields( fields, field_count );
    if ( field_index == TYPE_INVALID )
        return TYPE_INVALID;

    /* prepare local copy of type to set correct field indices */

    type_t local      = *type;
    local.field_index = field_index;
    local.field_count = field_count;

    /* register the type */
    uint16_t type_id = rf_add_type( &local );
    return type_id;
}

/*============================================================================================*/
// Resolve all field subtypes (safe to call multiple times). Writes cached type_id into field.type_id

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
/* Resolve a single field's subtype on demand (returns resolved id or TYPE_INVALID) */

uint16_t
rf_resolve_field( field_t* f )
{
    if ( !f )
        return TYPE_INVALID;
    if ( f->type_hash == 0 )
        return TYPE_INVALID;
    if ( f->type_id != TYPE_INVALID )
        return f->type_id;

    f->type_id = rf_get_tid_from_hash( f->type_hash );
    return f->type_id;
}

void
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
            printf( "...unresolved field: %s :field index %u with type_hash %u\n",
                    sid_cstr( f->name_sid ), i, f->type_hash );
            failed = true;
        }
    }

    if ( failed == true )
    {
        printf( "/**************************************************************/\n" );
        printf( "unresolved types exis!\n" );
        printf( "/**************************************************************/\n" );
        // assert( 0 && "Some field types could not be resolved!" );
    }
}

/*============================================================================================*/
// Unregister all types with module_id: unlinks them from hash buckets and marks invalid.

void
rf_unregister_module( uint8_t module_id )
{
    // iterate all hash slots
    for ( uint32_t slot = 0; slot < TYPE_HASH_SIZE; ++slot )
    {
        uint16_t prev = TYPE_INVALID;
        uint16_t cur  = g_registry.type_hash[ slot ];

        // walk chain and remove matching module types
        while ( cur != TYPE_INVALID )
        {
            type_t*  type = &g_registry.type_array[ cur ];
            uint16_t next = type->next;

            if ( type->valid && type->module_id == module_id )
            {
                /* unlink cur from chain */
                if ( prev == TYPE_INVALID )
                {
                    g_registry.type_hash[ slot ] = next;
                }
                else
                {
                    g_registry.type_array[ prev ].next = next;
                }

                type->valid = 0; /* mark invalid */

                /* optionally clear fields or leave them (depending on design) */
                /* for now fields will remain and will be recteated on module reload */
                /* if the module type changed on reload, we will create a new entry and leave the old one */
                /* this will avoid dangling pointers in existing data structures */
            }
            else
            {
                prev = cur;
            }
            cur = next;
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
        if ( t->valid && t->hash == hash )
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