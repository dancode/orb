/*==============================================================================================

    Reflection : Diagnostics

==============================================================================================*/

void
rf_print_type( uint16_t type_id )
{
    const rf_type_t* t = rf_get_type( type_id );
    if ( !t || !t->valid )
    {
        printf( "Invalid type ID: %u\n", type_id );
        return;
    }

    printf( "Type: %s\n", sid_cstr( t->name_sid ) );
    printf( "  Size: %u bytes, Align: %u\n", t->size, t->align );
    printf( "  Module: %u, Version: %u\n", t->module_id, t->version );
    printf( "  Fields: %u\n", t->field_count );

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rf_field_t* f  = &g_registry.field_array[ t->field_index + i ];
        const rf_type_t*  ft = rf_get_type( f->type_id );

        printf( "    [%u] %s : %s (offset=%u, size=%u)\n", i, sid_cstr( f->name_sid ),
                ft ? sid_cstr( ft->name_sid ) : "unresolved", f->offset, f->size );
    }
}

/*============================================================================================*/

void
rf_print_types( void )
{
    printf( "=== Reflection Registry ===\n" );
    printf( "Types: %u/%u\n", g_registry.type_count, MAX_TYPES );
    printf( "Fields: %u/%u\n", g_registry.field_count, MAX_FIELDS );
    printf( "Attributes: %u/%u\n", g_registry.attr_count, MAX_ATTRIBUTES );
    printf( "\nTypes:\n" );

    for ( uint16_t i = 0; i < g_registry.type_count; i++ )
    {
        const rf_type_t* t = &g_registry.type_array[ i ];
        if ( !t->valid )
            continue;

        printf( "  [%u] %s (size=%u, fields=%u, module=%u, v%u)\n", i, sid_cstr( t->name_sid ), t->size,
                t->field_count, t->module_id, t->version );
    }
}

/*============================================================================================*/

void
rf_print_module( uint8_t module_id )
{
    const rf_module_t* mod = rf_module_get( module_id );
    if ( !mod )
    {
        printf( "Invalid module ID: %u\n", module_id );
        return;
    }

    printf( "Module: %s\n", sid_cstr( mod->name_sid ) );
    printf( "  Version: %u\n", mod->version );
    printf( "  Load Count: %u\n", mod->load_count );
    printf( "  State: %u\n", mod->state );
    printf( "  Types: %u\n", mod->type_count );
}

/*============================================================================================*/