/*==============================================================================================

    core/reflect/reflect_registry.c
    
    Holds all registered types, fields, and attributes in fixed-size arrays. 
    Provides functions for initialization, shutdown, and internal management of the registry data.

==============================================================================================*/

static registry_t g_registry;         // global registry data (single instance)
static const bool rf_debug = true;    // enable debug output

/*==============================================================================================

    Reflection : Initialization

==============================================================================================*/

static void
rf_init_builtin_types( void )
{
    const char*    builtin_names[] = { "invalid", "void",     "bool",     "char",    "int8_t",
                                       "uint8_t", "int16_t",  "uint16_t", "int32_t", "uint32_t",
                                       "int64_t", "uint64_t", "float",    "double",  "string" };
    const uint16_t builtin_sizes[] = { 0, 0, 1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, sizeof( char* ) };
    const uint8_t  builtin_align[] = { 0, 0, 1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, sizeof( char* ) };

    for ( uint16_t i = 0; i < RF_TYPE_BUILT_IN_COUNT; ++i )
    {
        rf_type_t t                = { 0 };
        t.name_sid                 = sid_intern_cstr( builtin_names[ i ] );
        t.hash                     = sid_hash( builtin_names[ i ] );
        t.field_index              = 0;
        t.field_count              = 0;
        t.attr_index               = ATTR_INVALID;
        t.attr_count               = 0;
        t.next                     = TYPE_INVALID;
        t.kind                     = RF_KIND_PRIMITIVE;
        t.size                     = builtin_sizes[ i ];    // set size
        t.align                    = builtin_align[ i ];    // set alignment
        t.module_id                = 0;                     // engine/system module .exe
        t.valid                    = 1;
        t.version                  = 1;

        g_registry.type_array[ i ] = t;

        // Add to hash table
        uint32_t slot                   = t.hash & TYPE_HASH_MASK;
        uint16_t old_head               = g_registry.type_hash[ slot ];
        g_registry.type_array[ i ].next = old_head;
        g_registry.type_hash[ slot ]    = i;

        if ( rf_debug )
        {
            printf( "Registered Module: %2u ID: %3u %s\n", 0, i, sid_cstr( t.name_sid ) );
        }
    }

    g_registry.type_count = RF_TYPE_BUILT_IN_COUNT;
}

/*============================================================================================*/

void
rf_init( void )
{
    sid_init();
    memset( &g_registry, 0, sizeof( g_registry ) );

    // Initialize hash table to invalid.
    for ( int i = 0; i < TYPE_HASH_SIZE; i++ ) g_registry.type_hash[ i ] = TYPE_INVALID;

    rf_module_register( "system", 1 /* version */ );
    rf_init_builtin_types();
}

/*============================================================================================*/

void
rf_exit( void )
{
    // Cleanup all modules
    for ( uint8_t i = 0; i < g_registry.module_count; i++ )
    {
        rf_module_end_unload( i );
    }

    memset( &g_registry, 0, sizeof( g_registry ) );
    sid_exit();
}

/*============================================================================================*/

void
rf_get_stats( uint16_t* type_count, uint16_t* field_count )
{
    if ( type_count )
        *type_count = g_registry.type_count;

    if ( field_count )
        *field_count = g_registry.field_count;
}

/*==============================================================================================
    Internal : Field + Attribute Management
==============================================================================================*/

static uint16_t
rf_push_fields( const rf_field_t* fields, uint16_t count )
{
    /* Append fields to the field array (copy from static table) */

    if ( !fields || count == 0 )
    {
        assert( 0 && "Invalid field registration! expected fields" );
        return 0;    // no fields is valid
    }
    if ( g_registry.field_count + count > MAX_FIELDS )
    {
        assert( 0 && "Field registry full!" );
        return TYPE_INVALID;
    }

    /* copy memory in one go */
    uint16_t start = g_registry.field_count;
    memcpy( &g_registry.field_array[ start ], fields, ( size_t )count * sizeof( rf_field_t ) );
    g_registry.field_count += count;

    return start;    // first field index
}

static uint16_t
rf_push_attributes( const rf_attrib_t* attrs, uint16_t count )
{
    /* Append attributes to attribute array (copy from static table) */

    if ( !attrs || count == 0 )
    {
        assert( 0 && "Invalid attribute registration! expected attributes" );
        return ATTR_INVALID;
    }
    if ( g_registry.attr_count + count > MAX_ATTRIBUTES )
    {
        assert( 0 && "Attribute registry full!" );
        return ATTR_INVALID;
    }

    uint16_t start = g_registry.attr_count;
    memcpy( &g_registry.attrib_array[ start ], attrs, count * sizeof( rf_attrib_t ) );
    g_registry.attr_count += count;

    return start;
}

/*============================================================================================*/
/* Add attribute to type (appended to end of current attributes) */
/* An attribute is just a name and value pair that can be used to annotate types and fields
   with extra information for tools, serialization, etc. */

bool
rf_type_add_attribute( uint16_t type_id, const rf_attrib_t* attr )
{
    /* incorrect parameter */
    if ( type_id >= g_registry.type_count || !attr )
    {
        assert( 0 && "Invalid type attribute registration!" );
        return false;
    }

    rf_type_t* type = &g_registry.type_array[ type_id ];

    if ( type->attr_count == 0 )
    {
        type->attr_index = rf_push_attributes( attr, 1 );
        if ( type->attr_index == ATTR_INVALID )
            return false;
        type->attr_count = 1;
        return true;
    }

    /* Contiguity check: this type's block must end at the current tail.
       All attributes for a type must be added before registering attributes on any other type or field. */
    assert( ( uint16_t )( type->attr_index + type->attr_count ) == g_registry.attr_count &&
            "rf_type_add_attribute: attributes must be registered contiguously" );

    uint16_t new_idx = rf_push_attributes( attr, 1 );
    if ( new_idx == ATTR_INVALID )
        return false;

    type->attr_count++;
    return true;
}

/*============================================================================================*/

bool
rf_field_add_attribute( uint16_t field_id, const rf_attrib_t* attr )
{
    if ( field_id >= g_registry.field_count || !attr )
        return false;

    rf_field_t* field = &g_registry.field_array[ field_id ];

    if ( field->attr_count == 0 )
    {
        field->attr_index = rf_push_attributes( attr, 1 );
        if ( field->attr_index == ATTR_INVALID )
            return false;
        field->attr_count = 1;
        return true;
    }

    /* Contiguity check: this field's block must end at the current tail.
       All attributes for a field must be added before registering attributes on any other type or field. */
    assert( ( uint16_t )( field->attr_index + field->attr_count ) == g_registry.attr_count &&
            "rf_field_add_attribute: attributes must be registered contiguously" );

    uint16_t new_idx = rf_push_attributes( attr, 1 );
    if ( new_idx == ATTR_INVALID )
        return false;

    field->attr_count++;
    return true;
}

/*==============================================================================================
    Internal : Type Slot Management
==============================================================================================*/

static uint16_t
rf_find_type_slot( uint32_t hash )
{
    uint32_t slot = hash & TYPE_HASH_MASK;
    uint16_t idx  = g_registry.type_hash[ slot ];

    while ( idx != TYPE_INVALID )
    {
        rf_type_t* t = &g_registry.type_array[ idx ];
        if ( t->hash == hash )
        {
            return idx;
        }
        idx = t->next;
    }
    return TYPE_INVALID;
}

static uint16_t
rf_create_type_slot( const rf_type_t* src_type )
{
    if ( g_registry.type_count >= MAX_TYPES )
    {
        assert( 0 && "Type registry full" );
        return TYPE_INVALID;
    }

    uint16_t   id         = g_registry.type_count++;
    rf_type_t* new_type   = &g_registry.type_array[ id ];
    *new_type             = *src_type;

    new_type->field_index = 0;
    new_type->field_count = 0;
    new_type->next        = TYPE_INVALID;

    // Insert into hash table
    uint32_t slot                = src_type->hash & TYPE_HASH_MASK;
    new_type->next               = g_registry.type_hash[ slot ];
    g_registry.type_hash[ slot ] = id;

    return id;
}


/*==============================================================================================
    Reflection : Type Registration
==============================================================================================*/

uint16_t
rf_register_type( rf_type_t* type, const rf_field_t* fields, uint16_t field_count )
{
    /* Convenience function to register type with fields (returns id)
       Ensures field index and count are set correctly. */

    if ( !type )
        return TYPE_INVALID;

    // Find or create slot
    uint16_t type_id  = rf_find_type_slot( type->hash );
    bool     is_new   = ( type_id == TYPE_INVALID );

    if ( is_new )
    {
        type_id = rf_create_type_slot( type );
        if ( type_id == TYPE_INVALID )
            return TYPE_INVALID;
    }

    rf_type_t* target = &g_registry.type_array[ type_id ];

    // Update type data
    target->name_sid  = type->name_sid;
    target->hash      = type->hash;

    target->kind      = type->kind;
    target->size      = type->size;
    target->align     = type->align;

    target->module_id = type->module_id;
    target->valid     = 1;
    // First registration uses the caller's version; hot-reload bumps it automatically.
    if ( is_new )
    {
        target->version = type->version;
    }
    else
    {
        target->version++;
        // Reset attribute block so re-registration appends fresh at the current tail.
        // Old slots are orphaned in the array (acceptable; fixed-size design does not compact).
        target->attr_index = ATTR_INVALID;
        target->attr_count = 0;
    }

    // Update fields
    if ( target->field_count != field_count )
    {
        // Count changed: push a new block; old block is orphaned.
        uint16_t field_idx = 0;
        if ( field_count > 0 )
        {
            field_idx = rf_push_fields( fields, field_count );
            if ( field_idx == FIELD_INVALID )
            {
                target->valid = 0;
                return TYPE_INVALID;
            }
        }
        target->field_index = field_idx;
        target->field_count = field_count;
    }
    else if ( !is_new && field_count > 0 )
    {
        // Same count on reload: check each field for content changes and update in place.
        // type_id is reset to TYPE_INVALID on any changed field so rf_resolve_fields() re-links it.
        rf_field_t* existing = &g_registry.field_array[ target->field_index ];
        for ( uint16_t i = 0; i < field_count; i++ )
        {
            bool name_changed = !sid_equals( existing[ i ].name_sid, fields[ i ].name_sid );
            bool type_changed = existing[ i ].type_hash != fields[ i ].type_hash;
            if ( name_changed || type_changed )
            {
                printf( "WARNING: %s field[%u] changed on reload:", sid_cstr( target->name_sid ), i );
                if ( name_changed )
                    printf( " name '%s'->'%s'", sid_cstr( existing[ i ].name_sid ),
                            sid_cstr( fields[ i ].name_sid ) );
                if ( type_changed )
                    printf( " type_hash 0x%08x->0x%08x", existing[ i ].type_hash, fields[ i ].type_hash );
                printf( "\n" );
                existing[ i ]         = fields[ i ];
                existing[ i ].type_id = TYPE_INVALID;    // force re-resolve
            }
        }
    }

    // Update module tracking.
    if ( type->module_id < g_registry.module_count )
    {
        // We add all module types in a linear block.
        // so first type added to module is our likely first index.
        rf_module_t* mod = &g_registry.module_array[ type->module_id ];
        if ( mod->first_type_id == TYPE_INVALID )
        {
            mod->first_type_id = type_id;
        }
        mod->type_count++;
        mod->state = RF_MODULE_LOADED;
    }

    if ( rf_debug )
    {
        printf( "Registered Module: %2u ID: %3u %s\n", type->module_id, type_id, sid_cstr( target->name_sid ) );
    }

    return type_id;
}

/*============================================================================================*/

void
rf_resolve_fields( void )
{
    /* resolve all field subtypes (safe to call multiple times) */
    
    printf( "\n" );
    for ( uint16_t i = 0; i < g_registry.field_count; i++ )
    {
        /* only attempt resolve if there is a hash and it's currently unresolved */
        rf_field_t* f = &g_registry.field_array[ i ];
        if ( f->type_id == TYPE_INVALID && f->type_hash != 0 )
        {
            // resolve hash to existing type id.
            // Will still be TYPE_INVALID if not registered yet
            f->type_id = rf_get_type_id_by_hash( f->type_hash );

            if ( rf_debug )
            {
                if ( f->type_id == TYPE_INVALID )
                {
                    printf( "Resolved field: %3u %16s = ????????\n", i, sid_cstr( f->name_sid ) );
                }
                else
                {
                    const rf_type_t* t = rf_get_type( f->type_id );
                    printf( "Resolved field: %3u %16s = %s\n", i, sid_cstr( f->name_sid ), 
                            sid_cstr( t->name_sid ) );
                }
            }
        }
    }
    printf( "\n" );
}

/*============================================================================================*/

bool
rf_validate_fields( void )
{
    /* Ensure all field types are resolved -- Called after all modules are loaded to verify. */
    /* success means all fields are resolved and reflection system cannot fail */

    bool failed = false;
    for ( uint16_t i = 0; i < g_registry.field_count; i++ )
    {
        rf_field_t* f = &g_registry.field_array[ i ];
        if ( f->type_id == TYPE_INVALID && f->type_hash != 0 )
        {
            printf( "ERROR: Unresolved field '%s' with type_hash 0x%08x\n", sid_cstr( f->name_sid ), f->type_hash );
            failed = true;
        }
    }
    if ( failed )
    {
        // ERROR: this should not happen
        printf( "Field resolution failed!\n" );
        return false;
    }
    return true;
}

/*============================================================================================*/

bool
rf_validate_types( void )
{
    /* this is the same as validate fields, but reports per-type */
    /* validate all types to ensure all fields declared in type are resolved */
    /* success means the type can be used */

    bool all_valid = true;
    for ( uint16_t i = 0; i < g_registry.type_count; i++ )
    {
        rf_type_t* t = &g_registry.type_array[ i ];
        if ( !t->valid )
            continue;

        /* validate fields */
        for ( uint16_t f = 0; f < t->field_count; f++ )
        {
            const rf_field_t* field = &g_registry.field_array[ t->field_index + f ];

            if ( field->type_id == TYPE_INVALID && field->type_hash != 0 )
            {
                printf( "Warning: Type '%s' has unresolved field '%s'\n", sid_cstr( t->name_sid ),
                        sid_cstr( field->name_sid ) );

                all_valid = false;
            }
        }
    }
    return all_valid;
}

/*============================================================================================*/

bool
rf_validate_registry( void )
{
    /* Validate the entire registry:
       1. Every valid type must be reachable by its own hash.
       2. No stale invalid entry may sit ahead of a valid entry with the same hash
          in a chain (would have shadowed lookups before the rf_get_type_id_by_hash fix,
          now reported as a structural warning). */

    bool valid = true;

    /* Pass 1: every valid type must round-trip through the hash table */
    for ( uint16_t i = 0; i < g_registry.type_count; i++ )
    {
        const rf_type_t* t = &g_registry.type_array[ i ];
        if ( !t->valid )
            continue;

        uint16_t found = rf_get_type_id_by_hash( t->hash );
        if ( found != i )
        {
            printf( "ERROR: Type '%s' hash lookup returned %u, expected %u\n",
                    sid_cstr( t->name_sid ), found, i );
            valid = false;
        }
    }

    /* Pass 2: walk every hash chain and flag stale invalid entries that precede
       a valid entry with the same full hash (structural shadowing). */
    for ( int slot = 0; slot < TYPE_HASH_SIZE; slot++ )
    {
        uint16_t idx = g_registry.type_hash[ slot ];
        while ( idx != TYPE_INVALID )
        {
            const rf_type_t* t = &g_registry.type_array[ idx ];
            if ( !t->valid )
            {
                /* Check whether a valid entry with the same hash follows in this chain */
                uint16_t next = t->next;
                while ( next != TYPE_INVALID )
                {
                    const rf_type_t* n = &g_registry.type_array[ next ];
                    if ( n->hash == t->hash && n->valid )
                    {
                        printf( "WARNING: Stale invalid entry '%s'[%u] precedes valid '%s'[%u] in hash chain\n",
                                sid_cstr( t->name_sid ), idx, sid_cstr( n->name_sid ), next );
                        valid = false;
                        break;
                    }
                    next = n->next;
                }
            }
            idx = t->next;
        }
    }

    return valid;
}

/*============================================================================================*/