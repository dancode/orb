/*============================================================================================*/
#include <assert.h>

#include "orb.h"
#include "reflection.h"
#include "str_intern.h"

/*============================================================================================*/

registry_t g_registry;

void
registry_init( void )
{
    str_intern_init();
    memset( &g_registry, 0, sizeof( g_registry ) );
}

uint32_t
registry_register_types( uint8_t         module_id,
                         const char**    type_names,
                         const uint16_t* type_sizes,
                         const uint16_t* field_counts,
                         const field_t*  fields,
                         uint16_t        type_count )
{
    UNUSED( module_id );
    UNUSED( type_names );
    UNUSED( type_sizes );
    UNUSED( field_counts );
    UNUSED( fields );

    // add all the fields for each type.
    uint16_t field_index = g_registry.field_count;
    for ( uint32_t i = 0; i < type_count; ++i )
    {
        if ( g_registry.type_count >= MAX_TYPES )
        {
            assert( 0 );
            break;
        }
        sid_t nameid = str_intern( type_names[ i ] );

        // Copy the type into type pool
        type_t* t      = &g_registry.type_array[ g_registry.type_count ];
        t->name_sid    = nameid.off;
        t->size        = type_sizes[ i ];
        t->field_index = field_index;
        t->field_count = field_counts[ i ];
        t->module_id   = module_id;
        t->valid       = 1;

        UNUSED( t );
        UNUSED( nameid );

        // Copy the fields into field pool
        for ( uint32_t f = 0; f < field_counts[ i ]; ++f )
        {
            g_registry.field_array[ field_index + f ] = fields[ field_index + f ];
        }

        g_registry.type_count++;
        field_index += field_counts[ i ];    // incremenet n
        g_registry.field_count = field_index;
    }
    return g_registry.type_count;
}

/*============================================================================================*/

// TDOD: optimize this with a hash map?

int
registry_find_type_by_hash( uint32_t hash )
{
    for ( uint32_t id = 0; id < g_registry.type_count; ++id )
    {
        // test every name against the hash -- one by one.
        const type_t* type = &g_registry.type_array[ id ];
        // const char*   name = str_from_sid( ( sid_t ){ 0, type->name_sid } );
        const char* name = str_from_off( type->name_sid );
        uint32_t    name_hash = sid_hash( name );
        if ( name_hash == hash )
            return ( int )id; // <-- found it!
    }
    return -1;    // type not found
}

const type_t*
registry_find_type( const char* type_name )
{
    uint32_t hash = sid_hash( type_name );
    int      id   = registry_find_type_by_hash( hash );
    return id >= 0 ? &g_registry.type_array[ id ] : NULL;
}

/*============================================================================================*/

void
registry_resolve_dependencies( void )
{
    for ( uint32_t i = 0; i < g_registry.type_count; ++i )
    {
        type_t* t = &g_registry.type_array[ i ];
        for ( uint32_t f = 0; f < t->field_count; ++f )
        {
            field_t* fld = &g_registry.field_array[ t->field_index + f ];
            if ( fld->sub_type_hash )
            {
                int tid          = registry_find_type_by_hash( fld->sub_type_hash );
                fld->sub_type_id = tid >= 0 ? ( uint16_t )tid : 0;
            }
        }
    }
}

/*============================================================================================*/