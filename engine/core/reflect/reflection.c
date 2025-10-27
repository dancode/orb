/*==============================================================================================

    reflections.c

==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "reflection.h"
#include "str_intern.h"

/*============================================================================================*/


typedef struct vec3_s
{
    float x, y, z;
} vec3_t;

extern registry_t g_registry;


void
reflection_test( void )
{
    int rsize = sizeof( type_t );
    int fsize = sizeof( field_t );

    UNUSED( rsize );
    UNUSED( fsize );

    registry_init();

    /* --- Simulate generated reflection data --- */
    static const char*    type_names[]   = { "Vec3", "Player" };
    static const uint16_t type_sizes[]   = { sizeof( float ) * 3, 32 };
    static const uint16_t field_counts[] = { 3, 2 };

    field_t               fields[]       = {
        // Vec3
        { 0, offsetof( vec3_t, x ), 4, 1, 0, 0 }, // x float
        { 0, offsetof( vec3_t, y ), 4, 1, 0, 0 }, // y float
        { 0, offsetof( vec3_t, z ), 4, 1, 0, 0 }, // z float

        // Player
        { 0, 0, 4, 1, 0, sid_hash( "Vec3" ) }, // depends on Vec3
        { 0, 4, 4, 1, 0, 0 }  // health float
    };

    // Intern names for fields
    const char* names[] = { "x", "y", "z", "pos", "health" };
    for ( int i = 0; i < 5; i++ ) fields[ i ].name_sid = str_intern( names[ i ] ).off;

    registry_register_types( 1, type_names, type_sizes, field_counts, fields, 2 );
    registry_resolve_dependencies();    // <-- slow but called infrequently

    const type_t* t = registry_find_type( "Player" );
    printf( "Found type Player, %u fields\n", t->field_count );
    for ( uint32_t f = 0; f < t->field_count; ++f )
    {
        field_t* fld = &g_registry.field_array[ t->field_index + f ];
        printf( "  Field %s offset=%u subtype_id=%u\n", str_from_sid( ( sid_t ){ 0, fld->name_sid } ),
                fld->offset, fld->sub_type_id );
    }
}

/*============================================================================================*/
