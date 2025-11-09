/*============================================================================================*/
#include <assert.h>

#include "orb.h"
#include "reflection.h"
#include "str_intern.h"

/*============================================================================================*/

static registry_t g_registry;

/*============================================================================================*/

void
registry_init( void )
{
    sid_init();
    memset( &g_registry, 0, sizeof( g_registry ) );

    for ( int i = 0; i < TYPE_HASH_SIZE; i++ ) g_registry.type_hash[ i ] = TYPE_INVALID;

    static const char* builtin_names[] = {
        "NULL", "bool", "int", "float", "double",
    };
    u16 builtin_sizes[] = {
        0, 1, 4, 4, 8,
    };

    for ( uint16_t i = 0; i < RF_TYPE_BUILT_IN; ++i )
    {
        type_t t    = { 0 };
        t.name_sid  = sid_intern_cstr( builtin_names[ i ] );
        t.hash      = sid_hash( builtin_names[ i ] );
        t.size      = builtin_sizes[ i ];
        t.valid     = 1;
        t.module_id = 0;

        // add to type array
        g_registry.type_array[ i ] = t;

        // add to hash table
        uint32_t slot                = t.hash % TYPE_HASH_SIZE;
        t.next                       = g_registry.type_hash[ slot ];
        g_registry.type_hash[ slot ] = i;
    }

    g_registry.type_count = RF_TYPE_BUILT_IN;
}

/*============================================================================================*/

void
registry_exit( void )
{
    memset( &g_registry, 0, sizeof( g_registry ) );
    sid_shutdown();
}

/*============================================================================================*/
// Get type by id

const type_t*
reflect_get_type( uint32_t id )
{
    if ( id == TYPE_INVALID || id >= g_registry.type_count )
        return &g_registry.type_array[ 0 ];    // safe invalid

    return &g_registry.type_array[ id ];
}

/*============================================================================================*/
// Lookup by hash (fast path)

uint16_t
reflect_get_type_id_hash( uint32_t hash )
{
    registry_t* reg = &g_registry;
    uint16_t    i   = reg->type_hash[ hash % TYPE_HASH_SIZE ];
    while ( i != TYPE_INVALID )
    {
        if ( reg->type_array[ i ].hash == hash )
            return i;

        i = reg->type_array[ i ].next;
    }
    return TYPE_INVALID;
}

/*============================================================================================*/
// Lookup by string (for first time resolve)

uint16_t
reflect_get_type_id( const char* name )
{
    return reflect_get_type_id_hash( sid_hash( name ) );
}

/*============================================================================================*/
// Find type by name.

const type_t*
registry_find_type( const char* type_name )
{
    const uint32_t hash    = sid_hash( type_name );
    const uint16_t type_id = reflect_get_type_id_hash( hash );
    return reflect_get_type( type_id );
}

/*============================================================================================*/
// Add new type (returns id)

uint16_t
reflect_register_type( const type_t* new_type )
{
    // check for space
    registry_t* reg = &g_registry;
    if ( reg->type_count >= MAX_TYPES )
    {
        assert( 0 && "Type registry full!" );
        return TYPE_INVALID;
    }

    // copy new type into type array
    uint16_t type_id = reg->type_count++;              // new type id
    type_t*  type    = &reg->type_array[ type_id ];    // new type entry
    *type            = *new_type;                      // copy type data

    // insert into hash table
    uint32_t slot          = new_type->hash % TYPE_HASH_SIZE;    // hash slot
    type->next             = reg->type_hash[ slot ];             // chain to previous
    reg->type_hash[ slot ] = type_id;                            // insert into hash table

    return type_id;    // new registered type id
}

/*============================================================================================*/
// Register fields (copy from static table)

uint16_t
reflect_register_fields( const field_t* src, uint16_t count )
{
    registry_t* reg = &g_registry;

    if ( reg->field_count + count > MAX_FIELDS )
    {
        assert( 0 && "Field registry full!" );
        return TYPE_INVALID;
    }

    uint16_t index = reg->field_count;
    memcpy( &reg->field_array[ index ], src, count * sizeof( field_t ) );
    reg->field_count += count;
    return index;    // first field offset
}

/*============================================================================================*/
// Resolve all field subtype hashes to type ids

void
reflect_resolve_field_types( void )
{
    for ( uint16_t i = 0; i < g_registry.field_count; i++ )
    {
        field_t* f = &g_registry.field_array[ i ];
        if ( f->type_id == RF_TYPE_INVALID && f->type_hash )
        {
            // resolve hash to existing type id.
            f->type_id = reflect_get_type_id_hash( f->type_hash );
        }
    }
}

/*============================================================================================*/
field_t*
reflect_get_field_by_id( uint32_t type_id )
{
    const type_t* t = reflect_get_type( type_id );
    if ( t == NULL || t->field_count == 0 )
        return NULL;
    return &g_registry.field_array[ t->field_index ];
}

/*============================================================================================*/