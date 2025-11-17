/*==============================================================================================

    reflection_test.c

==============================================================================================*/

/* test types */

#define MODULE_GAME 1    // temporary module identifier.

typedef struct vec3_s
{
    float x, y, z;
} vec3_t;

typedef struct transform_s
{
    vec3_t position;
    vec3_t rotation;
    vec3_t scale;

} transform_t;

typedef struct entity_s
{
    int32_t     id;
    char        name[ 64 ];
    transform_t transform;
    float       health;
    vec3_t*     velocity;    // Pointer example

} entity_t;

void rf_test_register_game_module( void );

/*==============================================================================================
    Registration Helpers
==============================================================================================*/

static void
register_vec3_type( uint8_t module_id )
{
    // Precompute hash
    uint32_t hash_vec3  = sid_hash( "vec3_t" );
    uint32_t hash_float = sid_hash( "float" );

    // Define type
    rf_type_t type = { 0 };
    type.name_sid  = sid_intern_cstr( "vec3_t" );
    type.hash      = hash_vec3;
    type.version   = 1;
    type.size      = RF_SIZEOF( vec3_t );
    type.align     = RF_ALIGNOF( vec3_t );
    type.kind      = RF_KIND_STRUCT;
    type.module_id = module_id;
    type.valid     = 1;

    // Define fields
    rf_field_t fields[ 3 ] = { 0 };

    fields[ 0 ].name_sid   = sid_intern_cstr( "x" );
    fields[ 0 ].type_hash  = hash_float;
    fields[ 0 ].type_id    = RF_TYPE_FLOAT;
    fields[ 0 ].offset     = RF_OFFSETOF( vec3_t, x );
    fields[ 0 ].size       = RF_FIELD_SIZE( vec3_t, x );
    fields[ 0 ].kind       = RF_KIND_PRIMITIVE;

    fields[ 1 ].name_sid   = sid_intern_cstr( "y" );
    fields[ 1 ].type_hash  = hash_float;
    fields[ 1 ].type_id    = RF_TYPE_FLOAT;
    fields[ 1 ].offset     = RF_OFFSETOF( vec3_t, y );
    fields[ 1 ].size       = RF_FIELD_SIZE( vec3_t, y );
    fields[ 1 ].kind       = RF_KIND_PRIMITIVE;

    fields[ 2 ].name_sid   = sid_intern_cstr( "z" );
    fields[ 2 ].type_hash  = hash_float;
    fields[ 2 ].type_id    = RF_TYPE_FLOAT;
    fields[ 2 ].offset     = RF_OFFSETOF( vec3_t, z );
    fields[ 2 ].size       = RF_FIELD_SIZE( vec3_t, z );
    fields[ 2 ].kind       = RF_KIND_PRIMITIVE;

    uint16_t tid           = rf_register_type( &type, fields, 3 );

    // Add attributes (if any)
    rf_attrib_t attr = { 0 };
    attr.name_sid    = sid_intern_cstr( "serializable" );
    attr.type        = RF_ATTR_BOOL;
    attr.value.u32   = 1;
    rf_type_add_attribute( tid, &attr );
}

static void
register_transform_type( uint8_t module_id )
{
    uint32_t  hash_transform = sid_hash( "transform_t" );
    uint32_t  hash_vec3      = sid_hash( "vec3_t" );

    rf_type_t type           = { 0 };
    type.name_sid            = sid_intern_cstr( "transform_t" );
    type.hash                = hash_transform;
    type.version             = 1;
    type.size                = RF_SIZEOF( transform_t );
    type.align               = RF_ALIGNOF( transform_t );
    type.kind                = RF_KIND_STRUCT;
    type.module_id           = module_id;
    type.valid               = 1;

    rf_field_t fields[ 3 ]   = { 0 };

    fields[ 0 ].name_sid     = sid_intern_cstr( "position" );
    fields[ 0 ].type_hash    = hash_vec3;
    fields[ 0 ].type_id      = TYPE_INVALID;    // Will be resolved
    fields[ 0 ].offset       = RF_OFFSETOF( transform_t, position );
    fields[ 0 ].size         = RF_FIELD_SIZE( transform_t, position );
    fields[ 0 ].kind         = RF_KIND_STRUCT;

    fields[ 1 ].name_sid     = sid_intern_cstr( "rotation" );
    fields[ 1 ].type_hash    = hash_vec3;
    fields[ 1 ].type_id      = TYPE_INVALID;
    fields[ 1 ].offset       = RF_OFFSETOF( transform_t, rotation );
    fields[ 1 ].size         = RF_FIELD_SIZE( transform_t, rotation );
    fields[ 1 ].kind         = RF_KIND_STRUCT;

    fields[ 2 ].name_sid     = sid_intern_cstr( "scale" );
    fields[ 2 ].type_hash    = hash_vec3;
    fields[ 2 ].type_id      = TYPE_INVALID;
    fields[ 2 ].offset       = RF_OFFSETOF( transform_t, scale );
    fields[ 2 ].size         = RF_FIELD_SIZE( transform_t, scale );
    fields[ 2 ].kind         = RF_KIND_STRUCT;

    uint16_t tid             = rf_register_type( &type, fields, 3 );
    UNUSED( tid );
}

static void
register_entity_type( uint8_t module_id )
{
    uint32_t  hash_entity    = sid_hash( "entity_t" );
    uint32_t  hash_int32     = sid_hash( "int32_t" );
    uint32_t  hash_char      = sid_hash( "char" );
    uint32_t  hash_transform = sid_hash( "transform_t" );
    uint32_t  hash_float     = sid_hash( "float" );
    uint32_t  hash_vec3      = sid_hash( "vec3_t" );

    rf_type_t type           = { 0 };
    type.name_sid            = sid_intern_cstr( "entity_t" );
    type.hash                = hash_entity;
    type.version             = 1;
    type.size                = RF_SIZEOF( entity_t );
    type.align               = RF_ALIGNOF( entity_t );
    type.kind                = RF_KIND_STRUCT;
    type.module_id           = module_id;
    type.valid               = 1;

    rf_field_t fields[ 5 ]   = { 0 };

    // id field
    fields[ 0 ].name_sid  = sid_intern_cstr( "id" );
    fields[ 0 ].type_hash = hash_int32;
    fields[ 0 ].type_id   = RF_TYPE_INT32;
    fields[ 0 ].offset    = RF_OFFSETOF( entity_t, id );
    fields[ 0 ].size      = RF_FIELD_SIZE( entity_t, id );
    fields[ 0 ].kind      = RF_KIND_PRIMITIVE;

    // name field (fixed array)
    fields[ 1 ].name_sid    = sid_intern_cstr( "name" );
    fields[ 1 ].type_hash   = hash_char;
    fields[ 1 ].type_id     = RF_TYPE_CHAR;
    fields[ 1 ].offset      = RF_OFFSETOF( entity_t, name );
    fields[ 1 ].size        = RF_FIELD_SIZE( entity_t, name );
    fields[ 1 ].kind        = RF_KIND_PRIMITIVE | RF_KIND_ARRAY;
    fields[ 1 ].array_count = 64;

    // transform field
    fields[ 2 ].name_sid  = sid_intern_cstr( "transform" );
    fields[ 2 ].type_hash = hash_transform;
    fields[ 2 ].type_id   = TYPE_INVALID;
    fields[ 2 ].offset    = RF_OFFSETOF( entity_t, transform );
    fields[ 2 ].size      = RF_FIELD_SIZE( entity_t, transform );
    fields[ 2 ].kind      = RF_KIND_STRUCT;

    // health field
    fields[ 3 ].name_sid  = sid_intern_cstr( "health" );
    fields[ 3 ].type_hash = hash_float;
    fields[ 3 ].type_id   = RF_TYPE_FLOAT;
    fields[ 3 ].offset    = RF_OFFSETOF( entity_t, health );
    fields[ 3 ].size      = RF_FIELD_SIZE( entity_t, health );
    fields[ 3 ].kind      = RF_KIND_PRIMITIVE;

    // velocity field (pointer)
    fields[ 4 ].name_sid      = sid_intern_cstr( "velocity" );
    fields[ 4 ].type_hash     = hash_vec3;
    fields[ 4 ].type_id       = TYPE_INVALID;
    fields[ 4 ].offset        = RF_OFFSETOF( entity_t, velocity );
    fields[ 4 ].size          = RF_FIELD_SIZE( entity_t, velocity );
    fields[ 4 ].kind          = RF_KIND_STRUCT | RF_KIND_POINTER;
    fields[ 4 ].pointer_depth = 1;

    uint16_t tid              = rf_register_type( &type, fields, 5 );

    // Add field attributes
    const rf_field_t* health_field = rf_get_field_by_name( tid, "health" );
    if ( health_field )
    {
        rf_attrib_t attr = { 0 };
        attr.name_sid    = sid_intern_cstr( "range" );
        attr.type        = RF_ATTR_FLOAT;
        attr.value.f32   = 100.0f;
        rf_field_add_attribute( rf_get_field_id( health_field ), &attr );

        attr.name_sid  = sid_intern_cstr( "min" );
        attr.type      = RF_ATTR_FLOAT;
        attr.value.f32 = 0.0f;
        rf_field_add_attribute( rf_get_field_id( health_field ), &attr );
    }
}

/*============================================================================================*/

static void
print_field_callback( uint16_t field_idx, const rf_field_t* field, void* userdata )
{
    /* Print field info */

    UNUSED( userdata );

    const rf_type_t* field_type = rf_get_type( field->type_id );
    const char*      type_name  = field_type ? sid_cstr( field_type->name_sid ) : "unresolved";

    printf( "Field: %3u %-14s : %-16s", field_idx, sid_cstr( field->name_sid ), type_name );
    printf( " offset: %4u size: %2u kind: 0x%04x", field->offset, field->size, field->kind );

    if ( field->kind & RF_KIND_ARRAY )
    {
        printf( " array[%u]", field->array_count );
    }
    if ( field->kind & RF_KIND_POINTER )
    {
        printf( " ptr%c%c\n", ( field->pointer_depth >= 1 ) ? '*' : 0, ( field->pointer_depth == 2 ) ? '*' : 0 );
    }

    // Print attributes
    if ( field->attr_count > 0 )
    {
        // printf( "\nattributes: " );
        // Note: In real code, would iterate through attributes here
    }
    printf( "\n" );
}

/*==============================================================================================
    Test: Basic Registration
==============================================================================================*/

static void
test_basic_registration( void )
{
    printf( "\n=== Test: Basic Registration ===\n" );

    rf_init();

    uint8_t module_id = rf_module_register( "game", 1 /* version */ );

    register_vec3_type( module_id );
    register_transform_type( module_id );
    register_entity_type( module_id );

    rf_resolve_fields();

    if ( rf_ensure_resolve() )
    {
        printf( "All types resolved successfully!\n" );
    }

    rf_exit();
}

/*==============================================================================================
    Test: Type Lookup
==============================================================================================*/

static void
test_type_lookup( void )
{
    printf( "\n=== Test: Type Lookup ===\n" );

    rf_init();
    uint8_t module_id = rf_module_register( "game", 1 );

    register_vec3_type( module_id );
    register_transform_type( module_id );
    register_entity_type( module_id );
    rf_resolve_fields();

    // Lookup by name
    const rf_type_t* entity = rf_get_type_by_name( "entity_t" );
    if ( entity )
    {
        printf( "Found entity_t: size = %u, fields = %u\n", entity->size, entity->field_count );
    }

    // Lookup by ID
    uint16_t         vec3_id = rf_get_type_id_by_name( "vec3_t" );
    const rf_type_t* vec3    = rf_get_type( vec3_id );
    if ( vec3 )
    {
        printf( "Found vec3_t: ID = %u, size = %u\n", vec3_id, vec3->size );
    }

    rf_exit();
}

/*==============================================================================================
    Test: Field Iteration
==============================================================================================*/

static void
test_field_iteration( void )
{
    printf( "\n=== Test: Field Iteration ===\n" );

    rf_init();
    uint8_t module_id = rf_module_register( "game", 1 );

    register_vec3_type( module_id );
    register_transform_type( module_id );
    register_entity_type( module_id );
    rf_resolve_fields();

    uint16_t vec3_id = rf_get_type_id_by_name( "vec3_t" );
    printf( "\nFields of vec3_t:\n\n" );
    rf_each_field( vec3_id, print_field_callback, NULL );

    uint16_t entity_id = rf_get_type_id_by_name( "entity_t" );
    printf( "\nFields of entity_t:\n\n" );
    rf_each_field( entity_id, print_field_callback, NULL );

    uint16_t transform_id = rf_get_type_id_by_name( "transform_t" );
    printf( "\nFields of transform_t:\n\n" );
    rf_each_field( transform_id, print_field_callback, NULL );

    rf_exit();
}

/*==============================================================================================
    Test: Hot Reload
==============================================================================================*/

static void
test_hot_reload( void )
{
    printf( "\n=== Test: Hot Reload Simulation ===\n" );

    rf_init();
    uint8_t module_id = rf_module_register( "game", 1 );

    // Initial registration
    printf( "\n--- Initial Load ---\n" );
    register_vec3_type( module_id );
    register_entity_type( module_id );
    rf_resolve_fields();

    uint16_t         entity_id = rf_get_type_id_by_name( "entity_t" );
    const rf_type_t* entity    = rf_get_type( entity_id );
    printf( "entity_t version: %u, fields: %u\n", entity->version, entity->field_count );

    // Simulate unload
    printf( "\n--- Module Unload ---\n" );
    rf_module_begin_unload( module_id );
    rf_module_end_unload( module_id );

    // Re-register module
    printf( "\n--- Module Reload ---\n" );
    module_id = rf_module_register( "game", 2 );

    // Re-register types with changes
    register_vec3_type( module_id );

    // Updated entity with new field:
    // Just to demonstrate version increment with changes to structure.

    uint32_t  hash_entity = sid_hash( "entity_t" );
    rf_type_t type        = { 0 };
    type.name_sid         = sid_intern_cstr( "entity_t" );
    type.hash             = hash_entity;
    type.version          = 2;
    type.size             = RF_SIZEOF( entity_t );
    type.align            = RF_ALIGNOF( entity_t );
    type.kind             = RF_KIND_STRUCT;
    type.module_id        = module_id;
    type.valid            = 1;

    // Simplified fields for demo
    rf_field_t fields[ 2 ] = { 0 };
    fields[ 0 ].name_sid   = sid_intern_cstr( "id" );
    fields[ 0 ].type_id    = RF_TYPE_INT32;
    fields[ 0 ].offset     = 0;
    fields[ 0 ].size       = 4;
    fields[ 0 ].kind       = RF_KIND_PRIMITIVE;

    fields[ 1 ].name_sid   = sid_intern_cstr( "health" );
    fields[ 1 ].type_id    = RF_TYPE_FLOAT;
    fields[ 1 ].offset     = 4;
    fields[ 1 ].size       = 4;
    fields[ 1 ].kind       = RF_KIND_PRIMITIVE;

    /* TODO: we need to use register type again with new fields */
    /* TODO: register type should detect existing type and call update */
    /* TODO: version should not be set by reflection declaration but auto-incremented on update */

    entity_id              = rf_update_type( entity_id, &type, fields, 2 );
    entity                 = rf_get_type( entity_id );
    printf( "entity_t version: %u, fields: %u\n", entity->version, entity->field_count );

    rf_exit();
}

/*==============================================================================================
    Test: Transactions
==============================================================================================*/

static void
test_transactions( void )
{
    printf( "\n=== Test: Transaction System ===\n" );

    rf_init();
    uint8_t module_id = rf_module_register( "game", 1 );

    // Successful transaction
    printf( "\n--- Successful Transaction ---\n" );
    rf_begin_transaction();

    register_vec3_type( module_id );
    register_transform_type( module_id );

    if ( rf_commit_transaction() )
    {
        printf( "Transaction committed successfully\n" );
        uint16_t type_count, field_count;
        rf_get_stats( &type_count, &field_count );
        printf( "Types: %u, Fields: %u\n", type_count, field_count );
    }

    // Failed transaction (rollback)
    printf( "\n--- Failed Transaction (Rollback) ---\n" );
    uint16_t checkpoint_types, checkpoint_fields;
    rf_get_stats( &checkpoint_types, &checkpoint_fields );

    rf_begin_transaction();

    // Register some types
    register_entity_type( module_id );

    // Rollback
    rf_rollback_transaction();
    printf( "Transaction rolled back\n" );

    uint16_t after_types, after_fields;
    rf_get_stats( &after_types, &after_fields );
    printf( "Types restored: %u -> %u\n", checkpoint_types, after_types );

    rf_exit();
}

/*==============================================================================================
    Test: Attributes
==============================================================================================*/

static void
test_attributes( void )
{
    printf( "\n=== Test: Attribute System ===\n" );

    rf_init();
    uint8_t module_id = rf_module_register( "game", 1 );

    register_vec3_type( module_id );
    rf_resolve_fields();

    uint16_t vec3_id = rf_get_type_id_by_name( "vec3_t" );

    // Check for serializable attribute
    if ( rf_type_has_attr( vec3_id, "serializable" ) )
    {
        const rf_attrib_t* attr = rf_type_get_attr( vec3_id, "serializable" );
        printf( "vec3_t is serializable: %u\n", attr->value.u32 );
    }

    rf_exit();
}

/*==============================================================================================
    Test: Diagnostics
==============================================================================================*/

static void
test_diagnostics( void )
{
    printf( "\n=== Test: Diagnostics ===\n" );

    rf_init();
    uint8_t module_id = rf_module_register( "game", 1 );

    register_vec3_type( module_id );
    register_transform_type( module_id );
    register_entity_type( module_id );
    rf_resolve_fields();

    rf_print_types();

    printf( "\n--- Detailed Type Info ---\n" );
    uint16_t entity_id = rf_get_type_id_by_name( "entity_t" );
    rf_print_type( entity_id );

    printf( "\n--- Module Info ---\n" );
    rf_print_module( module_id );

    printf( "\n--- Registry Validation ---\n" );
    if ( rf_validate_registry() )
    {
        printf( "Registry is valid\n" );
    }

    rf_exit();
}


/*============================================================================================*/

void
reflection_test( void )
{
    int field_size = sizeof( rf_field_t );
    int type_size  = sizeof( rf_type_t );
    int attr_size  = sizeof( rf_attrib_t );

    UNUSED( field_size );
    UNUSED( type_size );

    printf( "\n" );
    printf( "========================================\n" );
    printf( "Enhanced Reflection System Tests\n" );
    printf( "========================================\n" );

    test_basic_registration();
    test_type_lookup();
    test_field_iteration();
    test_hot_reload();
    test_transactions();
    test_attributes();
    test_diagnostics();

    printf( "\n========================================\n" );
    printf( "All tests completed!\n" );
    printf( "========================================\n\n" );

    /**************************************************************/

    rf_init();
    rf_test_register_game_module();
    rf_ensure_resolve();

    /**************************************************************/

    rf_exit();
}

/*============================================================================================*/

typedef struct player_s    // sample player type
{
    int    id;
    float  health;
    vec3_t pos;

} player_t;

void
test_iterate( uint16_t idx, const rf_field_t* f, void* data )
{
    UNUSED( data );
    const char* fname = sid_cstr( f->name_sid );
    printf( " field[%u] %s offset=%u size=%u type_id=%u\n", ( unsigned )idx, fname, ( unsigned )f->offset,
            ( unsigned )f->size, ( unsigned )f->type_id );
};

void
rf_test_register_game_module( void )
{
    /* Precompute hashes (generator would have emitted these constants) */
    uint32_t hash_player = sid_hash( "player_t" );
    uint32_t hash_float  = sid_hash( "float" );
    uint32_t hash_vec3   = sid_hash( "vec3_t" );

    // --- 1. Define the type_t struct ---
    {
        // This local struct is passed to the registry, which copies and maintains the data.
        rf_type_t type_vec3 = { 0 };
        type_vec3.name_sid  = sid_intern_cstr( "vec3_t" );
        type_vec3.hash      = hash_vec3;
        type_vec3.size      = sizeof( vec3_t );
        type_vec3.module_id = MODULE_GAME;    // Use your module identifier
        type_vec3.valid     = 1;

        // --- 2. Define the field_t array ---

        // The build tool generates this static array of field metadata.
        rf_field_t fields_vec3[ 3 ];
        memset( fields_vec3, 0, sizeof( fields_vec3 ) );

        // Field 'x'
        fields_vec3[ 0 ].name_sid  = sid_intern_cstr( "x" );
        fields_vec3[ 0 ].offset    = offsetof( vec3_t, x );
        fields_vec3[ 0 ].size      = sizeof( float );
        fields_vec3[ 0 ].type_hash = hash_float;    // Hash of the subtype ("float")
        // NOTE: fields_vec3[0].kind is 0, implying a primitive/simple type (for now).

        // Field 'y'
        fields_vec3[ 1 ].name_sid  = sid_intern_cstr( "y" );
        fields_vec3[ 1 ].offset    = offsetof( vec3_t, y );
        fields_vec3[ 1 ].size      = sizeof( float );
        fields_vec3[ 1 ].type_hash = hash_float;

        // Field 'z'
        fields_vec3[ 2 ].name_sid  = sid_intern_cstr( "z" );
        fields_vec3[ 2 ].offset    = offsetof( vec3_t, z );
        fields_vec3[ 2 ].size      = sizeof( float );
        fields_vec3[ 2 ].type_hash = hash_float;

        // --- 3. Register the type and fields together ---

        // Call the single, unified registration function.
        // The function will find a stable ID or create a new one,
        // and update the fields only if the count has changed (3 in this case).
        rf_register_type( &type_vec3, fields_vec3, 3 );
    }
    {
        /* Create field array locally (we must call sid_intern_cstr at runtime) */
        rf_field_t fields[ 2 ];
        memset( fields, 0, sizeof( fields ) );

        fields[ 0 ].name_sid  = sid_intern_cstr( "health" );
        fields[ 0 ].offset    = ( uint32_t )offsetof( player_t, health );
        fields[ 0 ].size      = ( uint32_t )sizeof( float );
        fields[ 0 ].kind      = RF_KIND_PRIMITIVE;
        fields[ 0 ].type_id   = TYPE_INVALID;
        fields[ 0 ].type_hash = hash_float;

        fields[ 1 ].name_sid  = sid_intern_cstr( "pos" );
        fields[ 1 ].offset    = ( uint32_t )offsetof( player_t, pos );
        fields[ 1 ].size      = ( uint32_t )sizeof( vec3_t );
        fields[ 1 ].kind      = 0;
        fields[ 1 ].type_id   = TYPE_INVALID;
        fields[ 1 ].type_hash = hash_vec3;

        /* Prepare type struct (note: we do not set field_index here; reflect_register_type_with_fields does) */
        rf_type_t player_type;
        memset( &player_type, 0, sizeof( player_type ) );
        player_type.name_sid  = sid_intern_cstr( "player_t" );
        player_type.hash      = hash_player;
        player_type.size      = ( uint16_t )sizeof( player_t );
        player_type.module_id = 1;
        player_type.valid     = 1;

        /* Register together (safe) */
        uint16_t tid = rf_register_type( &player_type, fields, 2 );
        ( void )tid;
    }
    printf( "\n" );
    printf( "========================================\n" );
    printf( "Test Module Adds\n" );
    printf( "========================================\n" );
    {
        rf_resolve_fields();

        /* lookup player type id by name */
        u32 pid = rf_get_type_id_by_name( "player_t" );
        printf( "Player type id = %u\n", ( unsigned )pid );

        /* iterate fields */
        rf_each_field( pid, test_iterate, NULL );

        /* lookup vector */
        {
            const rf_type_t* t = rf_get_type_by_name( "vec3_t" );
            printf( "Found type Vec3, %u fields\n", t->field_count );
            for ( uint32_t f = 0; f < t->field_count; ++f )
            {
                const rf_field_t* fld = rf_get_field( t->field_index + f );
                printf( "  Field %s offset=%u subtype_id=%u\n", sid_cstr( fld->name_sid ), fld->offset, fld->type_id );
            }
        }
        /* lookup player */
        {
            const rf_type_t* t = rf_get_type_by_name( "player_t" );
            printf( "Found type Player, %u fields\n", t->field_count );
            for ( uint32_t f = 0; f < t->field_count; ++f )
            {
                const rf_field_t* fld = rf_get_field( t->field_index + f );
                printf( "  Field %s offset=%u subtype_id=%u\n", sid_cstr( fld->name_sid ), fld->offset, fld->type_id );
            }
        }
    }
}

/*============================================================================================*/