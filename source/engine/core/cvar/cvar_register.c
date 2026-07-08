/*==============================================================================================

    cvar_register.c

    Registration Functions

    Re-registration (dll hot reload) returns the existing entry. The typed registrars
    preserve the runtime value and skip string pool allocation in that case, so repeated
    reloads neither stomp user changes nor leak pool space.

    Includes user-value promotion: if a cvar was created as a user variable (CVAR_USR) but
    is now being registered as a built-in variable, its value is promoted from the user
    string pool into the main cvar system during that registration.

==============================================================================================*/
// clang-format off

static u16  g_user_off             = USER_STRING_INVALID_OFFSET;
static u16  g_user_buck            = USER_STRING_INVALID_LIST;
static bool g_user_promote_pending = false;

static void
cvar_cache_user_value( cvar_t* cv )
{
    // Cache user value info for later promotion since it is about to be removed
    // and we need to preserve the string value whos offset as stored in the cvar data.
    // Must run while cv->type is still CVAR_USR (cvar_register_internal overwrites it
    // with the real type right after calling this).

    if ( cv->type == CVAR_USR )
    {
        g_user_off             = cv->u.value_offset;
        g_user_buck            = cv->u.bucket_index;
        g_user_promote_pending = true;
    }
}

static void
cvar_promote_user_value( cvar_t* cv )
{
    if ( !g_user_promote_pending )
        return;

    g_user_promote_pending = false;

    if ( g_user_off == USER_STRING_INVALID_OFFSET || g_user_buck == USER_STRING_INVALID_LIST )
    {
        con_printf( "cvar: expected a user value to be cached\n" );
        return;
    }

    const u16   off     = g_user_off;
    const u16   buck    = g_user_buck;

    const char* val_str = user_string_pool_get( &g_user_string_pool, off );
    if ( val_str && val_str[ 0 ] )
    {
        // Use central value parser to assign correctly
        cvar_set( cv, val_str );
    }

    // Free user-pool allocation
    user_string_pool_free( &g_user_string_pool, off, buck );

    g_user_off  = USER_STRING_INVALID_OFFSET;
    g_user_buck = USER_STRING_INVALID_LIST;
}

/* Create the cvar entry and place in hash lookup.
   Sets *existed when the cvar was already registered with a real type (hot-reload
   re-registration). The user-var promotion path reports existed=false so the typed
   init still runs in full and the promoted value is applied over it. */

static cvar_t*
cvar_register_internal( const char* name, const char* desc, cvar_type_t base_type, u32 flags, bool* existed )
{
    *existed = false;

    // find existing cvar and return it (dll reload case)
    cvar_t* existing = cvar_hash_find( name );
    if ( existing )
    {
        // If a user var already existed, promote it to the real type
        if ( existing->type == CVAR_USR )
        {
            // Update metadata
            existing->desc = ( u16 )string_pool_push( &g_cvar_string_pool, desc ? desc : "" );
            cvar_cache_user_value( existing );    // must run before type is overwritten below
            existing->type  = base_type;
            existing->flags = ( u16 )flags;
            existing->mods  &= ~( CVAR_MODIFIED | CVAR_LATCHED );
            return existing;
        }

        // Base type changed across a reload: wipe the union and run typed init fresh.
        // The old pool allocations (str list / buf) are orphaned; rare one-time cost.
        if ( existing->type != base_type )
        {
            log_write( LOG_LEVEL_WARN, "cvar", "'%s' re-registered with a different type", name );
            existing->type  = base_type;
            existing->flags = ( u16 )flags;
            existing->mods  &= ~( CVAR_MODIFIED | CVAR_LATCHED );
            memset( &existing->i, 0, sizeof( existing->i ) );    // .i spans the whole union
            return existing;
        }

        *existed = true;
        return existing;
    }

    if ( cmd_exists( name ) )
    {
        // Not fatal: typed registrars dereference the returned pointer unconditionally, so
        // rejecting here would crash every call site. Warn instead -- the cvar is registered
        // but cmd_execute_string dispatches the command first, shadowing it.
        log_write( LOG_LEVEL_WARN, "cvar", "'%s' collides with a registered command; cvar is shadowed", name );
    }

    if ( g_cvar_count >= MAX_CVARS )
    {
        log_write( LOG_LEVEL_FATAL, "cvar", "pool overflow (max %d)", MAX_CVARS );
        ORB_UNREACHABLE();
    }

    cvar_t* cv = &g_cvar_pool[ g_cvar_count ];
    memset( cv, 0, sizeof( cvar_t ) );

    cv->name        = ( u16 )string_pool_push( &g_cvar_string_pool, name );
    cv->desc        = ( u16 )string_pool_push( &g_cvar_string_pool, desc );
    cv->type        = base_type;
    cv->flags       = ( u16 )flags;
    cv->mods         = 0;
    cv->callback_id = CVAR_CB_NONE;

    /* CVAR_USR must be initialized to valid 'empty' values */
    if ( base_type == CVAR_USR )
    {
        cv->u.value_offset = USER_STRING_INVALID_OFFSET;
        cv->u.bucket_index = USER_STRING_INVALID_LIST;
        cv->mods             = CVAR_USER_CREATED;
    }

    cvar_hash_insert( g_cvar_count );
    ++g_cvar_count;

    return cv;
}

/* Public wrapper (user-var creation path; typed registrars use the internal form) */

cvar_t*
cvar_register_base( const char* name, const char* desc, u32 flags )
{
    bool existed;
    return cvar_register_internal( name, desc, CVAR_USR, flags, &existed );
}

/* Register a boolean cvar */

cvar_t*
cvar_register_b( const char* name, const char* desc, bool value, u32 flags )
{
    bool    existed;
    cvar_t* cv = cvar_register_internal( name, desc, CVAR_BOOL, flags, &existed );

    if ( existed )
    {
        cv->b.reset = value;    // refresh default, keep runtime value across reload
        return cv;
    }

    cv->b.value = value;
    cv->b.latch = value;
    cv->b.reset = value;
    cvar_promote_user_value( cv );

    return cv;
}

/* Register an integer cvar with optional min/max bounds. min == max means unbounded unless
   the caller OR's CVAR_BOUNDED into `flags` (needed to clamp to a single value). */

cvar_t*
cvar_register_i( const char* name, const char* desc, i32 value, i32 min, i32 max, u32 flags )
{
    bool    existed;
    cvar_t* cv = cvar_register_internal( name, desc, CVAR_INT, flags, &existed );
    if ( min != max || ( flags & CVAR_BOUNDED ) )
        cv->flags |= CVAR_BOUNDED;
    else
        cv->flags &= ~CVAR_BOUNDED;

    if ( existed )
    {
        // refresh default and bounds, keep runtime value across reload
        cv->i.reset = value;
        cv->i.min   = min;
        cv->i.max   = max;
        return cv;
    }

    cv->i.value = value;
    cv->i.min   = min;
    cv->i.max   = max;
    cv->i.latch = value;
    cv->i.reset = value;
    cvar_promote_user_value( cv );
    return cv;
}

/* Register a float cvar with optional min/max bounds. min == max means unbounded unless
   the caller OR's CVAR_BOUNDED into `flags` (needed to clamp to a single value). */

cvar_t*
cvar_register_f( const char* name, const char* desc, f32 value, f32 min, f32 max, u32 flags )
{
    bool    existed;
    cvar_t* cv = cvar_register_internal( name, desc, CVAR_FLOAT, flags, &existed );
    if ( min != max || ( flags & CVAR_BOUNDED ) )
        cv->flags |= CVAR_BOUNDED;
    else
        cv->flags &= ~CVAR_BOUNDED;

    if ( existed )
    {
        // refresh default and bounds, keep runtime value across reload
        cv->f.reset = value;
        cv->f.min   = min;
        cv->f.max   = max;
        return cv;
    }

    cv->f.value = value;
    cv->f.min   = min;
    cv->f.max   = max;
    cv->f.latch = value;
    cv->f.reset = value;
    cvar_promote_user_value( cv );
    return cv;
}

/* Register a string list cvar (select from predefined options by index) */

cvar_t*
cvar_register_s( const char* name, const char* desc, const char** values, u32 count, u32 def_index, u32 flags )
{
    bool    existed;
    cvar_t* cv = cvar_register_internal( name, desc, CVAR_STR, flags, &existed );

    /* Reload: keep the existing option list and value; re-reserving would leak the pool */
    if ( existed )
        return cv;

    if ( !values || count == 0 )
        return cv;

    if ( def_index >= count )
        def_index = 0;

    /* Find maximum string length */
    u32 maxlen = 0;
    for ( u32 i = 0; i < count; ++i )
    {
        u32 len = ( u32 )strlen( values[ i ] ) + 1;
        if ( len > maxlen )
            maxlen = len;
    }
    maxlen = string_pool_align_up( maxlen );

    /* Reserve contiguous space for all strings */
    u32 total_bytes = maxlen * count;
    u32 base_off    = string_pool_reserve( &g_cvar_string_pool, total_bytes );

    /* Copy strings into fixed-width slots */
    for ( u32 i = 0; i < count; ++i )
    {
        char* dst = g_cvar_string_pool.data + base_off + ( i * maxlen );
        strncpy( dst, values[ i ], maxlen - 1 );
        dst[ maxlen - 1 ] = '\0';
    }

    cv->s.base  = ( u16 )base_off;
    cv->s.width = ( u16 )maxlen;
    cv->s.count = ( u16 )count;
    cv->s.value = ( u16 )def_index;
    cv->s.latch = ( u16 )def_index;
    cv->s.reset = ( u16 )def_index;

    cvar_promote_user_value( cv );
    return cv;
}

/* Register a writable string buffer cvar with fixed size */

cvar_t*
cvar_register_w( const char* name, const char* desc, const char* reset, u32 size, u32 flags )
{
    const i32 align_size = string_pool_align_up( size );

    bool    existed;
    cvar_t* cv = cvar_register_internal( name, desc, CVAR_BUF, flags, &existed );

    /* Reload: keep the existing buffer and value; re-reserving would leak the pool */
    if ( existed )
        return cv;

    cv->w.reset        = ( u16 )string_pool_push( &g_cvar_string_pool, reset );
    cv->w.size         = ( u16 )align_size;
    cv->w.buf          = ( u16 )string_pool_reserve( &g_cvar_string_pool, cv->w.size );
    string_pool_write( &g_cvar_string_pool, cv->w.buf, reset, cv->w.size );

    cvar_promote_user_value( cv );

    return cv;
}

/* Register a read-only string reference cvar */

cvar_t*
cvar_register_r( const char* name, const char* desc, const char* value, u32 flags )
{
    bool    existed;
    cvar_t* cv = cvar_register_internal( name, desc, CVAR_REF, flags, &existed );

    /* Reload: keep the existing reference; re-pushing would leak the pool */
    if ( existed )
        return cv;

    cv->r.value = ( u16 )string_pool_push( &g_cvar_string_pool, value );

    cvar_promote_user_value( cv );
    return cv;
}

/* Register a user-created string cvar */

cvar_t*
cvar_register_u( const char* name, const char* value )
{
    cvar_t* cv = cvar_register_base( name, NULL, 0 );
    cvar_set( cv, value );
    return cv;
}

/*============================================================================================*/
// clang-format on
