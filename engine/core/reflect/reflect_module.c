/*==============================================================================================

    Reflection : Module Management

==============================================================================================*/

uint8_t
rf_module_register( const char* name, uint32_t version )
{
    if ( !name || g_registry.module_count >= MAX_MODULES )
    {
        assert( 0 && "Module registry is full" );
        return MODULE_INVALID;
    }

    // Check if module already exists
    sid_t name_sid = sid_intern_cstr( name );
    for ( uint8_t i = 0; i < g_registry.module_count; i++ )
    {
        if ( sid_equals( g_registry.module_array[ i ].name_sid, name_sid ) )
        {
            // Update version and mark as loading
            g_registry.module_array[ i ].version = version;
            g_registry.module_array[ i ].load_count++;
            g_registry.module_array[ i ].state = RF_MODULE_LOADING;
            return i;
        }
    }

    // Create new module
    uint8_t      module_id = g_registry.module_count++;
    rf_module_t* mod       = &g_registry.module_array[ module_id ];

    mod->name_sid          = name_sid;
    mod->version           = version;
    mod->load_count        = 1;
    mod->type_count        = 0;
    mod->first_type_id     = TYPE_INVALID;
    mod->state             = RF_MODULE_LOADING;
    mod->dll_handle        = NULL;

    if ( rf_debug )
    {
        printf( "Registered Module: [ %s ] id: %u version: %u\n", sid_cstr( mod->name_sid ), module_id, version );
    }

    return module_id;
}

/*============================================================================================*/

void
rf_module_begin_unload( uint8_t module_id )
{
    /* Mark module as unloading and invalidate its types */

    if ( module_id >= g_registry.module_count )
        return;

    rf_module_t* mod = &g_registry.module_array[ module_id ];
    mod->state       = RF_MODULE_UNLOADING;

    // Mark all types from this module as invalid
    for ( uint16_t i = 0; i < g_registry.type_count; i++ )
    {
        rf_type_t* t = &g_registry.type_array[ i ];
        if ( t->module_id == module_id && t->valid )
        {
            t->valid      = 0;
        }
    }
}

/*============================================================================================*/

void
rf_module_end_unload( uint8_t module_id )
{
    if ( module_id >= g_registry.module_count )
        return;

    rf_module_t* mod   = &g_registry.module_array[ module_id ];
    mod->state         = RF_MODULE_UNLOADED;
    mod->type_count    = 0;
    mod->first_type_id = TYPE_INVALID;

    // Note: We keep deprecated types for hot-reload matching
    // They can be revalidated when module reloads
}

/*============================================================================================*/

const rf_module_t*
rf_module_get( uint8_t module_id )
{
    if ( module_id >= g_registry.module_count )
        return NULL;

    return &g_registry.module_array[ module_id ];
}

/*============================================================================================*/