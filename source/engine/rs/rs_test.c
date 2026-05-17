/*==============================================================================================

    engine/rs/rs_test.c - Exercise the rs_ reflection system.

    Compiled as a standalone TU in the test sandbox (sb_engine_core_reflect).
    Depends only on engine_rs.  rs_init() uses the internal string pool so no
    external interner is needed.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "orb.h"
#include "engine/rs/rs.h"

#define test_intern rs_intern_name
#define test_cstr   rs_name_cstr

typedef struct rs_test_vec3_s
{
    float x, y, z;
} rs_test_vec3_t;

typedef struct rs_test_transform_s
{
    rs_test_vec3_t position;
    rs_test_vec3_t rotation;
    rs_test_vec3_t scale;
} rs_test_transform_t;

typedef struct rs_test_entity_s
{
    int32_t              id;
    char                 name[ 64 ];
    rs_test_transform_t  transform;
    float                health;
    rs_test_vec3_t*      velocity;       /* pointer field    */
    const char*          label;          /* const char*      */
    rs_test_vec3_t*      slots[ 8 ];     /* array-of-pointer */
} rs_test_entity_t;

/*==============================================================================================
    Registration helpers
==============================================================================================*/

static void
rs_test_register_vec3( void )
{
    const uint32_t h_vec3  = rs_hash_str( "vec3_t" );
    const uint32_t h_float = rs_hash_str( "float" );

    rs_type_t type = { 0 };
    type.name_id  = test_intern( "vec3_t" );
    type.hash      = h_vec3;
    type.size      = RS_SIZEOF( rs_test_vec3_t );
    type.align     = RS_ALIGNOF( rs_test_vec3_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 3 ] = { 0 };

    fields[ 0 ].name_id   = test_intern( "x" );
    fields[ 0 ].name_hash = rs_hash_str( "x" );
    fields[ 0 ].type_hash = h_float;
    fields[ 0 ].offset    = RS_OFFSETOF( rs_test_vec3_t, x );
    fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_vec3_t, x );
    fields[ 0 ].mods      = RS_NO_MODS;

    fields[ 1 ].name_id   = test_intern( "y" );
    fields[ 1 ].name_hash = rs_hash_str( "y" );
    fields[ 1 ].type_hash = h_float;
    fields[ 1 ].offset    = RS_OFFSETOF( rs_test_vec3_t, y );
    fields[ 1 ].size      = RS_FIELD_SIZE( rs_test_vec3_t, y );

    fields[ 2 ].name_id   = test_intern( "z" );
    fields[ 2 ].name_hash = rs_hash_str( "z" );
    fields[ 2 ].type_hash = h_float;
    fields[ 2 ].offset    = RS_OFFSETOF( rs_test_vec3_t, z );
    fields[ 2 ].size      = RS_FIELD_SIZE( rs_test_vec3_t, z );

    uint16_t tid = rs_register_type( &type, fields, 3 );

    rs_attrib_t a = { 0 };
    a.name_id    = test_intern( "serializable" );
    a.name_hash  = rs_hash_str( "serializable" );
    a.type        = RS_ATTR_BOOL;
    a.value.u32   = 1;
    rs_type_add_attr( tid, &a );
}

static void
rs_test_register_transform( void )
{
    const uint32_t h_xform = rs_hash_str( "transform_t" );
    const uint32_t h_vec3  = rs_hash_str( "vec3_t" );

    rs_type_t type = { 0 };
    type.name_id  = test_intern( "transform_t" );
    type.hash      = h_xform;
    type.size      = RS_SIZEOF( rs_test_transform_t );
    type.align     = RS_ALIGNOF( rs_test_transform_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 3 ] = { 0 };

    fields[ 0 ].name_id   = test_intern( "position" );
    fields[ 0 ].name_hash = rs_hash_str( "position" );
    fields[ 0 ].type_hash = h_vec3;
    fields[ 0 ].offset    = RS_OFFSETOF( rs_test_transform_t, position );
    fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_transform_t, position );

    fields[ 1 ].name_id   = test_intern( "rotation" );
    fields[ 1 ].name_hash = rs_hash_str( "rotation" );
    fields[ 1 ].type_hash = h_vec3;
    fields[ 1 ].offset    = RS_OFFSETOF( rs_test_transform_t, rotation );
    fields[ 1 ].size      = RS_FIELD_SIZE( rs_test_transform_t, rotation );

    fields[ 2 ].name_id   = test_intern( "scale" );
    fields[ 2 ].name_hash = rs_hash_str( "scale" );
    fields[ 2 ].type_hash = h_vec3;
    fields[ 2 ].offset    = RS_OFFSETOF( rs_test_transform_t, scale );
    fields[ 2 ].size      = RS_FIELD_SIZE( rs_test_transform_t, scale );

    rs_register_type( &type, fields, 3 );
}

static void
rs_test_register_entity( void )
{
    const uint32_t h_entity = rs_hash_str( "entity_t" );
    const uint32_t h_int32  = rs_hash_str( "int32_t" );
    const uint32_t h_char   = rs_hash_str( "char" );
    const uint32_t h_xform  = rs_hash_str( "transform_t" );
    const uint32_t h_float  = rs_hash_str( "float" );
    const uint32_t h_vec3   = rs_hash_str( "vec3_t" );

    rs_type_t type = { 0 };
    type.name_id  = test_intern( "entity_t" );
    type.hash      = h_entity;
    type.size      = RS_SIZEOF( rs_test_entity_t );
    type.align     = RS_ALIGNOF( rs_test_entity_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 7 ] = { 0 };

    /* id : int32_t                                  */
    fields[ 0 ].name_id   = test_intern( "id" );
    fields[ 0 ].name_hash = rs_hash_str( "id" );
    fields[ 0 ].type_hash = h_int32;
    fields[ 0 ].offset    = RS_OFFSETOF( rs_test_entity_t, id );
    fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_entity_t, id );

    /* name : char[64]    base=char, mods=[ARRAY], aux=64 */
    fields[ 1 ].name_id   = test_intern( "name" );
    fields[ 1 ].name_hash = rs_hash_str( "name" );
    fields[ 1 ].type_hash = h_char;
    fields[ 1 ].offset    = RS_OFFSETOF( rs_test_entity_t, name );
    fields[ 1 ].size      = RS_FIELD_SIZE( rs_test_entity_t, name );
    fields[ 1 ].mods      = RS_MODS( RS_M_ARRAY, RS_M_END, RS_M_END, RS_M_END );
    fields[ 1 ].aux       = 64;

    /* transform : transform_t                       */
    fields[ 2 ].name_id   = test_intern( "transform" );
    fields[ 2 ].name_hash = rs_hash_str( "transform" );
    fields[ 2 ].type_hash = h_xform;
    fields[ 2 ].offset    = RS_OFFSETOF( rs_test_entity_t, transform );
    fields[ 2 ].size      = RS_FIELD_SIZE( rs_test_entity_t, transform );

    /* health : float                                */
    fields[ 3 ].name_id   = test_intern( "health" );
    fields[ 3 ].name_hash = rs_hash_str( "health" );
    fields[ 3 ].type_hash = h_float;
    fields[ 3 ].offset    = RS_OFFSETOF( rs_test_entity_t, health );
    fields[ 3 ].size      = RS_FIELD_SIZE( rs_test_entity_t, health );

    /* velocity : vec3_t*  base=vec3, mods=[PTR]     */
    fields[ 4 ].name_id   = test_intern( "velocity" );
    fields[ 4 ].name_hash = rs_hash_str( "velocity" );
    fields[ 4 ].type_hash = h_vec3;
    fields[ 4 ].offset    = RS_OFFSETOF( rs_test_entity_t, velocity );
    fields[ 4 ].size      = RS_FIELD_SIZE( rs_test_entity_t, velocity );
    fields[ 4 ].mods      = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    /* label : const char*  base_const=1, mods=[PTR] */
    fields[ 5 ].name_id    = test_intern( "label" );
    fields[ 5 ].name_hash  = rs_hash_str( "label" );
    fields[ 5 ].type_hash  = h_char;
    fields[ 5 ].offset     = RS_OFFSETOF( rs_test_entity_t, label );
    fields[ 5 ].size       = RS_FIELD_SIZE( rs_test_entity_t, label );
    fields[ 5 ].base_const = 1;
    fields[ 5 ].mods       = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    /* slots : vec3_t*[8]   mods=[PTR, ARRAY], aux=8 */
    fields[ 6 ].name_id   = test_intern( "slots" );
    fields[ 6 ].name_hash = rs_hash_str( "slots" );
    fields[ 6 ].type_hash = h_vec3;
    fields[ 6 ].offset    = RS_OFFSETOF( rs_test_entity_t, slots );
    fields[ 6 ].size      = RS_FIELD_SIZE( rs_test_entity_t, slots );
    fields[ 6 ].mods      = RS_MODS( RS_M_PTR, RS_M_ARRAY, RS_M_END, RS_M_END );
    fields[ 6 ].aux       = 8;

    uint16_t tid = rs_register_type( &type, fields, 7 );

    /* Attach two attributes to the 'health' field to exercise the repeated-entry path. */
    const rs_field_t* health = rs_find_field( tid, "health" );
    if ( health )
    {
        uint16_t fid  = (uint16_t)( health - rs_get_field( 0 ) );
        rs_attrib_t a = { 0 };
        a.name_id    = test_intern( "range" );
        a.name_hash  = rs_hash_str( "range" );
        a.type        = RS_ATTR_FLOAT;
        a.value.f32   = 0.0f;
        rs_field_add_attr( fid, &a );
        a.value.f32   = 100.0f;
        rs_field_add_attr( fid, &a );
    }
}

/*==============================================================================================
    Test cases
==============================================================================================*/

static void
test_basic( void )
{
    printf( "\n=== rs: basic registration ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    rs_test_register_vec3();
    rs_test_register_transform();
    rs_test_register_entity();

    bool ok = rs_finalize_frame( game );
    printf( "finalize: %s\n", ok ? "OK" : "FAIL" );

    rs_print_frame( game );

    rs_pop_frame( game );
    rs_exit();
}

static void
test_hot_reload_rs( void )
{
    printf( "\n=== rs: hot reload via pop + repush ===\n" );

    rs_init();

    uint16_t game = rs_push_frame( "game", 1 );
    rs_test_register_vec3();
    rs_test_register_transform();
    rs_test_register_entity();
    rs_finalize_frame( game );

    uint16_t t_count, f_count, fr_count;
    rs_get_stats( &t_count, &f_count, &fr_count );
    printf( "  before reload: types=%u fields=%u frames=%u\n", t_count, f_count, fr_count );

    /* Drop and reload the 'game' frame. */
    rs_pop_frame( game );
    rs_get_stats( &t_count, &f_count, &fr_count );
    printf( "  after pop    : types=%u fields=%u frames=%u\n", t_count, f_count, fr_count );

    game = rs_push_frame( "game", 2 );
    rs_test_register_vec3();
    rs_test_register_transform();
    rs_test_register_entity();
    rs_finalize_frame( game );

    rs_get_stats( &t_count, &f_count, &fr_count );
    printf( "  after reload : types=%u fields=%u frames=%u\n", t_count, f_count, fr_count );

    /* Schema hash should be identical because the layout did not change. */
    uint16_t v1 = rs_find_type_by_name( "vec3_t" );
    if ( v1 != RS_TYPE_INVALID )
        printf( "  vec3_t schema_hash = 0x%08x\n", rs_get_type( v1 )->schema_hash );

    rs_pop_frame( game );
    rs_exit();
}

static void
test_mod_decode( void )
{
    printf( "\n=== rs: mod chain pretty-print ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    rs_test_register_vec3();
    rs_test_register_transform();
    rs_test_register_entity();
    rs_finalize_frame( game );

    uint16_t eid = rs_find_type_by_name( "entity_t" );
    rs_print_type( eid );

    rs_pop_frame( game );
    rs_exit();
}

/*==============================================================================================
    Enum test
==============================================================================================*/

typedef enum rs_test_color_e
{
    RS_TEST_COLOR_RED   = 0,
    RS_TEST_COLOR_GREEN = 1,
    RS_TEST_COLOR_BLUE  = 2,
    RS_TEST_COLOR_ALPHA = 0xFF,
} rs_test_color_t;

static void
test_enums( void )
{
    printf( "\n=== rs: enum registration ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    rs_type_t type = { 0 };
    type.name_id  = test_intern( "color_t" );
    type.hash      = rs_hash_str( "color_t" );
    type.size      = (uint16_t)sizeof( rs_test_color_t );
    type.align     = (uint8_t)_Alignof( rs_test_color_t );
    /* kind is forced to RS_KIND_ENUM by rs_register_enum */

    rs_enum_t entries[ 4 ] = {
        { .name_id = test_intern( "RED" ),   .name_hash = rs_hash_str( "RED" ),   .value = RS_TEST_COLOR_RED   },
        { .name_id = test_intern( "GREEN" ), .name_hash = rs_hash_str( "GREEN" ), .value = RS_TEST_COLOR_GREEN },
        { .name_id = test_intern( "BLUE" ),  .name_hash = rs_hash_str( "BLUE" ),  .value = RS_TEST_COLOR_BLUE  },
        { .name_id = test_intern( "ALPHA" ), .name_hash = rs_hash_str( "ALPHA" ), .value = RS_TEST_COLOR_ALPHA },
    };

    uint16_t tid = rs_register_enum( &type, entries, 4 );
    rs_finalize_frame( game );

    rs_print_type( tid );

    /* Lookup by name */
    const rs_enum_t* by_name = rs_enum_find_by_name( tid, "BLUE" );
    printf( "  find_by_name(\"BLUE\")  -> value=%lld\n",
            by_name ? (long long)by_name->value : -1 );

    /* Lookup by value */
    const rs_enum_t* by_value = rs_enum_find_by_value( tid, 0xFF );
    printf( "  find_by_value(0xFF)   -> name=%s\n",
            by_value ? test_cstr( by_value->name_id ) : "<none>" );

    /* Negative cases */
    if ( rs_enum_find_by_name( tid, "PURPLE" ) == NULL )
        printf( "  find_by_name(\"PURPLE\") correctly returned NULL\n" );

    /* Hot-reload survival: pop, repush, expect same schema_hash. */
    uint32_t schema_before = rs_get_type( tid )->schema_hash;
    rs_pop_frame( game );

    game = rs_push_frame( "game", 2 );
    tid  = rs_register_enum( &type, entries, 4 );
    rs_finalize_frame( game );

    uint32_t schema_after = rs_get_type( tid )->schema_hash;
    printf( "  schema_hash before=0x%08x after=0x%08x %s\n",
            schema_before, schema_after,
            ( schema_before == schema_after ) ? "(stable)" : "(MISMATCH)" );

    rs_pop_frame( game );
    rs_exit();
}

/*==============================================================================================
    Function-signature test

      Reflect:
          typedef void (*on_die_fn)( int32_t reason, vec3_t* loc );
          struct npc_t { float health; on_die_fn on_die; };
==============================================================================================*/

typedef struct rs_test_npc_s
{
    float        health;
    void       ( *on_die )( int32_t reason, rs_test_vec3_t* loc );
} rs_test_npc_t;

static void
test_function_sigs( void )
{
    printf( "\n=== rs: function signatures ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    /* vec3_t is needed by the signature's second parameter. */
    rs_test_register_vec3();

    /* --- Register the signature type "on_die_fn". --- */
    const uint32_t h_sig   = rs_hash_str( "on_die_fn" );
    const uint32_t h_void  = rs_hash_str( "void" );
    const uint32_t h_int32 = rs_hash_str( "int32_t" );
    const uint32_t h_vec3  = rs_hash_str( "vec3_t" );

    rs_type_t sig_type = { 0 };
    sig_type.name_id  = test_intern( "on_die_fn" );
    sig_type.hash      = h_sig;
    sig_type.size      = (uint16_t)sizeof( void* );    /* a pointer holds the callable */
    sig_type.align     = (uint8_t)_Alignof( void* );

    /* index 0 = return, indices 1..N = params */
    rs_field_t sig_fields[ 3 ] = { 0 };

    sig_fields[ 0 ].name_id   = test_intern( "return" );
    sig_fields[ 0 ].name_hash = rs_hash_str( "return" );
    sig_fields[ 0 ].type_hash = h_void;
    /* return: void  -> no mods, no aux */

    sig_fields[ 1 ].name_id   = test_intern( "reason" );
    sig_fields[ 1 ].name_hash = rs_hash_str( "reason" );
    sig_fields[ 1 ].type_hash = h_int32;
    sig_fields[ 1 ].size      = (uint16_t)sizeof( int32_t );
    /* int32_t value */

    sig_fields[ 2 ].name_id   = test_intern( "loc" );
    sig_fields[ 2 ].name_hash = rs_hash_str( "loc" );
    sig_fields[ 2 ].type_hash = h_vec3;
    sig_fields[ 2 ].size      = (uint16_t)sizeof( rs_test_vec3_t* );
    sig_fields[ 2 ].mods      = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    uint16_t sig_id = rs_register_function( &sig_type, sig_fields, 3 );

    /* --- Register a struct that contains an on_die callback field. --- */
    const uint32_t h_npc = rs_hash_str( "npc_t" );

    rs_type_t npc_type = { 0 };
    npc_type.name_id  = test_intern( "npc_t" );
    npc_type.hash      = h_npc;
    npc_type.size      = RS_SIZEOF( rs_test_npc_t );
    npc_type.align     = RS_ALIGNOF( rs_test_npc_t );
    npc_type.kind      = RS_KIND_STRUCT;

    rs_field_t npc_fields[ 2 ] = { 0 };

    npc_fields[ 0 ].name_id   = test_intern( "health" );
    npc_fields[ 0 ].name_hash = rs_hash_str( "health" );
    npc_fields[ 0 ].type_hash = rs_hash_str( "float" );
    npc_fields[ 0 ].offset    = RS_OFFSETOF( rs_test_npc_t, health );
    npc_fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_npc_t, health );

    /* on_die: void(*)(int32_t, vec3_t*)
         base = void, mods=[FUNCTION], aux=sig_id  */
    npc_fields[ 1 ].name_id   = test_intern( "on_die" );
    npc_fields[ 1 ].name_hash = rs_hash_str( "on_die" );
    npc_fields[ 1 ].type_hash = h_void;
    npc_fields[ 1 ].offset    = RS_OFFSETOF( rs_test_npc_t, on_die );
    npc_fields[ 1 ].size      = RS_FIELD_SIZE( rs_test_npc_t, on_die );
    npc_fields[ 1 ].mods      = RS_MODS( RS_M_FUNCTION, RS_M_END, RS_M_END, RS_M_END );
    npc_fields[ 1 ].aux       = sig_id;

    uint16_t npc_id = rs_register_type( &npc_type, npc_fields, 2 );

    rs_finalize_frame( game );

    printf( "\n-- signature --\n" );
    rs_print_type( sig_id );

    printf( "\n-- struct with callback --\n" );
    rs_print_type( npc_id );

    /* Sanity checks via the convenience accessors. */
    const rs_field_t* ret = rs_function_get_return( sig_id );
    printf( "  return resolves to type_id=%u  (expect void = %u)\n",
            ret ? ret->type_id : 0xFFFF, RS_PRIM_VOID );
    printf( "  param_count = %u\n", rs_function_param_count( sig_id ) );
    const rs_field_t* p1 = rs_function_get_param( sig_id, 1 );
    printf( "  param[1] '%s' base_type_id=%u (expect vec3_t)\n",
            p1 ? test_cstr( p1->name_id ) : "?", p1 ? p1->type_id : 0xFFFF );

    rs_pop_frame( game );
    rs_exit();
}

/*==============================================================================================
    Serialization round-trip test

      Reflect a struct that exercises all the redaction rules:
        - primitives, nested struct, inline char array        -> preserved
        - pointer field                                       -> zeroed on save
        - field marked @transient                             -> zeroed on save
      Then write, scribble the destination, read back, and verify byte-for-byte equality
      with the expected post-redaction state.
==============================================================================================*/

typedef struct rs_test_save_s
{
    int32_t          id;
    float            health;
    rs_test_vec3_t   position;       /* nested struct        */
    char             name[ 16 ];     /* inline array         */
    rs_test_vec3_t*  cache_ptr;      /* pointer - redacted   */
    uint32_t         cached_hash;    /* @transient - redacted */
} rs_test_save_t;

static void
test_serialize( void )
{
    printf( "\n=== rs: serialization round-trip ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    rs_test_register_vec3();

    /* Register save_t */
    const uint32_t h_save  = rs_hash_str( "save_t" );
    const uint32_t h_int32 = rs_hash_str( "int32_t" );
    const uint32_t h_u32   = rs_hash_str( "uint32_t" );
    const uint32_t h_float = rs_hash_str( "float" );
    const uint32_t h_vec3  = rs_hash_str( "vec3_t" );
    const uint32_t h_char  = rs_hash_str( "char" );

    rs_type_t type = { 0 };
    type.name_id  = test_intern( "save_t" );
    type.hash      = h_save;
    type.size      = RS_SIZEOF( rs_test_save_t );
    type.align     = RS_ALIGNOF( rs_test_save_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 6 ] = { 0 };

    fields[ 0 ].name_id   = test_intern( "id" );
    fields[ 0 ].name_hash = rs_hash_str( "id" );
    fields[ 0 ].type_hash = h_int32;
    fields[ 0 ].offset    = RS_OFFSETOF( rs_test_save_t, id );
    fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_save_t, id );

    fields[ 1 ].name_id   = test_intern( "health" );
    fields[ 1 ].name_hash = rs_hash_str( "health" );
    fields[ 1 ].type_hash = h_float;
    fields[ 1 ].offset    = RS_OFFSETOF( rs_test_save_t, health );
    fields[ 1 ].size      = RS_FIELD_SIZE( rs_test_save_t, health );

    fields[ 2 ].name_id   = test_intern( "position" );
    fields[ 2 ].name_hash = rs_hash_str( "position" );
    fields[ 2 ].type_hash = h_vec3;
    fields[ 2 ].offset    = RS_OFFSETOF( rs_test_save_t, position );
    fields[ 2 ].size      = RS_FIELD_SIZE( rs_test_save_t, position );

    fields[ 3 ].name_id   = test_intern( "name" );
    fields[ 3 ].name_hash = rs_hash_str( "name" );
    fields[ 3 ].type_hash = h_char;
    fields[ 3 ].offset    = RS_OFFSETOF( rs_test_save_t, name );
    fields[ 3 ].size      = RS_FIELD_SIZE( rs_test_save_t, name );
    fields[ 3 ].mods      = RS_MODS( RS_M_ARRAY, RS_M_END, RS_M_END, RS_M_END );
    fields[ 3 ].aux       = 16;

    fields[ 4 ].name_id   = test_intern( "cache_ptr" );
    fields[ 4 ].name_hash = rs_hash_str( "cache_ptr" );
    fields[ 4 ].type_hash = h_vec3;
    fields[ 4 ].offset    = RS_OFFSETOF( rs_test_save_t, cache_ptr );
    fields[ 4 ].size      = RS_FIELD_SIZE( rs_test_save_t, cache_ptr );
    fields[ 4 ].mods      = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    fields[ 5 ].name_id   = test_intern( "cached_hash" );
    fields[ 5 ].name_hash = rs_hash_str( "cached_hash" );
    fields[ 5 ].type_hash = h_u32;
    fields[ 5 ].offset    = RS_OFFSETOF( rs_test_save_t, cached_hash );
    fields[ 5 ].size      = RS_FIELD_SIZE( rs_test_save_t, cached_hash );

    uint16_t tid = rs_register_type( &type, fields, 6 );

    /* Mark cached_hash @transient. */
    const rs_field_t* hash_field = rs_find_field( tid, "cached_hash" );
    assert( hash_field );
    {
        uint16_t fid = (uint16_t)( hash_field - rs_get_field( 0 ) );
        rs_attrib_t a = { 0 };
        a.name_id    = test_intern( "transient" );
        a.name_hash  = rs_hash_str( "transient" );
        a.type        = RS_ATTR_BOOL;
        a.value.u32   = 1;
        rs_field_add_attr( fid, &a );
    }

    rs_finalize_frame( game );

    /* --- Source instance with distinctive values --- */
    rs_test_vec3_t dummy = { 9, 9, 9 };
    rs_test_save_t src   = { 0 };
    src.id           = 42;
    src.health       = 75.5f;
    src.position     = (rs_test_vec3_t){ 1.0f, 2.0f, 3.0f };
    memcpy( src.name, "hello", 6 );
    src.cache_ptr    = &dummy;      /* should be zeroed on save */
    src.cached_hash  = 0xCAFEBABE;  /* @transient - should be zeroed on save */

    /* --- Write --- */
    uint8_t buf[ 256 ];
    size_t  written = rs_write( &src, tid, buf, sizeof( buf ) );
    printf( "  wrote %zu bytes (header=%d + body=%u)\n",
            written, RS_SAVE_HEADER_SIZE, (unsigned)sizeof( rs_test_save_t ) );
    assert( written == (size_t)RS_SAVE_HEADER_SIZE + sizeof( rs_test_save_t ) );

    /* peek */
    uint32_t peeked = rs_peek_type_hash( buf, written );
    printf( "  peek_type_hash    = 0x%08x  (expect 0x%08x)\n", peeked, h_save );
    assert( peeked == h_save );

    /* --- Read back into a deliberately-trashed destination --- */
    rs_test_save_t dst;
    memset( &dst, 0xAA, sizeof( dst ) );

    size_t          consumed = 0;
    rs_io_status_t  st       = rs_read( &dst, tid, buf, written, &consumed );
    printf( "  read status=%d consumed=%zu\n", (int)st, consumed );
    assert( st == RS_IO_OK );
    assert( consumed == written );

    /* --- Verify --- */
    assert( dst.id          == 42 );
    assert( dst.health      == 75.5f );
    assert( dst.position.x  == 1.0f && dst.position.y == 2.0f && dst.position.z == 3.0f );
    assert( memcmp( dst.name, "hello", 6 ) == 0 );
    assert( dst.cache_ptr   == NULL );          /* pointer redacted */
    assert( dst.cached_hash == 0 );             /* transient redacted */
    printf( "  round-trip OK; pointer and @transient cleared as expected\n" );

    /* --- Schema-hash mismatch: corrupt the header and expect refusal --- */
    uint8_t corrupt[ 256 ];
    memcpy( corrupt, buf, written );
    corrupt[ 8 ] ^= 0x01;       /* flip a bit in schema_hash */
    st = rs_read( &dst, tid, corrupt, written, NULL );
    printf( "  corrupt schema -> status=%d (expect %d)\n", (int)st, RS_IO_INCOMPAT );
    assert( st == RS_IO_INCOMPAT );

    /* --- Truncated buffer --- */
    st = rs_read( &dst, tid, buf, 10, NULL );
    assert( st == RS_IO_TRUNCATED );

    rs_pop_frame( game );
    rs_exit();
}

/*==============================================================================================
    Bitset enum test
==============================================================================================*/

typedef enum rs_test_perm_e
{
    RS_TEST_PERM_NONE    = 0,
    RS_TEST_PERM_READ    = 1 << 0,
    RS_TEST_PERM_WRITE   = 1 << 1,
    RS_TEST_PERM_EXEC    = 1 << 2,
    RS_TEST_PERM_ALL     = RS_TEST_PERM_READ | RS_TEST_PERM_WRITE | RS_TEST_PERM_EXEC,
} rs_test_perm_t;

static void
test_bitset( void )
{
    printf( "\n=== rs: bitset enum ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    rs_type_t type = { 0 };
    type.name_id  = test_intern( "perm_t" );
    type.hash      = rs_hash_str( "perm_t" );
    type.size      = (uint16_t)sizeof( rs_test_perm_t );
    type.align     = (uint8_t)_Alignof( rs_test_perm_t );

    /* Order matters when decoding: place multi-bit masks (ALL) BEFORE the single-bit
       components so a value of 0b111 prints as "ALL" instead of "READ | WRITE | EXEC".
       Try swapping the order to see the alternative formatting. */
    rs_enum_t entries[ 5 ] = {
        { .name_id = test_intern( "NONE"  ), .name_hash = rs_hash_str( "NONE"  ), .value = RS_TEST_PERM_NONE  },
        { .name_id = test_intern( "ALL"   ), .name_hash = rs_hash_str( "ALL"   ), .value = RS_TEST_PERM_ALL   },
        { .name_id = test_intern( "READ"  ), .name_hash = rs_hash_str( "READ"  ), .value = RS_TEST_PERM_READ  },
        { .name_id = test_intern( "WRITE" ), .name_hash = rs_hash_str( "WRITE" ), .value = RS_TEST_PERM_WRITE },
        { .name_id = test_intern( "EXEC"  ), .name_hash = rs_hash_str( "EXEC"  ), .value = RS_TEST_PERM_EXEC  },
    };

    uint16_t tid = rs_register_bitset( &type, entries, 5 );
    rs_finalize_frame( game );

    printf( "  is_bitset(perm_t) = %s\n", rs_enum_is_bitset( tid ) ? "true" : "false" );
    assert( rs_enum_is_bitset( tid ) );

    char buf[ 64 ];

    /* 0 -> "NONE" via the zero-valued enumerator. */
    rs_bitset_describe( tid, 0, buf, sizeof( buf ) );
    printf( "  describe(0)               = \"%s\"\n", buf );

    /* READ|WRITE -> "READ | WRITE" */
    rs_bitset_describe( tid, RS_TEST_PERM_READ | RS_TEST_PERM_WRITE, buf, sizeof( buf ) );
    printf( "  describe(READ|WRITE)      = \"%s\"\n", buf );

    /* All bits -> "ALL" because the multi-bit mask comes first in registration. */
    rs_bitset_describe( tid, RS_TEST_PERM_ALL, buf, sizeof( buf ) );
    printf( "  describe(ALL)             = \"%s\"\n", buf );

    /* Unknown bits -> tail hex */
    rs_bitset_describe( tid, RS_TEST_PERM_READ | 0x100, buf, sizeof( buf ) );
    printf( "  describe(READ|0x100)      = \"%s\"\n", buf );

    /* find_flag(WRITE) */
    const rs_enum_t* w = rs_bitset_find_flag( tid, RS_TEST_PERM_WRITE );
    printf( "  find_flag(WRITE)          = %s\n", w ? test_cstr( w->name_id ) : "?" );
    assert( w && w->value == RS_TEST_PERM_WRITE );

    rs_pop_frame( game );
    rs_exit();
}

/*==============================================================================================
    Reference walker test

      Reflect:
          struct inner_t { vec3_t* p; };
          struct walk_t  { int32_t id; vec3_t* single; vec3_t* slots[3]; inner_t nested; };

      Expected visits: 1 (single) + 3 (slots) + 1 (nested.p) = 5
==============================================================================================*/

typedef struct rs_test_inner_s
{
    rs_test_vec3_t* p;
} rs_test_inner_t;

typedef struct rs_test_walk_s
{
    int32_t          id;
    rs_test_vec3_t*  single;
    rs_test_vec3_t*  slots[ 3 ];
    rs_test_inner_t  nested;
} rs_test_walk_t;

typedef struct
{
    int       count;
    uint16_t  expected_pointee_id;
} rs_test_walk_ctx_t;

static void
rs_test_walk_visitor( void** slot, uint16_t pointee_id, const rs_field_t* f, void* user )
{
    rs_test_walk_ctx_t* ctx = (rs_test_walk_ctx_t*)user;
    ctx->count++;

    const rs_type_t* pointee = rs_get_type( pointee_id );
    printf( "  visit field '%-12s' slot=%p -> %p  pointee=%s\n",
            test_cstr( f->name_id ),
            (void*)slot, *slot,
            pointee ? test_cstr( pointee->name_id ) : "?" );

    /* Loose sanity check: every visit in this test should point at a vec3_t. */
    assert( pointee_id == ctx->expected_pointee_id );
}

static void
test_walker( void )
{
    printf( "\n=== rs: reference walker ===\n" );

    rs_init();
    uint16_t game = rs_push_frame( "game", 1 );

    rs_test_register_vec3();

    /* Register rs_test_inner_t { vec3_t* p; } */
    {
        const uint32_t h_inner = rs_hash_str( "inner_t" );
        const uint32_t h_vec3  = rs_hash_str( "vec3_t" );

        rs_type_t type = { 0 };
        type.name_id  = test_intern( "inner_t" );
        type.hash      = h_inner;
        type.size      = RS_SIZEOF( rs_test_inner_t );
        type.align     = RS_ALIGNOF( rs_test_inner_t );
        type.kind      = RS_KIND_STRUCT;

        rs_field_t fields[ 1 ] = { 0 };

        fields[ 0 ].name_id   = test_intern( "p" );
        fields[ 0 ].name_hash = rs_hash_str( "p" );
        fields[ 0 ].type_hash = h_vec3;
        fields[ 0 ].offset    = RS_OFFSETOF( rs_test_inner_t, p );
        fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_inner_t, p );
        fields[ 0 ].mods      = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

        rs_register_type( &type, fields, 1 );
    }

    /* Register rs_test_walk_t */
    uint16_t walk_tid;
    {
        const uint32_t h_walk  = rs_hash_str( "walk_t" );
        const uint32_t h_int32 = rs_hash_str( "int32_t" );
        const uint32_t h_vec3  = rs_hash_str( "vec3_t" );
        const uint32_t h_inner = rs_hash_str( "inner_t" );

        rs_type_t type = { 0 };
        type.name_id  = test_intern( "walk_t" );
        type.hash      = h_walk;
        type.size      = RS_SIZEOF( rs_test_walk_t );
        type.align     = RS_ALIGNOF( rs_test_walk_t );
        type.kind      = RS_KIND_STRUCT;

        rs_field_t fields[ 4 ] = { 0 };

        fields[ 0 ].name_id   = test_intern( "id" );
        fields[ 0 ].name_hash = rs_hash_str( "id" );
        fields[ 0 ].type_hash = h_int32;
        fields[ 0 ].offset    = RS_OFFSETOF( rs_test_walk_t, id );
        fields[ 0 ].size      = RS_FIELD_SIZE( rs_test_walk_t, id );

        fields[ 1 ].name_id   = test_intern( "single" );
        fields[ 1 ].name_hash = rs_hash_str( "single" );
        fields[ 1 ].type_hash = h_vec3;
        fields[ 1 ].offset    = RS_OFFSETOF( rs_test_walk_t, single );
        fields[ 1 ].size      = RS_FIELD_SIZE( rs_test_walk_t, single );
        fields[ 1 ].mods      = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

        fields[ 2 ].name_id   = test_intern( "slots" );
        fields[ 2 ].name_hash = rs_hash_str( "slots" );
        fields[ 2 ].type_hash = h_vec3;
        fields[ 2 ].offset    = RS_OFFSETOF( rs_test_walk_t, slots );
        fields[ 2 ].size      = RS_FIELD_SIZE( rs_test_walk_t, slots );
        fields[ 2 ].mods      = RS_MODS( RS_M_PTR, RS_M_ARRAY, RS_M_END, RS_M_END );
        fields[ 2 ].aux       = 3;

        fields[ 3 ].name_id   = test_intern( "nested" );
        fields[ 3 ].name_hash = rs_hash_str( "nested" );
        fields[ 3 ].type_hash = h_inner;
        fields[ 3 ].offset    = RS_OFFSETOF( rs_test_walk_t, nested );
        fields[ 3 ].size      = RS_FIELD_SIZE( rs_test_walk_t, nested );

        walk_tid = rs_register_type( &type, fields, 4 );
    }

    rs_finalize_frame( game );

    /* Build an instance with distinctive dummy pointers so the visitor output is readable. */
    rs_test_vec3_t v1 = { 1.0f, 0.0f, 0.0f };
    rs_test_vec3_t v2 = { 0.0f, 2.0f, 0.0f };
    rs_test_vec3_t v3 = { 0.0f, 0.0f, 3.0f };
    rs_test_vec3_t v4 = { 9.0f, 9.0f, 9.0f };
    rs_test_vec3_t v5 = { 7.0f, 7.0f, 7.0f };

    rs_test_walk_t inst = { 0 };
    inst.id        = 42;
    inst.single    = &v1;
    inst.slots[0]  = &v2;
    inst.slots[1]  = &v3;
    inst.slots[2]  = NULL;          /* walker still visits null slots */
    inst.nested.p  = &v4;
    (void)v5;

    rs_test_walk_ctx_t ctx = { 0, rs_find_type_by_name( "vec3_t" ) };
    rs_walk_refs( &inst, walk_tid, rs_test_walk_visitor, &ctx );

    printf( "  total visits = %d (expected 5)\n", ctx.count );
    assert( ctx.count == 5 );

    rs_pop_frame( game );
    rs_exit();
}

/*==============================================================================================
    Entry
==============================================================================================*/

void
rs_run_tests( void )
{
    printf( "\n" );
    printf( "========================================\n" );
    printf( "rs: Reflection System Tests\n" );
    printf( "  sizeof(rs_type_t)   = %zu\n",  sizeof( rs_type_t ) );
    printf( "  sizeof(rs_field_t)  = %zu\n",  sizeof( rs_field_t ) );
    printf( "  sizeof(rs_attrib_t) = %zu\n",  sizeof( rs_attrib_t ) );
    printf( "  sizeof(rs_frame_t)  = %zu\n",  sizeof( rs_frame_t ) );
    printf( "========================================\n" );

    test_basic();
    test_mod_decode();
    test_hot_reload_rs();
    test_enums();
    test_bitset();
    test_function_sigs();
    test_walker();
    test_serialize();

    printf( "\nrs: all tests complete.\n" );
}

/*============================================================================================*/
