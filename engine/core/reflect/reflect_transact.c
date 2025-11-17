/*==============================================================================================

    Reflection : Transational Support

==============================================================================================*/

void
rf_begin_transaction( void )
{
    g_registry.transaction_active     = 1;
    g_registry.transaction_checkpoint = g_registry.type_count;
}

/*============================================================================================*/

bool
rf_commit_transaction( void )
{
    if ( !g_registry.transaction_active )
    {
        return false;
    }
    
    // Resolve all new types added in transaction
    rf_resolve_fields();
    
    // Validate
    bool valid                        = rf_validate_types();
    
    g_registry.transaction_active     = 0;
    g_registry.transaction_checkpoint = 0;

    return valid;
}

/*============================================================================================*/

void
rf_rollback_transaction( void )
{
    if ( !g_registry.transaction_active )
    {
        return;
    }

    // Remove types added during transaction
    uint16_t checkpoint = g_registry.transaction_checkpoint;
    for ( uint16_t i = checkpoint; i < g_registry.type_count; i++ )
    {
        // Remove from hash table
        rf_type_t* t    = &g_registry.type_array[ i ];
        uint32_t   slot = t->hash & TYPE_HASH_MASK;
    
        // Find and unlink from chain
        if ( g_registry.type_hash[ slot ] == i )
        {
            g_registry.type_hash[ slot ] = t->next;
        }
        else
        {
            uint16_t prev = g_registry.type_hash[ slot ];
            while ( prev != TYPE_INVALID )
            {
                if ( g_registry.type_array[ prev ].next == i )
                {
                    g_registry.type_array[ prev ].next = t->next;
                    break;
                }
                prev = g_registry.type_array[ prev ].next;
            }
        }
    }
    
    g_registry.type_count             = checkpoint;
    g_registry.transaction_active     = 0;
    g_registry.transaction_checkpoint = 0;
}

/*============================================================================================*/