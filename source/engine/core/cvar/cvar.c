/*==============================================================================================

    cvar.c

    System for managing console variables. Core value access/modification -- storage
    (cvar_hash.c), registration (cvar_register.c), the priority guard (cvar_priority.c), and
    callbacks (cvar_callback.c) each live in their own file; this one holds init, lookup,
    type queries, and get/set/print.

==============================================================================================*/
// clang-format off

/*==============================================================================================

    System Initialization

==============================================================================================*/

void
cvar_system_init( void )
{
    string_pool_init( &g_cvar_string_pool );
    user_string_pool_init( &g_user_string_pool );

    cvar_hash_init();
    cvar_callbacks_init();
    cvar_priority_reset();
    g_cvar_count = 0;
}

void
cvar_system_exit( void )
{
    string_pool_exit( &g_cvar_string_pool );
    user_string_pool_exit( &g_user_string_pool );
    g_cvar_count = 0;
}

void
cvar_compact_user_pool( void )
{
    user_string_pool_t new_pool = { 0 };
    user_string_pool_init( &new_pool );

    for ( u32 i = 0; i < g_cvar_count; i++ )
    {
        cvar_t* cv = &g_cvar_pool[ i ];
        if ( cv->type == CVAR_USR )
        {
            const char* str = user_string_pool_get( &g_user_string_pool, cv->u.value_offset );
            if ( str )
            {
                u16 new_bucket;
                u16 new_offset     = user_string_pool_alloc( &new_pool, str, &new_bucket );
                cv->u.value_offset = new_offset;
                cv->u.bucket_index = new_bucket;
            }
        }
    }

    user_string_pool_exit( &g_user_string_pool );
    g_user_string_pool = new_pool;
}

/*==============================================================================================

    Lookup Functions

==============================================================================================*/

cvar_t*
cvar_find( const char* name )
{
    return cvar_hash_find( name );
}

cvar_t*
cvar_get_by_index( u32 index )
{
    if ( index >= g_cvar_count )
        return NULL;

    return &g_cvar_pool[ index ];
}

u32
cvar_get_count( void )
{
    return g_cvar_count;
}

/*==============================================================================================

    Type Query Functions

==============================================================================================*/

bool cvar_is_bool   ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_BOOL );  }
bool cvar_is_int    ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_INT );   }
bool cvar_is_float  ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_FLOAT ); }
bool cvar_is_str    ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_STR );   }
bool cvar_is_buf    ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_BUF );   }
bool cvar_is_ref    ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_REF );   }
bool cvar_is_user   ( const cvar_t* cv ) { return ( cv && cv->type == CVAR_USR );   }

/*==============================================================================================

    Value Access Functions

==============================================================================================*/

static const char*
cvar_pool_string( u16 offset )
{
    if ( offset >= g_cvar_string_pool.used )
        return "<bad offset>";
    return g_cvar_string_pool.data + offset;
}

const char*
cvar_get_name( const cvar_t* cv )
{
    if ( !cv )
        return "<null>";
    return cvar_pool_string( cv->name );
}

const char*
cvar_get_desc( const cvar_t* cv )
{
    if ( !cv )
        return "<null>";
    return cvar_pool_string( cv->desc );
}

bool
cvar_get_bool( const cvar_t* cv )
{
    assert( cvar_is_bool( cv ) );
    return cv->b.value;
}

i32
cvar_get_int( const cvar_t* cv )
{
    assert( cvar_is_int( cv ) );
    return cv->i.value;
}

f32
cvar_get_float( const cvar_t* cv )
{
    assert( cvar_is_float( cv ) );
    return cv->f.value;
}

const char*
cvar_get_string_from_id( const cvar_t* cv, i32 value_id )
{
    if ( !cv || cv->type != CVAR_STR )
        return "";

    if ( cv->s.count == 0 )
        return "";

    // String set strings are in the main pool
    return g_cvar_string_pool.data + cv->s.base + ( value_id * cv->s.width );
}

const char*
cvar_get_string( const cvar_t* cv )
{
    if ( !cv )
        return "";

    switch ( cv->type )
    {
        case CVAR_STR: return cvar_get_string_from_id( cv, cv->s.value );
        case CVAR_BUF: return string_pool_get( &g_cvar_string_pool, cv->w.buf );
        case CVAR_REF: return string_pool_get( &g_cvar_string_pool, cv->r.value );
        case CVAR_USR: return user_string_pool_get( &g_user_string_pool, cv->u.value_offset );
        default: return "";
    }
}

/* Format any cvar's current value as a display string. Numeric types format into a
   round-robin buffer; string types forward to cvar_get_string (single owner of that logic). */

const char*
cvar_value_string( const cvar_t* cv )
{
    if ( !cv )
        return "";

    // Round-robin buffer so multiple calls survive within a single printf.
    // NOT thread safe; the cvar system is single-threaded by contract.
    static char bufs[ 4 ][ 32 ];
    static int  buf_idx = 0;
    char*       buf     = bufs[ buf_idx++ & 3 ];

    switch ( cv->type )
    {
        case CVAR_BOOL:     return ( cv->b.value ? "1" : "0" );
        case CVAR_INT:      snprintf( buf, sizeof( bufs[ 0 ] ), "%d", cv->i.value ); return buf;
        case CVAR_FLOAT:    snprintf( buf, sizeof( bufs[ 0 ] ), "%g", cv->f.value ); return buf;
        default:            return cvar_get_string( cv );
    }
}

/*==============================================================================================

    Value Modification

==============================================================================================*/

/* Reset cvar to default value */

void
cvar_reset( cvar_t* cv )
{
    if ( !cv )
        return;

    bool changed = false;

    switch ( cv->type )
    {
        case CVAR_BOOL:
            changed     = ( cv->b.value != cv->b.reset );
            cv->b.value = cv->b.reset;
            cv->b.latch = cv->b.reset;
            break;

        case CVAR_INT:
            changed     = ( cv->i.value != cv->i.reset );
            cv->i.value = cv->i.reset;
            cv->i.latch = cv->i.reset;
            break;

        case CVAR_FLOAT:
            changed     = ( cv->f.value != cv->f.reset );
            cv->f.value = cv->f.reset;
            cv->f.latch = cv->f.reset;
            break;

        case CVAR_STR:
            changed     = ( cv->s.value != cv->s.reset );
            cv->s.value = cv->s.reset;
            cv->s.latch = cv->s.reset;
            break;

        case CVAR_BUF:
        {
            const char* reset_str = g_cvar_string_pool.data + cv->w.reset;
            changed = ( strcmp( string_pool_get( &g_cvar_string_pool, cv->w.buf ), reset_str ) != 0 );
            string_pool_write( &g_cvar_string_pool, cv->w.buf, reset_str, cv->w.size );
            break;
        }

            // CVAR_USR has no "reset" value. Freeing its current value is
            // the equivalent of "resetting" it to an empty string.

        case CVAR_USR:
            changed = ( user_string_pool_get( &g_user_string_pool, cv->u.value_offset )[ 0 ] != '\0' );
            user_string_pool_free( &g_user_string_pool, cv->u.value_offset, cv->u.bucket_index );
            cv->u.value_offset = USER_STRING_INVALID_OFFSET;
            cv->u.bucket_index = USER_STRING_INVALID_LIST;
            break;

        default: break;
    }

    if ( changed && ( cv->mods & CVAR_CALLBACK ) )
        cvar_callback_invoke( cv );

    cv->mods &= ~( CVAR_MODIFIED | CVAR_LATCHED );

    // Back at the code default -- a lower-priority source is free to set it again.
    cv->priority = CVAR_PRI_CODE;
}

/* Reset all cvars to default values */

void
cvar_reset_all( void )
{
    for ( u32 i = 0; i < g_cvar_count; ++i )
    {
        cvar_t* cv = &g_cvar_pool[ i ];

        /* Skip CVAR_NORESTART variables */
        if ( cv->flags & CVAR_NORESTART )
            continue;

        cvar_reset( cv );
    }
}

/* Apply all latched cvar values */

void
cvar_apply_latched( void )
{
    for ( u32 i = 0; i < g_cvar_count; ++i )
    {
        cvar_t* cv = &g_cvar_pool[ i ];

        if ( !( cv->mods & CVAR_LATCHED ))
            continue;

        switch ( cv->type )
        {
            case CVAR_BOOL:     cv->b.value = cv->b.latch; break;
            case CVAR_INT:      cv->i.value = cv->i.latch; break;
            case CVAR_FLOAT:    cv->f.value = cv->f.latch; break;
            case CVAR_STR:      cv->s.value = cv->s.latch; break;

            default: break;
        }

        cv->mods &= ~CVAR_LATCHED;
        cv->mods |= CVAR_MODIFIED;

        if ( cv->mods & CVAR_CALLBACK )
            cvar_callback_invoke( cv );
    }
}

/* Clear all CVAR_MODIFIED flags */

void
cvar_clear_modified( void )
{
    for ( u32 i = 0; i < g_cvar_count; ++i )
    {
        g_cvar_pool[ i ].mods &= ~CVAR_MODIFIED;
    }
}

/*==============================================================================================

    Named Access

    Set/get a cvar by name or by pointer, priority-gated through cvar_set_value_internal
    (see the Priority Guard section above for what gates a set and what tags an unspecified
    one). This is the layer cmd.c's "set"/"seta"/bare-name dispatch and cvar_load_defaults'
    config exec both go through.

==============================================================================================*/

/* Internal function that contains all the 'set' logic. */

static bool
cvar_set_value_internal( cvar_t* cv, const char* value, cvar_priority_t priority )
{
    /* Check protection flags */
    if ( cv->flags & CVAR_ROM )
    {
        con_printf( "cvar: '%s' is read-only\n", cvar_get_name( cv ) );
        return false;
    }

    if ( g_cvar_priority_guard && priority < ( cvar_priority_t )cv->priority )
    {
        con_printf( "cvar: '%s' was set by a higher-priority source, ignoring\n", cvar_get_name( cv ) );
        return false;
    }

    /* TODO: Check CVAR_INIT, CVAR_CHEAT flags based on system state */

    const cvar_type_t type = cv->type;
    bool changed = false;
    bool success = true;    // false = value rejected (parse error / read-only type)

    switch ( type )
    {
        case CVAR_BOOL:
        {
            /* Parse: 1/0, true/false, on/off, yes/no */

            bool new_value = false;
            bool parsed = false;

            // Fast single-character check
            if ( value[ 0 ] && !value[ 1 ] )
            {
                if      ( value[ 0 ] == '1' ) { new_value = true;  parsed = true; }
                else if ( value[ 0 ] == '0' ) { new_value = false; parsed = true; }
            }
            else
            {
                if      ( cvar_str_icmp_eq( value, "true" )  || cvar_str_icmp_eq( value, "on" )  || cvar_str_icmp_eq( value, "yes" ) ) { new_value = true;  parsed = true; }
                else if ( cvar_str_icmp_eq( value, "false" ) || cvar_str_icmp_eq( value, "off" ) || cvar_str_icmp_eq( value, "no" ) )  { new_value = false; parsed = true; }
            }

            if ( !parsed ) { success = false; break; } // Invalid bool string

            /* Apply new value, handle latch/modify logic */

            const bool is_latched = ( cv->flags & CVAR_LATCH );
            bool* target = is_latched ? &cv->b.latch : &cv->b.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->mods |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
        case CVAR_INT:
        {
            char* endptr = NULL;
            long  new_value = strtol( value, &endptr, 0 );

            // Check for conversion error.
            if ( endptr == value || *endptr != '\0' ) {
                success = false;
                break;
            }

            if ( cv->flags & CVAR_BOUNDED )
            {
                if ( new_value < cv->i.min ) new_value = cv->i.min;
                if ( new_value > cv->i.max ) new_value = cv->i.max;
            }
            
            const bool is_latched = ( cv->flags & CVAR_LATCH );
            i32* target = is_latched ? &cv->i.latch : &cv->i.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->mods |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
        case CVAR_FLOAT:
        {
            char* endptr = NULL;
            float new_value = strtof( value, &endptr );

            // Check for conversion error.
            if ( endptr == value || *endptr != '\0' ) {
                success = false;
                break;
            }

            if ( cv->flags & CVAR_BOUNDED )
            {
                if ( new_value < cv->f.min ) new_value = cv->f.min;
                if ( new_value > cv->f.max ) new_value = cv->f.max;
            }

            const bool is_latched = ( cv->flags & CVAR_LATCH );
            f32* target = is_latched ? &cv->f.latch : &cv->f.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->mods |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }        
        case CVAR_STR:
        {        
            u16 new_value = 0xFFFF;

            /* Accept numeric index or string match */
            if ( isdigit( ( unsigned char )value[ 0 ] ) )
            {
                char* endptr = NULL;
                u32   idx    = ( u32 )strtoul( value, &endptr, 10 );
                if ( *endptr == '\0' && idx < cv->s.count )
                    new_value = ( u16 )idx;
            }
            else
            {
                /* TODO: find best match for convenience */
                /* Find matching string (case-insensitive) */
                for ( u32 i = 0; i < cv->s.count; ++i )
                {
                    const char* s = g_cvar_string_pool.data + cv->s.base + i * cv->s.width;
                    if ( cvar_str_icmp_eq( s, value ) )
                    {
                        new_value = ( u16 )i;
                        break;    // first match wins
                    }
                }
            }

            if ( new_value == 0xFFFF )
            {
                success = false;    // no matching option
                break;
            }

            const bool is_latched = ( cv->flags & CVAR_LATCH );
            u16* target = is_latched ? &cv->s.latch : &cv->s.value;
            if ( *target != new_value )
            {
                *target = new_value;
                cv->mods |= is_latched ? CVAR_LATCHED : CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
        case CVAR_BUF:
        {
            const char* cur = string_pool_get( &g_cvar_string_pool, cv->w.buf );
            if ( strcmp( cur, value ) != 0 )
            {
                string_pool_write( &g_cvar_string_pool, cv->w.buf, value, cv->w.size );
                cv->mods |= CVAR_MODIFIED;
                changed = true;
            }
            break;
        }
        case CVAR_REF:
        {
            /* read-only reference - cannot set */
            success = false;
            break;
        }
        case CVAR_USR:
        {
            /* Compare BEFORE freeing: the freelist commonly returns the same offset
               for a same-bucket realloc, so an after-the-fact offset compare would
               report "unchanged" for a value that did change. */

            const char* cur = user_string_pool_get( &g_user_string_pool, cv->u.value_offset );
            if ( strcmp( cur, value ) == 0 )
                break;    // unchanged

            /* Free the old string's buffer and allocate from the correct size pool */

            user_string_pool_free( &g_user_string_pool, cv->u.value_offset, cv->u.bucket_index );

            u16 new_bucket;
            u16 new_offset = user_string_pool_alloc( &g_user_string_pool, value, &new_bucket );

            cv->u.value_offset = new_offset;
            cv->u.bucket_index = new_bucket;
            cv->mods |= CVAR_MODIFIED;
            changed = true;
            break;
        }
    }

    // Record who last successfully set this cvar, even if the value didn't change (e.g. the
    // console re-setting an already-current value) -- a later lower-priority source must
    // still be blocked from stomping it.
    if ( success )
        cv->priority = ( u8 )priority;

    // Invoke callbacks if value changed
    if ( changed && ( cv->mods & CVAR_CALLBACK ) )
    {
        cvar_callback_invoke( cv );
    }

    return success;
}

/*============================================================================================*/
/* Set cvar value by name with string value (returns true if changed) */
/* This is the implementation of the new 'non-creating' set function */
/* Used for "var value" style assignment */

bool
cvar_set_value( const char* name, const char* value )
{
    return cvar_set_value_pri( name, value, cvar_source_priority() );
}

bool
cvar_set_value_pri( const char* name, const char* value, cvar_priority_t priority )
{
    if ( !name || !value )
        return false;

    cvar_t* cv = cvar_find( name );
    if ( !cv )
        return false; /* Does not create, just returns false */

    return cvar_set_value_internal( cv, value, priority );
}

bool
cvar_set( cvar_t* cv, const char* value )
{
    return cvar_set_pri( cv, value, cvar_source_priority() );
}

bool
cvar_set_pri( cvar_t* cv, const char* value, cvar_priority_t priority )
{
    if ( !cv || !value )
        return false;

    return cvar_set_value_internal( cv, value, priority );
}

/* Get cvar value as string by name */

const char*
cvar_get_value( const char* name )
{
    return cvar_value_string( cvar_find( name ) );
}

/*==============================================================================================

    Cvar Output

==============================================================================================*/

/* Print cvar value with type info */

void
cvar_print_value( const cvar_t* cv )
{
    if ( !cv )
        return;

    const char* name  = cvar_get_name( cv );
    const char* value = cvar_value_string( cv );

    con_printf( "  \"%s\" is: \"%s\"", name, value );

    /* Show latched value if present */
    if ( cv->mods & CVAR_LATCHED )
    {
        con_printf( " (latched)" );
    }

    /* Show type info */
    switch ( cv->type )
    {
        case CVAR_BOOL: con_printf( " [bool]" ); break;
        case CVAR_INT:
            if ( cv->flags & CVAR_BOUNDED )
                    con_printf( " [int: %d..%d]", cv->i.min, cv->i.max );
            else    con_printf( " [int]" );
            break;

        case CVAR_FLOAT:
            if ( cv->flags & CVAR_BOUNDED )
                    con_printf( " [float: %.2f..%.2f]", cv->f.min, cv->f.max );
            else    con_printf( " [float]" );
            break;

        case CVAR_STR: con_printf( " [choice: %u of %u]", cv->s.value, cv->s.count ); break;
        case CVAR_BUF: con_printf( " [string]" ); break;
        case CVAR_REF: con_printf( " [readonly]" ); break;
        case CVAR_USR: con_printf( " [user]" ); break;
    }

    con_printf( "\n" );
}

/*============================================================================================*/
/* Print cvar flags */

void
cvar_print_flags( const cvar_t* cv )
{
    if ( !cv )
        return;

    con_printf( "  Type:" );

    if ( cv->flags & CVAR_ROM )        con_printf( " ROM" );
    if ( cv->flags & CVAR_INIT )       con_printf( " INIT" );
    if ( cv->flags & CVAR_LATCH )      con_printf( " LATCH" );
    if ( cv->flags & CVAR_CHEAT )      con_printf( " CHEAT" );

    if ( cv->flags & CVAR_RUNTIME )    con_printf( " RUNTIME" );
    if ( cv->flags & CVAR_NORESTART )  con_printf( " NORESTART" );

    if ( cv->flags & CVAR_ARCHIVE )    con_printf( " ARCHIVE" );

    if ( cv->flags & CVAR_DEVONLY )    con_printf( " DEVONLY" );
    if ( cv->flags & CVAR_HIDDEN )     con_printf( " HIDDEN" );

    if ( cv->flags & CVAR_NETSYNC )    con_printf( " NETSYNC" );
    if ( cv->flags & CVAR_USERINFO )   con_printf( " USERINFO" );
    if ( cv->flags & CVAR_SERVERINFO ) con_printf( " SERVERINFO" );
    if ( cv->flags & CVAR_SYSTEMINFO ) con_printf( " SYSTEMINFO" );
    
    con_printf( "\n" );
}

/*============================================================================================*/
// clang-format on