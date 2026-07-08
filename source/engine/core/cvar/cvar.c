/*==============================================================================================

    cvar.c

    System for managing console variables.

==============================================================================================*/

/* Case-insensitive string compare helper */

static bool
cvar_str_icmp_eq( const char* a, const char* b )
{
    while ( *a && *b )
    {
        char ca = *a;
        if ( ca >= 'A' && ca <= 'Z' )
            ca = ca + ( 'a' - 'A' );

        char cb = *b;
        if ( cb >= 'A' && cb <= 'Z' )
            cb = cb + ( 'a' - 'A' );

        if ( ca != cb )
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// clang-format off
/*==============================================================================================

    Utility Functions

==============================================================================================*/

static u32
cvar_hash( const char* s )
{
    u32 h = 2166136261u;    // FNV offset basis
    while ( *s )
    {
        char c = *s++;
        if ( c >= 'A' && c <= 'Z' )
            c = c + ( 'a' - 'A' );

        h ^= ( unsigned char )c;
        h *= 16777619u;    // FNV prime
    }
    return h;
}

/*==============================================================================================

    Cvar System : Callbacks

    * Supports multiple callbacks per cvar with module tracking for hot reload.
    * Cvar callbacks are called on value changes.
    * Callbacks are referenced by id if a cvar has any (cvar.callback_id field)

    * Callback table linking cvars to functions. 
    * Each function slot records its owning module id (from mod_current_id() at registration) 
    * Hot-reload can drop exactly the pointers that are about to dangle, 
    * Multiple modules can hook the same cvar.

==============================================================================================*/

#define MAX_CVAR_CALLBACKS      128       // Max global callbacks
#define MAX_CVAR_FUNCS_PER_CVAR 3         // Max callbacks per cvar
#define INVALID_ID              0xFFFF    // Invalid callback ID (no callback)

typedef struct cvar_callback_s
{
    u16 function_id[ MAX_CVAR_FUNCS_PER_CVAR ];    // Function indices (INVALID_ID = empty)
    u16 module_id[ MAX_CVAR_FUNCS_PER_CVAR ];      // Owning module per slot (INVALID_ID = host/none)

} cvar_callback_t;

// The function pointer array stores a pointer-sized encoded "next free index",
// instead of a function pointer when slot is free.

static cvar_callback_fn g_function_array[ MAX_CVAR_CALLBACKS ]; // function pointer array.
static cvar_callback_t  g_callback_table[ MAX_CVAR_CALLBACKS ]; // callback entry.
static u16              g_callback_count     = 0;               // number of used callback slots
static u16              g_function_free_head = INVALID_ID;      // free function pointer storage.
static u16              g_callback_free_head = INVALID_ID;      // freed callback-table entries (next in function_id[0])

/*============================================================================================*/
/* Initialize callback system */

void
cvar_callbacks_init()
{
    g_callback_count = 0;

    /* Fill every u16 with INVALID_ID (0xFFFF): memset writes byte 0xFF, intentionally */
    memset( g_callback_table, 0xFF, sizeof( g_callback_table ) );

    /* Initialize intrusive free list in g_function_array */
    for ( u16 i = 0; i < MAX_CVAR_CALLBACKS - 1; i++ )
    {
        /* Encode next index as pointer value */
        g_function_array[ i ] = ( cvar_callback_fn )( uintptr_t )( i + 1 );
    }
    g_function_array[ MAX_CVAR_CALLBACKS - 1 ] = ( cvar_callback_fn )( uintptr_t )INVALID_ID;
    g_function_free_head                                = 0; /* Head points to first free slot */
    g_callback_free_head                             = INVALID_ID;
}

/*============================================================================================*/
/* Allocate a free function slot (O(1)) */

static inline u16
alloc_function_slot( cvar_callback_fn fn )
{
    if ( g_function_free_head == INVALID_ID )
        return INVALID_ID; /* No free slots */

    u16 slot = g_function_free_head;

    /* Pop from free list */
    g_function_free_head = ( u16 )( uintptr_t )g_function_array[ slot ];

    /* Assign actual function */
    g_function_array[ slot ] = fn;

    return slot;
}

/* Free a function slot (O(1)) */

static inline void
free_function_slot( u16 slot )
{
    if ( slot >= MAX_CVAR_CALLBACKS )
        return;

    /* Push this slot back into the free list */
    g_function_array[ slot ] = ( cvar_callback_fn )( uintptr_t )g_function_free_head;
    g_function_free_head              = slot;
}

/* Return a cvar's callback-table entry to the freelist (next-link stored in
   function_id[0]) and detach it from the cvar. Caller must have freed or kept
   every function slot first. */

static void
callback_table_release( cvar_t* cv )
{
    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    cb->function_id[ 0 ] = g_callback_free_head;
    g_callback_free_head       = cv->callback_id;

    cv->callback_id = INVALID_ID;
    cv->mods &= ~CVAR_CALLBACK;
}

/*============================================================================================*/
/* Module id provider - injected by the host via core_wire_mod_callbacks() so core never
   links against the mod library. NULL (tools, sandboxes without mod) means every
   registration is host-owned (-1 -> INVALID_ID, never matched by unload sweeps). */

static cvar_module_id_fn g_cvar_module_id_fn = NULL;

void
cvar_set_module_id_fn( cvar_module_id_fn fn )
{
    g_cvar_module_id_fn = fn;
}

/*============================================================================================*/
/* Register callback for cvar. The owning module id is resolved automatically: inside a
   DLL's init()/reload() this is that module's id; host code outside any lifecycle call
   gets -1 -> INVALID_ID (never matched by unload sweeps). */

u16
cvar_callback_register( cvar_t* cv, cvar_callback_fn fn )
{
    if ( !cv || !fn )
        return INVALID_ID;

    const i32 module_id = g_cvar_module_id_fn ? g_cvar_module_id_fn() : -1;

    /* Allocate callback slot if needed (reuse freed entries first) */
    if ( cv->callback_id == INVALID_ID )
    {
        if ( g_callback_free_head != INVALID_ID )
        {
            cv->callback_id = g_callback_free_head;
            g_callback_free_head  = g_callback_table[ g_callback_free_head ].function_id[ 0 ];
        }
        else if ( g_callback_count < MAX_CVAR_CALLBACKS )
        {
            cv->callback_id = g_callback_count++;
        }
        else
        {
            con_printf( "cvar: callback table full\n" );
            return INVALID_ID;
        }

        cv->mods |= CVAR_CALLBACK;

        cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

        for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
        {
            cb->function_id[ i ] = INVALID_ID;
            cb->module_id[ i ]   = INVALID_ID;
        }
    }

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    /* Find empty function slot */
    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        if ( cb->function_id[ i ] == INVALID_ID )
        {
            u16 slot = alloc_function_slot( fn );
            if ( slot == INVALID_ID )
            {
                con_printf( "cvar: no free callback slots available\n" );
                return INVALID_ID;
            }

            cb->function_id[ i ] = slot;
            cb->module_id[ i ]   = ( u16 )module_id;
            return cv->callback_id;
        }
    }

    con_printf( "cvar: no room for more callbacks on '%s'\n", cvar_get_name( cv ) );
    return INVALID_ID;
}

/*============================================================================================*/
/* Clear all callbacks associated with a single cvar */

void
cvar_callback_unregister( cvar_t* cv )
{
    if ( !cv || cv->callback_id == INVALID_ID )
        return;

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        const u16 fid = cb->function_id[ i ];
        if ( fid != INVALID_ID )
        {
            free_function_slot( fid );
            cb->function_id[ i ] = INVALID_ID;
        }
        cb->module_id[ i ] = INVALID_ID;
    }

    callback_table_release( cv );
}

/*============================================================================================*/
/* Invoke all callbacks for cvar */

void
cvar_callback_invoke( cvar_t* cv )
{
    if ( !cv || cv->callback_id == INVALID_ID )
        return;

    cvar_callback_t* cb = &g_callback_table[ cv->callback_id ];

    for ( int i = 0; i < MAX_CVAR_FUNCS_PER_CVAR; i++ )
    {
        u16 fid = cb->function_id[ i ];
        if ( fid != INVALID_ID && g_function_array[ fid ] )
        {
            g_function_array[ fid ]( cv );
        }
    }
}

/*============================================================================================*/
/* Remove all callbacks owned by a module (fired by the mod system's unload hook).
   Only the module's own function slots are freed; callbacks other modules registered
   on the same cvar survive. The table entry is released once every slot is empty. */

void
cvar_callback_unregister_by_module( i32 module_id )
{
    int total_cvars = cvar_get_count();

    for ( int i = 0; i < total_cvars; i++ )
    {
        cvar_t* cv = cvar_get_by_index( i );
        if ( !cv || cv->callback_id == INVALID_ID )
            continue;

        cvar_callback_t* cb        = &g_callback_table[ cv->callback_id ];
        bool             any_left  = false;

        for ( int k = 0; k < MAX_CVAR_FUNCS_PER_CVAR; k++ )
        {
            if ( cb->function_id[ k ] == INVALID_ID )
                continue;

            if ( cb->module_id[ k ] == ( u16 )module_id )
            {
                free_function_slot( cb->function_id[ k ] );
                cb->function_id[ k ] = INVALID_ID;
                cb->module_id[ k ]   = INVALID_ID;
            }
            else
            {
                any_left = true;
            }
        }

        if ( !any_left )
        {
            callback_table_release( cv );
        }
    }
}


/*==============================================================================================

    Cvar Hash Table - Open Addressing with Linear Probing

    - Hash is initialized to HASH_EMPTY.
    - uses u16 indexes into the cvar instance pool.
    - HASH_TOMBSTONE is used for deletion.
    - insert: linear probe to next empty slot (or first tombstone).
    - find: linear probe until empty slot or match.

==============================================================================================*/

#define MAX_CVARS      256                      // Maximum registered cvars
#define HASH_SIZE      512                      // Hash table size (power of 2)
#define HASH_MASK      ( HASH_SIZE - 1 )        // Bit mask for hash

#define HASH_EMPTY     ( ( u16 )0xFFFF )        // Empty slot sentinel
#define HASH_TOMBSTONE ( ( u16 )0xFFFE )        // Deleted slot sentinel

static u32    g_cvar_count = 0;                 // Number of registered cvars
static cvar_t g_cvar_pool[ MAX_CVARS ];         // Fixed cvar array
static u16    global_cvar_hash[ HASH_SIZE ];    // Hash table of cvar indices

string_pool_t g_cvar_string_pool;               // String pool for cvar names and descriptions

/*============================================================================================*/
/* Initialize hash table */

static void
cvar_hash_init()
{
    for ( u32 i = 0; i < HASH_SIZE; ++i ) global_cvar_hash[ i ] = HASH_EMPTY;
}

/*============================================================================================*/
/* Find cvar by name using linear probing (returns NULL if not found) */

static cvar_t*
cvar_hash_find( const char* name )
{
    if ( !name )
        return NULL;

    u32  hash  = cvar_hash( name ) & HASH_MASK;
    const u32 start = hash;

    while ( true )
    {
        const u16 idx = global_cvar_hash[ hash ];
        if ( idx == HASH_EMPTY )
        {
            return NULL;    // Not found
        }

        if ( idx != HASH_TOMBSTONE )
        {
            cvar_t*     cv      = &g_cvar_pool[ idx ];
            const char* cv_name = string_pool_get( &g_cvar_string_pool, cv->name );
            if ( cvar_str_icmp_eq( cv_name, name ) )
            {
                return cv;    // Found cvar!
            }
        }

        hash = ( hash + 1 ) & HASH_MASK;
        if ( hash == start )
        {
            return NULL;    // Full loop
        }
    }
}

/*============================================================================================*/
/* Insert cvar into hash table by its cvar array id */

static void
cvar_hash_insert( u32 cvar_index )
{
    cvar_t*     cv         = &g_cvar_pool[ cvar_index ];
    const char* name       = string_pool_get( &g_cvar_string_pool, cv->name );

    u32         hash       = cvar_hash( name ) & HASH_MASK;
    u32         start      = hash;
    u32         first_tomb = ( u32 )-1;    // first free found if adding after not found.

    while ( true )
    {
        u16 slot = global_cvar_hash[ hash ];

        if ( slot == HASH_EMPTY )
        {
            if ( first_tomb != ( u32 )-1 )
            {
                // new entry goes into first tombstone found.
                global_cvar_hash[ first_tomb ] = ( u16 )cvar_index;
            }
            else
            {
                // current slot is new entry.
                global_cvar_hash[ hash ] = ( u16 )cvar_index;
            }
            return;
        }
        else if ( slot == HASH_TOMBSTONE )
        {
            if ( first_tomb == ( u32 )-1 )
                first_tomb = hash;
        }
        else
        {
            /* If duplicate insertion we do nothing */

            cvar_t* other = &g_cvar_pool[ slot ];
            if ( cvar_str_icmp_eq( string_pool_get( &g_cvar_string_pool, other->name ), name ) )
            {
                /* Duplicate found - shouldn't happen */
                return;
            }
        }

        hash = ( hash + 1 ) & HASH_MASK;
        if ( hash == start )
        {
            log_write( LOG_LEVEL_FATAL, "cvar", "hash table full while inserting cvar" );
            ORB_UNREACHABLE();
        }
    }
}

/*==============================================================================================

    System Initialization

==============================================================================================*/

/* The user string pool is now a `user_string_pool_t` instance exported from string_pool.c */
extern user_string_pool_t g_user_string_pool;

void
cvar_system_init( void )
{
    string_pool_init( &g_cvar_string_pool );
    user_string_pool_init( &g_user_string_pool );

    cvar_hash_init();
    cvar_callbacks_init();
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

    User value promotion

    If a cvar was created as a user variable (CVAR_USR) but is now being registered
    as a built-in variable, we need to promote its value from the user string pool 
    to the main cvar system. Value promotion occurs during registration of built-in cvars.

==============================================================================================*/

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

/*==============================================================================================

    Register Functions

    Re-registration (dll hot reload) returns the existing entry. The typed registrars
    preserve the runtime value and skip string pool allocation in that case, so repeated
    reloads neither stomp user changes nor leak pool space.

==============================================================================================*/
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
    cv->callback_id = INVALID_ID;

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

/*============================================================================================*/

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

/*============================================================================================*/
/* Internal function that contains all the 'set' logic. */

static bool
cvar_set_value_internal( cvar_t* cv, const char* value )
{
    /* Check protection flags */
    if ( cv->flags & CVAR_ROM )
    {
        con_printf( "cvar: '%s' is read-only\n", cvar_get_name( cv ) );
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
    if ( !name || !value )
        return false;

    cvar_t* cv = cvar_find( name );
    if ( !cv )
        return false; /* Does not create, just returns false */

    return cvar_set_value_internal( cv, value );
}

bool
cvar_set( cvar_t* cv, const char* value )
{
    if ( !cv || !value )
        return false;

    return cvar_set_value_internal( cv, value );
}

/*============================================================================================*/
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

/* Get cvar value as string by name */

const char*
cvar_get_value( const char* name )
{
    return cvar_value_string( cvar_find( name ) );
}

/*============================================================================================*/
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