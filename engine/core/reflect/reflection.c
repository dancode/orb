/*==============================================================================================

    reflections.c

==============================================================================================*/
#include <stdio.h>
#include <string.h>

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
    int    id;
    float  health;
    vec3_t pos;
} player_t;

/*============================================================================================*/

extern registry_t g_registry;    // registry.c

/*============================================================================================*/

void
rf_register_game_module_for_test( void )
{
    /* Precompute hashes (generator would have emitted these constants) */
    uint32_t hash_player = sid_hash( "player_t" );
    uint32_t hash_float  = sid_hash( "float" );
    uint32_t hash_vec3   = sid_hash( "vec3_t" );

    /* Create field array locally (we must call sid_intern_cstr at runtime) */
    field_t fields[ 2 ];
    memset( fields, 0, sizeof( fields ) );

    fields[ 0 ].name_sid  = sid_intern_cstr( "health" );
    fields[ 0 ].offset    = ( uint32_t )offsetof( player_t, health );
    fields[ 0 ].size      = ( uint32_t )sizeof( float );
    fields[ 0 ].kind      = 0;
    fields[ 0 ].type_id   = TYPE_INVALID;
    fields[ 0 ].type_hash = hash_float;

    fields[ 1 ].name_sid  = sid_intern_cstr( "pos" );
    fields[ 1 ].offset    = ( uint32_t )offsetof( player_t, pos );
    fields[ 1 ].size      = ( uint32_t )sizeof( vec3_t );
    fields[ 1 ].kind      = 0;
    fields[ 1 ].type_id   = TYPE_INVALID;
    fields[ 1 ].type_hash = hash_vec3;

    /* Prepare type struct (note: we do not set field_index here; reflect_register_type_with_fields does) */
    type_t player_type;
    memset( &player_type, 0, sizeof( player_type ) );
    player_type.name_sid  = sid_intern_cstr( "player_t" );
    player_type.hash      = hash_player;
    player_type.size      = ( uint16_t )sizeof( player_t );
    player_type.module_id = 1;
    player_type.valid     = 1;

    /* Register together (safe) */
    uint16_t tid = rf_add_type_with_fields( &player_type, fields, 2 );
    ( void )tid;
}


void
test_iterate( uint16_t idx, const field_t* f, void* data )
{
    UNUSED( data );
    const char* fname = sid_cstr( f->name_sid );
    printf( " field[%u] %s offset=%u size=%u type_id=%u\n", ( unsigned )idx, fname, ( unsigned )f->offset,
            ( unsigned )f->size, ( unsigned )f->type_id );
};

    /*============================================================================================*/

void
reflection_test( void )
{
    /**************************************************************/

    rf_init();    
    rf_register_game_module_for_test();
    rf_resolve_fields();
    rf_ensure_resolve();

    /**************************************************************/

    /* lookup player type id by name */
    u32 pid = rf_get_tid_from_name( "player_t" );
    printf( "Player type id = %u\n", ( unsigned )pid );

    /* iterate fields */
    rf_each_field( pid, test_iterate, NULL );

    /* lookup vector */
    {
        const type_t* t = rf_get_type_from_name( "vec3_t" );
        printf( "Found type Vec3, %u fields\n", t->field_count );
        for ( uint32_t f = 0; f < t->field_count; ++f )
        {
            const field_t* fld = rf_get_field( t->field_index + f );
            printf( "  Field %s offset=%u subtype_id=%u\n", sid_cstr( fld->name_sid ), fld->offset, fld->type_id );
        }
    }
    /* lookup player */
    {
        const type_t* t = rf_get_type_from_name( "player_t" );
        printf( "Found type Player, %u fields\n", t->field_count );
        for ( uint32_t f = 0; f < t->field_count; ++f )
        {
            const field_t* fld = rf_get_field( t->field_index + f );
            printf( "  Field %s offset=%u subtype_id=%u\n", sid_cstr( fld->name_sid ), fld->offset, fld->type_id );
        }
    }

    rf_exit();
}

/*============================================================================================*/
