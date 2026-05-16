/*==============================================================================================

    core/rs/rs_test.c - Exercise the rs_ reflection system.

==============================================================================================*/

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
    const uint32_t h_vec3  = sid_hash( "vec3_t" );
    const uint32_t h_float = sid_hash( "float" );

    rs_type_t type = { 0 };
    type.name_sid  = sid_intern_cstr( "vec3_t" );
    type.hash      = h_vec3;
    type.size      = RS_SIZEOF( rs_test_vec3_t );
    type.align     = RS_ALIGNOF( rs_test_vec3_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 3 ] = { 0 };
    uint32_t   hashes[ 3 ] = { h_float, h_float, h_float };

    fields[ 0 ].name_sid = sid_intern_cstr( "x" );
    fields[ 0 ].offset   = RS_OFFSETOF( rs_test_vec3_t, x );
    fields[ 0 ].size     = RS_FIELD_SIZE( rs_test_vec3_t, x );
    fields[ 0 ].mods     = RS_NO_MODS;

    fields[ 1 ].name_sid = sid_intern_cstr( "y" );
    fields[ 1 ].offset   = RS_OFFSETOF( rs_test_vec3_t, y );
    fields[ 1 ].size     = RS_FIELD_SIZE( rs_test_vec3_t, y );

    fields[ 2 ].name_sid = sid_intern_cstr( "z" );
    fields[ 2 ].offset   = RS_OFFSETOF( rs_test_vec3_t, z );
    fields[ 2 ].size     = RS_FIELD_SIZE( rs_test_vec3_t, z );

    uint16_t tid = rs_register_type( &type, fields, hashes, 3 );

    rs_attrib_t a = { 0 };
    a.name_sid    = sid_intern_cstr( "serializable" );
    a.type        = RS_ATTR_BOOL;
    a.value.u32   = 1;
    rs_type_add_attr( tid, &a );
}

static void
rs_test_register_transform( void )
{
    const uint32_t h_xform = sid_hash( "transform_t" );
    const uint32_t h_vec3  = sid_hash( "vec3_t" );

    rs_type_t type = { 0 };
    type.name_sid  = sid_intern_cstr( "transform_t" );
    type.hash      = h_xform;
    type.size      = RS_SIZEOF( rs_test_transform_t );
    type.align     = RS_ALIGNOF( rs_test_transform_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 3 ] = { 0 };
    uint32_t   hashes[ 3 ] = { h_vec3, h_vec3, h_vec3 };

    fields[ 0 ].name_sid = sid_intern_cstr( "position" );
    fields[ 0 ].offset   = RS_OFFSETOF( rs_test_transform_t, position );
    fields[ 0 ].size     = RS_FIELD_SIZE( rs_test_transform_t, position );

    fields[ 1 ].name_sid = sid_intern_cstr( "rotation" );
    fields[ 1 ].offset   = RS_OFFSETOF( rs_test_transform_t, rotation );
    fields[ 1 ].size     = RS_FIELD_SIZE( rs_test_transform_t, rotation );

    fields[ 2 ].name_sid = sid_intern_cstr( "scale" );
    fields[ 2 ].offset   = RS_OFFSETOF( rs_test_transform_t, scale );
    fields[ 2 ].size     = RS_FIELD_SIZE( rs_test_transform_t, scale );

    rs_register_type( &type, fields, hashes, 3 );
}

static void
rs_test_register_entity( void )
{
    const uint32_t h_entity = sid_hash( "entity_t" );
    const uint32_t h_int32  = sid_hash( "int32_t" );
    const uint32_t h_char   = sid_hash( "char" );
    const uint32_t h_xform  = sid_hash( "transform_t" );
    const uint32_t h_float  = sid_hash( "float" );
    const uint32_t h_vec3   = sid_hash( "vec3_t" );

    rs_type_t type = { 0 };
    type.name_sid  = sid_intern_cstr( "entity_t" );
    type.hash      = h_entity;
    type.size      = RS_SIZEOF( rs_test_entity_t );
    type.align     = RS_ALIGNOF( rs_test_entity_t );
    type.kind      = RS_KIND_STRUCT;

    rs_field_t fields[ 7 ] = { 0 };
    uint32_t   hashes[ 7 ] = { h_int32, h_char, h_xform, h_float, h_vec3, h_char, h_vec3 };

    /* id : int32_t                                  */
    fields[ 0 ].name_sid = sid_intern_cstr( "id" );
    fields[ 0 ].offset   = RS_OFFSETOF( rs_test_entity_t, id );
    fields[ 0 ].size     = RS_FIELD_SIZE( rs_test_entity_t, id );

    /* name : char[64]    base=char, mods=[ARRAY], aux=64 */
    fields[ 1 ].name_sid = sid_intern_cstr( "name" );
    fields[ 1 ].offset   = RS_OFFSETOF( rs_test_entity_t, name );
    fields[ 1 ].size     = RS_FIELD_SIZE( rs_test_entity_t, name );
    fields[ 1 ].mods     = RS_MODS( RS_M_ARRAY, RS_M_END, RS_M_END, RS_M_END );
    fields[ 1 ].aux      = 64;

    /* transform : transform_t                       */
    fields[ 2 ].name_sid = sid_intern_cstr( "transform" );
    fields[ 2 ].offset   = RS_OFFSETOF( rs_test_entity_t, transform );
    fields[ 2 ].size     = RS_FIELD_SIZE( rs_test_entity_t, transform );

    /* health : float                                */
    fields[ 3 ].name_sid = sid_intern_cstr( "health" );
    fields[ 3 ].offset   = RS_OFFSETOF( rs_test_entity_t, health );
    fields[ 3 ].size     = RS_FIELD_SIZE( rs_test_entity_t, health );

    /* velocity : vec3_t*  base=vec3, mods=[PTR]     */
    fields[ 4 ].name_sid = sid_intern_cstr( "velocity" );
    fields[ 4 ].offset   = RS_OFFSETOF( rs_test_entity_t, velocity );
    fields[ 4 ].size     = RS_FIELD_SIZE( rs_test_entity_t, velocity );
    fields[ 4 ].mods     = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    /* label : const char*  base_const=1, mods=[PTR] */
    fields[ 5 ].name_sid   = sid_intern_cstr( "label" );
    fields[ 5 ].offset     = RS_OFFSETOF( rs_test_entity_t, label );
    fields[ 5 ].size       = RS_FIELD_SIZE( rs_test_entity_t, label );
    fields[ 5 ].base_const = 1;
    fields[ 5 ].mods       = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    /* slots : vec3_t*[8]   mods=[PTR, ARRAY], aux=8 */
    fields[ 6 ].name_sid = sid_intern_cstr( "slots" );
    fields[ 6 ].offset   = RS_OFFSETOF( rs_test_entity_t, slots );
    fields[ 6 ].size     = RS_FIELD_SIZE( rs_test_entity_t, slots );
    fields[ 6 ].mods     = RS_MODS( RS_M_PTR, RS_M_ARRAY, RS_M_END, RS_M_END );
    fields[ 6 ].aux      = 8;

    uint16_t tid = rs_register_type( &type, fields, hashes, 7 );

    /* Attach two attributes to the 'health' field to exercise the repeated-entry path. */
    const rs_field_t* health = rs_find_field( tid, "health" );
    if ( health )
    {
        uint16_t fid  = (uint16_t)( health - &g_rs.fields[ 0 ] );
        rs_attrib_t a = { 0 };
        a.name_sid    = sid_intern_cstr( "range" );
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
    uint8_t game = rs_push_frame( "game", 1 );

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

    uint8_t game = rs_push_frame( "game", 1 );
    rs_test_register_vec3();
    rs_test_register_transform();
    rs_test_register_entity();
    rs_finalize_frame( game );

    uint16_t t_count, f_count;
    uint8_t  fr_count;
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
    uint8_t game = rs_push_frame( "game", 1 );

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
    uint8_t game = rs_push_frame( "game", 1 );

    rs_type_t type = { 0 };
    type.name_sid  = sid_intern_cstr( "color_t" );
    type.hash      = sid_hash( "color_t" );
    type.size      = (uint16_t)sizeof( rs_test_color_t );
    type.align     = (uint8_t)_Alignof( rs_test_color_t );
    /* kind is forced to RS_KIND_ENUM by rs_register_enum */

    rs_enumerator_t entries[ 4 ] = {
        { sid_intern_cstr( "RED" ),   RS_TEST_COLOR_RED   },
        { sid_intern_cstr( "GREEN" ), RS_TEST_COLOR_GREEN },
        { sid_intern_cstr( "BLUE" ),  RS_TEST_COLOR_BLUE  },
        { sid_intern_cstr( "ALPHA" ), RS_TEST_COLOR_ALPHA },
    };

    uint16_t tid = rs_register_enum( &type, entries, 4 );
    rs_finalize_frame( game );

    rs_print_type( tid );

    /* Lookup by name */
    const rs_enumerator_t* by_name = rs_enum_find_by_name( tid, "BLUE" );
    printf( "  find_by_name(\"BLUE\")  -> value=%lld\n",
            by_name ? (long long)by_name->value : -1 );

    /* Lookup by value */
    const rs_enumerator_t* by_value = rs_enum_find_by_value( tid, 0xFF );
    printf( "  find_by_value(0xFF)   -> name=%s\n",
            by_value ? sid_cstr( by_value->name_sid ) : "<none>" );

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
    uint8_t game = rs_push_frame( "game", 1 );

    /* vec3_t is needed by the signature's second parameter. */
    rs_test_register_vec3();

    /* --- Register the signature type "on_die_fn". --- */
    const uint32_t h_sig   = sid_hash( "on_die_fn" );
    const uint32_t h_void  = sid_hash( "void" );
    const uint32_t h_int32 = sid_hash( "int32_t" );
    const uint32_t h_vec3  = sid_hash( "vec3_t" );

    rs_type_t sig_type = { 0 };
    sig_type.name_sid  = sid_intern_cstr( "on_die_fn" );
    sig_type.hash      = h_sig;
    sig_type.size      = (uint16_t)sizeof( void* );    /* a pointer holds the callable */
    sig_type.align     = (uint8_t)_Alignof( void* );

    /* index 0 = return, indices 1..N = params */
    rs_field_t sig_fields[ 3 ] = { 0 };
    uint32_t   sig_hashes[ 3 ] = { h_void, h_int32, h_vec3 };

    sig_fields[ 0 ].name_sid = sid_intern_cstr( "return" );
    /* return: void  -> no mods, no aux */

    sig_fields[ 1 ].name_sid = sid_intern_cstr( "reason" );
    sig_fields[ 1 ].size     = (uint16_t)sizeof( int32_t );
    /* int32_t value */

    sig_fields[ 2 ].name_sid = sid_intern_cstr( "loc" );
    sig_fields[ 2 ].size     = (uint16_t)sizeof( rs_test_vec3_t* );
    sig_fields[ 2 ].mods     = RS_MODS( RS_M_PTR, RS_M_END, RS_M_END, RS_M_END );

    uint16_t sig_id = rs_register_function( &sig_type, sig_fields, sig_hashes, 3 );

    /* --- Register a struct that contains an on_die callback field. --- */
    const uint32_t h_npc = sid_hash( "npc_t" );

    rs_type_t npc_type = { 0 };
    npc_type.name_sid  = sid_intern_cstr( "npc_t" );
    npc_type.hash      = h_npc;
    npc_type.size      = RS_SIZEOF( rs_test_npc_t );
    npc_type.align     = RS_ALIGNOF( rs_test_npc_t );
    npc_type.kind      = RS_KIND_STRUCT;

    rs_field_t npc_fields[ 2 ] = { 0 };
    uint32_t   npc_hashes[ 2 ] = { sid_hash( "float" ), h_void };

    npc_fields[ 0 ].name_sid = sid_intern_cstr( "health" );
    npc_fields[ 0 ].offset   = RS_OFFSETOF( rs_test_npc_t, health );
    npc_fields[ 0 ].size     = RS_FIELD_SIZE( rs_test_npc_t, health );

    /* on_die: void(*)(int32_t, vec3_t*)
         base = void, mods=[FUNCTION], aux=sig_id  */
    npc_fields[ 1 ].name_sid = sid_intern_cstr( "on_die" );
    npc_fields[ 1 ].offset   = RS_OFFSETOF( rs_test_npc_t, on_die );
    npc_fields[ 1 ].size     = RS_FIELD_SIZE( rs_test_npc_t, on_die );
    npc_fields[ 1 ].mods     = RS_MODS( RS_M_FUNCTION, RS_M_END, RS_M_END, RS_M_END );
    npc_fields[ 1 ].aux      = sig_id;

    uint16_t npc_id = rs_register_type( &npc_type, npc_fields, npc_hashes, 2 );

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
            p1 ? sid_cstr( p1->name_sid ) : "?", p1 ? p1->type_id : 0xFFFF );

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
    test_function_sigs();

    printf( "\nrs: all tests complete.\n" );
}

/*============================================================================================*/
