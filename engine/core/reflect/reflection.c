/*==============================================================================================

    reflections.c

==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "str_intern.h"
#include "reflection.h"

/*============================================================================================*/

#define MODULE_GAME 1

typedef struct vec3_s    // sample vector type
{
    float x, y, z;
} vec3_t;

typedef struct player_s
{
    vec3_t pos;
    float  health;
} player_t;

/*============================================================================================*/

extern registry_t g_registry;    // registry.c

/*============================================================================================*/

// -----------------------------------------------------------------------------
// Example generated registration function
// -----------------------------------------------------------------------------


void
reflect_register_game_module( void )
{
    // Hashes (precomputed by generator normally)
    // const uint32_t hash_player = sid_hash( "player_t" );
    // const uint32_t hash_float  = sid_hash( "float" );
    // const uint32_t hash_vec3   = sid_hash( "vec3_t" );

    // Build fields in code
    field_t player_fields[ 2 ];
    memset( player_fields, 0, sizeof( player_fields ) );

    player_fields[ 0 ].name_sid  = sid_intern_cstr( "health" );
    player_fields[ 0 ].offset    = offsetof( player_t, health );
    player_fields[ 0 ].size      = 4;
    player_fields[ 0 ].type_hash = sid_hash( "float" );
    player_fields[ 0 ].type_id   = RF_TYPE_INVALID;

    player_fields[ 1 ].name_sid  = sid_intern_cstr( "pos" );
    player_fields[ 1 ].offset    = offsetof( player_t, pos );
    player_fields[ 1 ].size      = sizeof( vec3_t );
    player_fields[ 1 ].type_hash = sid_hash( "vec3_t" );
    player_fields[ 1 ].type_id   = RF_TYPE_INVALID;

    uint16_t field_index         = reflect_register_fields( player_fields, 2 );

    // Build type record
    type_t player_type      = { 0 };
    player_type.name_sid    = sid_intern_cstr( "Player" );
    player_type.hash        = sid_hash( "player_t" );
    player_type.size        = sizeof( player_t );
    player_type.field_index = field_index;
    player_type.field_count = 2;
    player_type.module_id   = 1;
    player_type.valid       = 1;

    reflect_register_type( &player_type );
}

void
reflection_test( void )
{
    registry_init();

    reflect_register_game_module();
    reflect_resolve_field_types();


    // printf( "\n========================================\n" );
    // printf( "\tReflection System Examples\n" );
    // printf( "========================================\n" );
    //
    // int rsize = sizeof( type_t );
    // int fsize = sizeof( field_t );
    //
    // UNUSED( rsize );
    // UNUSED( fsize );
    //
    // registry_init();
    //
    // // Sample types to register.
    // static const char*    type_names[]   = { "Vec3", "Player" };
    // static const uint16_t type_sizes[]   = { sizeof( float ) * 3, 32 };
    // static const uint16_t field_counts[] = { 3, 2 };
    //
    // // Sample fields to register.
    // field_t fields[] = {
    //     // Vec3
    //     { 0 /* SID */, offsetof( vec3_t, x ), /* size */ 4, RF_TYPE_FLOAT, 0, 0 }, // x float
    //     { 0 /* SID */, offsetof( vec3_t, y ), /* size */ 4, RF_TYPE_FLOAT, 0, 0 }, // y float
    //     { 0 /* SID */, offsetof( vec3_t, z ), /* size */ 4, RF_TYPE_FLOAT, 0, 0 }, // z float
    //
    //     // Player
    //     { 0 /* SID */, /* off */ 0, /* size */ 4, RF_TYPE_FLOAT, 0, sid_hash( "Vec3" ) }, // depends on
    //     Vec3 { 0 /* SID */, /* off */ 4, /* size */ 4, RF_TYPE_FLOAT, 0, 0 }  // health float
    // };
    //
    // // Assign field names (interned SIDs)
    // const char* names[] = { "x", "y", "z", "pos", "health" };
    //
    // for ( int i = 0; i < 5; i++ )
    // {
    //     fields[ i ].name_sid = sid_intern_cstr( names[ i ] );
    // }
    //
    // registry_register_types( 1, type_names, type_sizes, field_counts, fields, 2 );
    // registry_resolve_dependencies();    // <-- slow but called infrequently
    //
    // Lookup vector
    {
        const type_t* t = registry_find_type( "Vec3" );
        printf( "Found type Vec3, %u fields\n", t->field_count );
        for ( uint32_t f = 0; f < t->field_count; ++f )
        {
            field_t* fld = reflect_get_field_by_id( t->field_index + f );
            printf( "  Field %s offset=%u subtype_id=%u\n", sid_cstr( fld->name_sid ), fld->offset, fld->type_id );
        }
    }
    // Lookup player after adding
    {
        const type_t* t = registry_find_type( "Player" );
        printf( "Found type Player, %u fields\n", t->field_count );
        for ( uint32_t f = 0; f < t->field_count; ++f )
        {
            field_t* fld = reflect_get_field_by_id( t->field_index + f );
            printf( "  Field %s offset=%u subtype_id=%u\n", sid_cstr( fld->name_sid ), fld->offset, fld->type_id );
        }
    }

    registry_exit();
}

/*============================================================================================*/
