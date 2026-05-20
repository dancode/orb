/*==============================================================================================

    mod_internal.c — Module system internals.

    Unity include for mod.c — not compiled directly. All symbols are file-local (static)
    or internal helpers consumed only within that translation unit.

==============================================================================================*/
/*==============================================================================================
    Internal Constants
==============================================================================================*/

#define MODULE_NAME_MAX 32  /* max module name length including null terminator */
#define MAX_MODULES     16  /* max concurrently registered modules */
#define MAX_PENDING     16  /* max modules awaiting debounced reload */
#define DEBOUNCE_MS     200 /* milliseconds to wait after last file change before reloading */

#ifdef _WIN32
    #define PATH_SEP '\\'
#else
    #define PATH_SEP '/'
#endif

#ifndef MAX_PATH
    #define MAX_PATH 260
#endif

/*==============================================================================================
    Internal Types
==============================================================================================*/
// clang-format off

typedef struct mod_info_s
{
    char                name[ MODULE_NAME_MAX ];
    module_status_t     status;

    bool                is_static;     /* true → no DLL, no shadow copies */
    uint32_t            shadow_count;  /* shadow file name counter for this slot */

    void*               dll;           /* handle to the loaded shadow copy */
    uint64_t            last_write;    /* file timestamp at last successful load */

    mod_desc_t*         mod_desc;      /* lifecycle: init / exit / reload + func_api */

    void*               state;         /* persistent state block; system-owned */
    int32_t             state_size;    /* size of the current allocation */

    void*               api_slot;      /* stable address; system writes new func_api here on reload */
    int32_t             api_slot_size; /* sizeof(func_api); fixed for the module's lifetime */

    int32_t             version;       /* increments on each successful hot-reload */

} mod_info_t;

typedef struct pending_reload_s
{
    char                name[ MODULE_NAME_MAX ];
    uint64_t            flagged_at_ms;   /* time the change was first detected */
    uint64_t            last_seen_write; /* file timestamp at flag time; reset if the file moves again */

} pending_reload_t;

/* Captured pre-reload handles. Restored by snapshot_rollback() if any step fails. */
typedef struct mod_snapshot_s
{
    void*               dll;
    mod_desc_t*         mod_desc;
    uint32_t            shadow_count;
    uint64_t            last_write;
    module_status_t     status;

} mod_snapshot_t;

/*==============================================================================================
    Global State
==============================================================================================*/

static mod_info_t       g_modules[ MAX_MODULES ];
static int32_t          g_module_count;

static int32_t          g_init_order[ MAX_MODULES ];
static int32_t          g_init_count;

static pending_reload_t g_pending[ MAX_PENDING ];
static int32_t          g_pending_count;

static char             g_root[ MAX_PATH ];
static char             g_last_error[ 256 ];
static uint32_t         g_shadow_counter;  /* globally incremented per shadow copy created */
static get_api_fn       g_api_func;        /* passed into every init() / reload() */

/*==============================================================================================
    Bootstrap Allocator

    Used from startup until core's init() has run. Intentionally simple.
==============================================================================================*/

static void
ms_log( const char* fmt, ... )
{
    char buf[ 512 ];

    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    /* route log messages through the host-provided log function */

    if ( g_mod_log_fn )
         g_mod_log_fn( ORB_LOG_INFO, "mod", buf );
    else
        printf( "%s\n", buf );
}

static void*
ms_alloc( size_t size )
{
    return malloc( size );
}

static void
ms_free( void* ptr )
{
    free( ptr );
}

/*==============================================================================================
    Error  (mod_last_error is public, lives in mod.c)
==============================================================================================*/

static void
set_error( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( g_last_error, sizeof( g_last_error ), fmt, args );
    va_end( args );
    ms_log( "[module] ERROR: %s", g_last_error );
}

/*==============================================================================================
    Path Helpers
==============================================================================================*/

static void
path_dll( const char* name, char* out, size_t size )
{
    /* "<root>\<name>.dll" */
    snprintf( out, size, "%s%c%s.dll", g_root, PATH_SEP, name );
}

static void
path_shadow( const char* name, uint32_t counter, char* out, size_t size )
{
    /* "<root>\<name>.tmp_<counter>.dll" */
    snprintf( out, size, "%s%c%s.tmp_%u.dll", g_root, PATH_SEP, name, counter );
}

static uint64_t
dll_file_time( const char* name )
{
    char path[ MAX_PATH ];
    path_dll( name, path, sizeof( path ) );
    return sys_file_time( path );
}

/*==============================================================================================
    Shadow File Helpers
==============================================================================================*/

static bool
shadow_copy( const char* name, uint32_t counter )
{
    char src[ MAX_PATH ], dst[ MAX_PATH ];
    path_dll( name, src, sizeof( src ) );
    path_shadow( name, counter, dst, sizeof( dst ) );

    if ( !sys_file_copy( src, dst ) )
    {
        set_error( "copy failed: %s > %s", src, dst );
        return false;
    }
    return true;
}

static void
shadow_delete( const char* name, uint32_t counter )
{
    char path[ MAX_PATH ];
    path_shadow( name, counter, path, sizeof( path ) );
    sys_file_delete( path ); /* best-effort; ignore failure */
}

/*==============================================================================================
    API Struct Validation

    Contract: func_api is a struct of function pointers only. func_api_size is therefore
    an exact multiple of sizeof(void*), and each slot must be a non-NULL function pointer
    after a successful load or reload. Consumers cache the api_slot address and dispatch
    through it without per-call NULL checks, so any NULL slot is a host crash waiting to
    happen. Runs on every load and every reload.
==============================================================================================*/

static bool
api_validate( const char* name, const void* func_api, int32_t func_api_size )
{
    if ( !func_api )
    {
        set_error( "'%s': func_api is NULL", name );
        return false;
    }
    if ( func_api_size <= 0 )
    {
        set_error( "'%s': func_api_size is zero", name );
        return false;
    }
    if ( ( func_api_size % ( int32_t )sizeof( void* ) ) != 0 )
    {
        set_error(
            "'%s': func_api_size (%d) is not a multiple of sizeof(void*) (%zu) - "
            "API struct must contain only function pointers",
            name, func_api_size, sizeof( void* ) );
        return false;
    }

    const void* const* slots      = ( const void* const* )func_api;
    int                slot_count = ( int )( func_api_size / ( int32_t )sizeof( void* ) );
    int                null_count = 0;

    for ( int i = 0; i < slot_count; ++i )
    {
        if ( !slots[ i ] )
        {
            ms_log( "[module] '%s': func_api slot %d (byte offset %d) is NULL", name, i, i * ( int )sizeof( void* ) );
            ++null_count;
        }
    }

    if ( null_count > 0 )
    {
        set_error( "'%s': %d NULL function pointer(s) in func_api - every slot must be set", name, null_count );
        return false;
    }

    return true;
}

/*==============================================================================================
    Low-level DLL Load / Unload

    Allocate/release the shadow copy and the OS library handle. State, api_slot, and
    lifecycle calls are the caller's responsibility.
==============================================================================================*/

static bool
slot_load_dll( mod_info_t* m )
{
    /* --- shadow copy --- */

    m->shadow_count = g_shadow_counter++;
    if ( !shadow_copy( m->name, m->shadow_count ) )
        return false;

    char shadow[ MAX_PATH ];
    path_shadow( m->name, m->shadow_count, shadow, sizeof( shadow ) );

    /* --- load shadow DLL --- */

    m->dll = sys_library_load( shadow );
    if ( !m->dll )
    {
        set_error( "LoadLibrary failed: %s", shadow );
        shadow_delete( m->name, m->shadow_count );
        return false;
    }

    /* --- resolve module lifecycle struct --- */

    get_mod_desc_fn get_mod_desc = ( get_mod_desc_fn )sys_library_get_symbol( m->dll, "get_mod_desc" );
    if ( !get_mod_desc )
    {
        set_error( "'%s' is missing 'get_mod_desc' export", m->name );
        goto fail;
    }

    m->mod_desc = get_mod_desc();
    if ( !m->mod_desc )
    {
        set_error( "'%s' get_mod_desc() returned NULL", m->name );
        goto fail;
    }
    if ( !api_validate( m->name, m->mod_desc->func_api, m->mod_desc->func_api_size ) )
        goto fail;

    /* --- record file-time for hot-reload detection --- */

    m->last_write = dll_file_time( m->name );
    return true;

fail:
    sys_library_unload( m->dll );
    shadow_delete( m->name, m->shadow_count );
    m->dll     = NULL;
    m->mod_desc = NULL;
    return false;
}

static void
slot_unload_dll( mod_info_t* m )
{
    if ( !m->dll )
        return;

    sys_library_unload( m->dll );
    shadow_delete( m->name, m->shadow_count );

    m->dll     = NULL;
    m->mod_desc = NULL;
}

/*==============================================================================================
    Slot Management

    slot_create / slot_destroy bracket the entire slot lifecycle. slot_destroy is safe to
    call at any point during setup or after init — it frees only what was allocated and
    ignores fields that were never set.
==============================================================================================*/

static int
slot_find( const char* name )
{
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( g_modules[ i ].status != MODULE_STATUS_EMPTY &&
             strncmp( g_modules[ i ].name, name, MODULE_NAME_MAX ) == 0 )
            return i;
    }
    return -1; /* none found */
}

static int
slot_alloc( void )
{
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( g_modules[ i ].status == MODULE_STATUS_EMPTY )
        {
            ++g_module_count;
            return i;
        }
    }
    assert( 0 && "module slots exhausted" );
    return -1;
}

static void
slot_free( int slot )
{
    memset( &g_modules[ slot ], 0, sizeof( mod_info_t ) );
    --g_module_count;
}

/* Reserve a fresh slot with `name` set and everything else zeroed.
   Caller fills in is_static, mod_desc, status, then runs the setup helpers. */

static mod_info_t*
slot_create( const char* name )
{
    int slot = slot_alloc();
    if ( slot < 0 )
    {
        set_error( "no free module slots" );
        return NULL;
    }

    mod_info_t* m = &g_modules[ slot ];
    memset( m, 0, sizeof( *m ) );
    strncpy( m->name, name, MODULE_NAME_MAX - 1 );
    m->name[ MODULE_NAME_MAX - 1 ] = '\0';
    return m;
}

/* Full teardown — frees state, frees api_slot if heap-owned, unloads DLL, releases slot.
   Does NOT call exit(); the caller decides whether the module is live. */

static void
slot_destroy( mod_info_t* m )
{
    if ( m->state )
    {
        ms_free( m->state );
        m->state = NULL;
    }
    if ( !m->is_static && m->api_slot )
    {
        ms_free( m->api_slot );
        m->api_slot = NULL;
    }
    if ( !m->is_static )
        slot_unload_dll( m );

    slot_free( ( int )( m - g_modules ) );
}

/*==============================================================================================
    State Helper — Allocate or grow the persistent state block.

    Zeroes the block on first allocation. On reload, preserves existing data and zeroes
    only the newly added tail if state_size grew. Never shrinks the allocation.
==============================================================================================*/

static bool
state_ensure( mod_info_t* m )
{
    int32_t required = m->mod_desc->state_size;
    if ( required <= 0 )
        return true; /* stateless module */

    if ( !m->state )
    {
        /* first load — allocate and zero */
        m->state = ms_alloc( ( size_t )required );
        if ( !m->state )
        {
            set_error( "'%s': state alloc failed (%d bytes)", m->name, required );
            return false;
        }
        memset( m->state, 0, ( size_t )required );
        m->state_size = required;
    }
    else if ( required > m->state_size )
    {
        /* state grew across a reload — realloc and zero the new tail */
        void* new_state = ms_alloc( ( size_t )required );
        if ( !new_state )
        {
            set_error( "'%s': state realloc failed (%d bytes)", m->name, required );
            return false;
        }
        memcpy( new_state, m->state, ( size_t )m->state_size );
        memset( ( char* )new_state + m->state_size, 0, ( size_t )( required - m->state_size ) );
        ms_free( m->state );
        m->state      = new_state;
        m->state_size = required;
    }
    /* required < state_size: keep the larger allocation, never shrink */

    return true;
}

/*==============================================================================================
    API Slot Helpers — System-owned func_api block.

    Consumers cache the slot's address (returned by mod_get_api). On reload the system
    memcpys the new func_api into this same block — consumers automatically dispatch
    through the new pointers without any manual re-cache in their reload() callback.

    Static modules skip the copy: their func_api is already a stable linked-in const
    struct, so api_slot points directly at it.
==============================================================================================*/

static bool
api_slot_create( mod_info_t* m )
{
    int32_t size = m->mod_desc->func_api_size;

    if ( m->is_static )
    {
        /* const struct already has a stable address — point at it directly */
        m->api_slot      = ( void* )m->mod_desc->func_api;
        m->api_slot_size = size;
        return true;
    }

    m->api_slot = ms_alloc( ( size_t )size );
    if ( !m->api_slot )
    {
        set_error( "'%s': api slot alloc failed (%d bytes)", m->name, size );
        return false;
    }

    m->api_slot_size = size;
    memcpy( m->api_slot, m->mod_desc->func_api, ( size_t )size );
    return true;
}

/* Copy the new DLL's func_api into the existing stable slot. Caller must have verified
   func_api_size still matches m->api_slot_size. */

static void
api_slot_refresh( mod_info_t* m )
{
    memcpy( m->api_slot, m->mod_desc->func_api, ( size_t )m->api_slot_size );
}

/*==============================================================================================
    Lifecycle Invocations  (all pass g_api_func so DLLs can call back into the system)
==============================================================================================*/

static bool
call_init( mod_info_t* m )
{
    if ( !m->mod_desc || !m->mod_desc->init )
        return true;
    return m->mod_desc->init( m->state, g_api_func );
}

static void
call_exit( mod_info_t* m )
{
    if ( !m->mod_desc || !m->mod_desc->exit )
        return;
    m->mod_desc->exit( m->state );
}

static bool
call_reload( mod_info_t* m )
{
    if ( !m->mod_desc )
        return true;
    if ( m->mod_desc->reload )
        return m->mod_desc->reload( m->state, g_api_func );

    ms_log( "[module] '%s' has no on_reload(), dynamic reload requires it", m->name );
    return false;
}

/*==============================================================================================
    Reload Snapshot — captures live handles so a failed reload can put them back.
==============================================================================================*/

static void
snapshot_save( const mod_info_t* m, mod_snapshot_t* s )
{
    s->dll          = m->dll;
    s->mod_desc     = m->mod_desc;
    s->shadow_count = m->shadow_count;
    s->last_write   = m->last_write;
    s->status       = m->status;
}

/* Tear down any partially-loaded new DLL, restore the saved handles, and re-run reload()
   on the old code to bring the module back to INITIALIZED. `reason` labels the log line. */

static void
snapshot_rollback( mod_info_t* m, const mod_snapshot_t* s, const char* reason )
{
    /* If a new DLL was loaded since the snapshot, tear it down — m->dll and
       m->shadow_count still point at the new shadow at this moment. */
    if ( m->dll && m->dll != s->dll )
        slot_unload_dll( m );

    m->dll          = s->dll;
    m->mod_desc     = s->mod_desc;
    m->shadow_count = s->shadow_count;
    m->last_write   = s->last_write;

    /* Revive the old code. It already had exit() called, so on_reload is the
       right hook to put it back to INITIALIZED. */
    if ( s->status == MODULE_STATUS_INITIALIZED && call_reload( m ) )
    {
        m->status = MODULE_STATUS_INITIALIZED;
        ms_log( "[module] '%s' %s - rolled back to previous version", m->name, reason );
    }
    else
    {
        m->status = MODULE_STATUS_ERROR;
        ms_log( "[module] '%s' %s AND rollback failed", m->name, reason );
    }
}

/*==============================================================================================
    Pending Queue
==============================================================================================*/

static pending_reload_t*
pending_find( const char* name )
{
    for ( int i = 0; i < g_pending_count; ++i )
    {
        if ( strncmp( g_pending[ i ].name, name, MODULE_NAME_MAX ) == 0 )
            return &g_pending[ i ];
    }
    return NULL;
}

/* Queue or refresh a pending reload. If the entry already exists and the file has not
   moved again, the existing debounce continues; otherwise the debounce window restarts. */

static void
pending_flag( const char* name, uint64_t now_ms, uint64_t file_time )
{
    pending_reload_t* p = pending_find( name );

    if ( p )
    {
        if ( file_time == p->last_seen_write )
            return; /* same file — let existing debounce run out */
    }
    else
    {
        if ( g_pending_count >= MAX_PENDING )
        {
            ms_log( "[module] WARN: pending reload table full, skipping '%s'", name );
            return;
        }
        p = &g_pending[ g_pending_count++ ];
        strncpy( p->name, name, MODULE_NAME_MAX - 1 );
        p->name[ MODULE_NAME_MAX - 1 ] = '\0';
        ms_log( "[module] '%s' queued for reload (%d ms debounce)", name, DEBOUNCE_MS );
    }

    p->flagged_at_ms   = now_ms;
    p->last_seen_write = file_time;
}

/*==============================================================================================
    Reload Core — swap a single module. Called only from mod_system_flush_reloads().
==============================================================================================*/

static bool
do_reload( mod_info_t* m )
{
    if ( m->is_static )
    {
        ms_log( "[module] '%s' is static — reload is a no-op", m->name );
        return true;
    }

    mod_snapshot_t prev;
    snapshot_save( m, &prev );

    if ( m->status == MODULE_STATUS_INITIALIZED )
        call_exit( m );

    /* Detach old handles so slot_load_dll has a clean slate to write into. */
    m->dll     = NULL;
    m->mod_desc = NULL;

    if ( !slot_load_dll( m ) )
    {
        snapshot_rollback( m, &prev, "load failed" );
        return false;
    }

    if ( m->mod_desc->func_api_size != m->api_slot_size )
    {
        set_error( "'%s': func_api_size changed across reload (%d -> %d); requires full restart", m->name,
                   m->api_slot_size, m->mod_desc->func_api_size );
        snapshot_rollback( m, &prev, "api size changed" );
        return false;
    }

    if ( !state_ensure( m ) )
    {
        snapshot_rollback( m, &prev, "state alloc failed" );
        return false;
    }

    if ( !call_reload( m ) )
    {
        set_error( "'%s' on_reload returned false", m->name );
        snapshot_rollback( m, &prev, "on_reload failed" );
        return false;
    }

    /* Commit: publish the new func_api to consumers, then swap DLL and notify. */
    api_slot_refresh( m );

    if ( prev.dll )
    {
        /* post_exit for the OLD instance — pairs with its earlier exit(). */
        if ( g_post_exit_fn )
            g_post_exit_fn( m->name, prev.mod_desc, g_post_exit_user );
        sys_library_unload( prev.dll );
        shadow_delete( m->name, prev.shadow_count );
    }

    /* pre_init for the NEW instance — bracketing the reload() that just ran with the
       same hook the initial init() would have received. */
    if ( g_pre_init_fn )
        g_pre_init_fn( m->name, m->mod_desc, g_pre_init_user );

    m->version++;
    m->status = MODULE_STATUS_INITIALIZED;
    ms_log( "[module] reloaded '%s' (reload #%d)", m->name, m->version );
    return true;
}

/*==============================================================================================
    Dependency Resolution — iterative topological sort

    Repeatedly place any module whose declared dependencies are already in the order.
    A full pass without progress means either a missing dep (caught earlier) or a cycle.
==============================================================================================*/

static bool
build_init_order( void )
{
    bool placed[ MAX_MODULES ] = { false };
    g_init_count               = 0;

    /* empty slots are pre-placed so they get ignored */
    for ( int i = 0; i < MAX_MODULES; ++i )
        if ( g_modules[ i ].status == MODULE_STATUS_EMPTY )
            placed[ i ] = true;

    bool progress = true;
    while ( progress )
    {
        progress = false;

        for ( int i = 0; i < MAX_MODULES; ++i )
        {
            if ( placed[ i ] )
                continue;

            mod_info_t* m     = &g_modules[ i ];
            bool        ready = true;

            for ( int d = 0; d < m->mod_desc->dep_count && ready; ++d )
            {
                const char* dep_name = m->mod_desc->deps[ d ];
                if ( !dep_name )
                    continue;

                /* dep is satisfied iff it is already in g_init_order */
                bool found = false;
                for ( int k = 0; k < g_init_count; ++k )
                {
                    if ( strcmp( g_modules[ g_init_order[ k ] ].name, dep_name ) == 0 )
                    {
                        found = true;
                        break;
                    }
                }

                if ( !found )
                {
                    if ( slot_find( dep_name ) < 0 )
                    {
                        set_error( "'%s' depends on '%s' which is not registered", m->name, dep_name );
                        return false;
                    }
                    ready = false; /* dep exists, waiting for a future pass */
                }
            }

            if ( ready )
            {
                g_init_order[ g_init_count++ ] = i;
                placed[ i ]                    = true;
                progress                       = true;
            }
        }
    }

    /* any unplaced slot at this point is part of a cycle */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( !placed[ i ] )
        {
            set_error( "dependency cycle detected involving '%s'", g_modules[ i ].name );
            return false;
        }
    }

    ms_log( "[module] init order (%d):", g_init_count );
    for ( int i = 0; i < g_init_count; ++i ) ms_log( "  %d: %s", i, g_modules[ g_init_order[ i ] ].name );

    return true;
}

/*==============================================================================================
    Stale Shadow Cleanup

    Delete every *.tmp_*.dll left behind by a previous session that crashed before it could
    clean up. The ".tmp_" signature is narrow enough to never touch real build outputs.
==============================================================================================*/

typedef struct
{
    int deleted;
    int failed;
} shadow_cleanup_ctx_t;

static bool
shadow_cleanup_cb( const char* filename, const char* full_path, void* userdata )
{
    shadow_cleanup_ctx_t* ctx = ( shadow_cleanup_ctx_t* )userdata;

    /* The glob already filtered by pattern, but confirm the ".tmp_" signature 
       so a future path_shadow() format change can't silently widen the sweep 
       to unrelated files. */

    if ( !strstr( filename, ".tmp_" ) )
        return true;

    if ( sys_file_delete( full_path ) )
        ++ctx->deleted;
    else
    {
        ms_log( "[module] cleanup: could not delete stale shadow '%s'", filename );
        ++ctx->failed;
    }
    return true;
}

static void
shadow_cleanup( void )
{
    shadow_cleanup_ctx_t ctx = { 0, 0 };
    sys_file_glob( g_root, "*.tmp_*.dll", shadow_cleanup_cb, &ctx );

    if ( ctx.deleted > 0 || ctx.failed > 0 )
        ms_log( "[module] stale shadow cleanup: %d deleted, %d failed", ctx.deleted, ctx.failed );
}

// clang-format on
/*============================================================================================*/