/*==============================================================================================

    mod.c — Hot-reload module system implementation.

    Bootstrap allocator
    -------------------
    The module system uses plain malloc/free/printf from startup so it can operate before
    core exists. Once mod_init_all() runs core's init(), the system can be promoted to
    core's real allocator; for now it stays on the bootstrap path throughout.

    Shadow-copy strategy
    --------------------
    The original DLL is never loaded directly — the build tool would lock it and block
    recompilation. We copy it to a uniquely-named shadow file and load that instead.
    The counter increments on each reload so the new shadow never collides with the
    still-loaded previous copy.

        Original : <exe_dir>\render.dll
        Shadow 0 : <exe_dir>\render.tmp_0.dll
        Shadow 1 : <exe_dir>\render.tmp_1.dll  (shadow 0 deleted after unload)

    State ownership
    ---------------
    The system allocates state_size bytes from the bootstrap allocator and zeroes them on
    first load. State is preserved across hot-reloads and never freed until final unload.
    If state_size grows across a reload the tail bytes are zeroed; the block never shrinks.
    Modules must not store pointers into their own code or static data — those addresses
    become invalid after a DLL swap.

    Deferred reload and debounce
    ----------------------------
    mod_reload, mod_reload_all, and mod_check_reloads all enqueue into g_pending — they
    never perform a swap directly. The host calls mod_system_flush_reloads() once per frame
    at a quiescent point. Every entry waits DEBOUNCE_MS after the most recent file write
    before flushing, so a swap can never race the linker. Manual reload requests are
    queued the same way as file-watch entries; if the file is already stable they fire on
    the very next flush, otherwise they wait out the writer.

    Dependency order
    ----------------
    The init-order dependency graph is for initialization sequencing only. It does not
    define the per-frame execution order; hosts schedule module update calls explicitly.

    Registration tiers — intentionally distinct, do not collapse into a single macro.

    Tier 1 — HOST SERVICES   mod_static_load()
        Always compiled into the exe. Acquired by other modules via MOD_FETCH_API() in init().
            mod_static_load( "sys",  sys_get_mod_api() );
            mod_static_load( "core", core_get_mod_api() );

    Tier 2 — SWITCHABLE MODULES   mod_load()  (macro)
        Static in monolithic builds, hot-reloadable DLL in dynamic builds.
        BUILD_STATIC controls which path the macro expands to.
            mod_load( render );
            mod_load( audio );

    Tier 3 — FORCE-DYNAMIC   mod_dynamic_load()
        Must be a DLL regardless of BUILD_STATIC — scripting runtimes, platform plugins.
            mod_dynamic_load( "lua_runtime" );

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "orb.h"
#include "engine/sys/sys.h"

#include "mod_api.h"
#include "mod_export.h"
#include "mod.h"

/*==============================================================================================
    Constants
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
    Bootstrap log / alloc / free

    Used from startup until core's init() has run. Intentionally simple.
==============================================================================================*/

static void
ms_log( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    vprintf( fmt, ap );
    printf( "\n" );
    va_end( ap );
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
    Internal Types
==============================================================================================*/
// clang-format off

typedef struct mod_info_s
{
    char                name[ MODULE_NAME_MAX ];
    module_status_t     status;

    bool                is_static;                  /* true → no DLL, no shadow copies */
    uint32_t            shadow_count;               /* shadow file name counter for this slot */

    void*               dll;                        /* handle to the loaded shadow copy */
    uint64_t            last_write;                 /* file timestamp at last successful load */

    mod_api_t*          mod_api;                    /* lifecycle: init / tick / exit / on_reload */

    void*               state;                      /* persistent state block; system-owned */
    int32_t             state_size;                 /* size of the current allocation */

    void*               api_slot;                   /* stable address; system writes new func_api here on reload */
    int32_t             api_slot_size;              /* sizeof(func_api); fixed for the module's lifetime */

    int32_t             version;                    /* increments on each successful hot-reload */

} mod_info_t;

typedef struct
{
    char                name[ MODULE_NAME_MAX ];
    uint64_t            flagged_at_ms;              /* time (ms) when the change was first detected */
    uint64_t            last_seen_write;            /* file timestamp at flag time; reset if it changes again */

} pending_reload_t;

/*==============================================================================================
    Global State
==============================================================================================*/

static mod_info_t           g_modules[ MAX_MODULES ];
static int32_t              g_module_count = 0; 

static int32_t              g_init_order[ MAX_MODULES ];
static int32_t              g_init_count = 0;

static char                 g_root[ MAX_PATH ] = { 0 };
static char                 g_last_error[ 256 ] = { 0 };

static uint32_t             g_shadow_counter = 0;      /* global incremented per shadow copy created */

static get_api_fn           g_api_func;                /* passed into every init() / on_reload() */

static pending_reload_t     g_pending[ MAX_PENDING ];
static int32_t              g_pending_count = 0;

// clang-format on

/*==============================================================================================
    Error Reporting
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

const char* /* public */
mod_last_error( void )
{
    return g_last_error;
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

/*==============================================================================================
    Module Slot Management
==============================================================================================*/

static int
slot_find( const char* name )
{
    /* find id of module if previously registered */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( g_modules[ i ].status != MODULE_STATUS_EMPTY &&
             strncmp( g_modules[ i ].name, name, MODULE_NAME_MAX ) == 0 )
            return i;
    }
    return -1; /* not found */
}

static int
slot_alloc( void )
{
    /* allocated a free module slot */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( g_modules[ i ].status == MODULE_STATUS_EMPTY )
        {
            ++g_module_count;
            return i; /* found empty module slot */
        }
    }
    /* overflow - should never occur */
    assert( 0 && "module slots exhausted" );
    return -1;
}

static void
slot_free( int slot )
{
    /* free a module slot (caller must have already unloaded the DLL and cleaned up) */
    memset( &g_modules[ slot ], 0, sizeof( mod_info_t ) );
    --g_module_count;
}

/*==============================================================================================
    Shadow Copy Helpers
==============================================================================================*/

static bool
shadow_copy( const char* name, uint32_t counter )
{
    /* Copy <name>.dll > <name>.tmp_<counter>.dll.  Returns true on success. */

    char src[ MAX_PATH ], dst[ MAX_PATH ];
    path_dll( name, src, sizeof( src ) );
    path_shadow( name, counter, dst, sizeof( dst ) );
    if ( sys_file_copy( src, dst ) == false )
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
    State Helper: System-owned API slot

    Consumers cache the slot's address (returned by mod_get_api). On reload the system
    memcpys the new func_api into this same block — consumers automatically dispatch
    through the new pointers without any manual re-cache in their reload() callback.

    Static modules skip the copy: their func_api is already a stable linked-in const
    struct, so api_slot points directly at it.
==============================================================================================*/

static bool
api_slot_create( mod_info_t* m )
{
    int32_t size = m->mod_api->func_api_size;
    if ( size <= 0 )
    {
        set_error( "'%s': func_api_size is zero", m->name );
        return false;
    }
    if ( m->mod_api->func_api == NULL )
    {
        set_error( "'%s': func_api is NULL", m->name );
        return false;
    }

    if ( m->is_static )
    {
        /* The const struct already has a stable address — point at it directly. */
        m->api_slot      = ( void* )m->mod_api->func_api;
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
    memcpy( m->api_slot, m->mod_api->func_api, ( size_t )size );
    return true;
}

/* Copy the new DLL's func_api into the existing stable slot.
   Caller must have verified func_api_size still matches m->api_slot_size. */
static void
api_slot_refresh( mod_info_t* m )
{
    memcpy( m->api_slot, m->mod_api->func_api, ( size_t )m->api_slot_size );
}

/*==============================================================================================
    API struct validation

    Contract: func_api is a struct of function pointers only. func_api_size is therefore
    an exact multiple of sizeof(void*), and each void*-sized slot must be a non-NULL function
    pointer after a successful load or reload. Consumers cache the api_slot address and
    dispatch through it without per-call NULL checks, so any NULL slot is a host crash
    waiting to happen.

    Runs on every load and every reload.
==============================================================================================*/

static bool
api_validate_func( const char* name, const void* func_api, int32_t func_api_size )
{
    if ( func_api == NULL )
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
        if ( slots[ i ] == NULL )
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
    State Helper — Allocate or grow the persistent state block.

    Zeroes the block on first allocation. On reload, preserves existing data and
    zeroes only the newly added tail if state_size grew. Never shrinks the allocation.
==============================================================================================*/

static bool
state_ensure( mod_info_t* m )
{
    int32_t required = m->mod_api->state_size;
    if ( required <= 0 )
    {
        return true; /* stateless module */
    }

    if ( m->state == NULL )
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
        /* state grew across a reload — realloc and zero the new region */
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
    /* if required < state_size: keep the larger allocation, never shrink */

    return true;
}

/*==============================================================================================
    Low-level DLL load / unload  (does not call init/exit or touch state)
==============================================================================================*/

static bool
slot_load_dll( mod_info_t* m )
{
    /* Load the DLL for slot, resolve API.  State must already be allocated (or NULL). */

    /* --- shadow copy --- */

    m->shadow_count = g_shadow_counter++;
    if ( !shadow_copy( m->name, m->shadow_count ) )
        return false;

    /* --- load shadow DLL --- */

    char shadow[ MAX_PATH ];
    path_shadow( m->name, m->shadow_count, shadow, sizeof( shadow ) );

    m->dll = sys_library_load( shadow ); /* platform: LoadLibraryA(full_path) */
    if ( !m->dll )
    {
        set_error( "LoadLibrary failed: %s", shadow );
        shadow_delete( m->name, m->shadow_count );
        return false;
    }

    /* --- resolve module lifecycle struct --- */

    get_mod_api_fn get_mod_api = ( get_mod_api_fn )sys_library_get_symbol( m->dll, "get_mod_api" );
    if ( get_mod_api == NULL )
    {
        set_error( "'%s' is missing 'get_mod_api' export", m->name );
        goto fail;
    }
    m->mod_api = get_mod_api();
    if ( m->mod_api == NULL )
    {
        set_error( "'%s' get_mod_api() returned NULL", m->name );
        goto fail;
    }
    if ( m->mod_api->func_api == NULL )
    {
        set_error( "'%s': mod_api_t::func_api is NULL", m->name );
        goto fail;
    }
    if ( !api_validate_func( m->name, m->mod_api->func_api, m->mod_api->func_api_size ) )
    {
        goto fail;
    }

    /* --- record file-time for hot-reload detection --- */

    char dll_path[ MAX_PATH ];
    path_dll( m->name, dll_path, sizeof( dll_path ) );
    m->last_write = sys_file_time( dll_path );

    return true; /* success */

fail:

    /* -- cleanup on failure --- */

    sys_library_unload( m->dll );
    m->dll     = NULL;
    m->mod_api = NULL;
    shadow_delete( m->name, m->shadow_count );

    return false; /* failure */
}

static void
slot_unload_dll( mod_info_t* m )
{
    /* Unload the DLL for slot, delete shadow copy.  Does NOT free state or call exit(). */

    if ( !m->dll )
        return;

    sys_library_unload( m->dll );
    shadow_delete( m->name, m->shadow_count );

    m->dll     = NULL;
    m->mod_api = NULL;
}

/*==============================================================================================
    Lifecycle call helpers  (all use g_get_api so DLLs can call back into the system)
==============================================================================================*/

/* called by module_init_all and mod_reload — one call site each */

static bool
call_init( mod_info_t* m )
{
    if ( !m->mod_api || !m->mod_api->init )
        return true; /* no init callback — fine */

    return m->mod_api->init( m->state, g_api_func );
}

static void
call_exit( mod_info_t* m )
{
    if ( !m->mod_api || !m->mod_api->exit )
        return;

    m->mod_api->exit( m->state );
}

static bool
call_reload( mod_info_t* m )
{
    if ( !m->mod_api )
        return true;

    if ( m->mod_api->reload )
    {
        return m->mod_api->reload( m->state, g_api_func );
    }

    ms_log( "[module] '%s' has no on_reload(), dynamic reload requires it", m->name );
    return false;
}

/*==============================================================================================
    Reload snapshot — captures the live DLL handles so a failed reload can put them back
==============================================================================================*/

typedef struct mod_snapshot_s
{
    void*           dll;          /* handle to the loaded shadow copy, if any */
    mod_api_t*      mod_api;      /* pointer to the module's API struct, if loaded and resolved */
    uint32_t        shadow_count; /* shadow file name counter for this slot, if any */
    uint64_t        last_write;   /* file timestamp at last successful load, for hot-reload detection */
    module_status_t status;       /* status at the moment of the snapshot */

} mod_snapshot_t;

static void
snapshot_save( const mod_info_t* m, mod_snapshot_t* s )
{
    s->dll          = m->dll;
    s->mod_api      = m->mod_api;
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
    m->mod_api      = s->mod_api;
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
    Public: API accessor
==============================================================================================*/

const void* /* public */
mod_get_api( const char* name )
{
    int slot = slot_find( name );
    if ( slot < 0 )
    {
        ms_log( "[module] get_api(\"%s\"): module not registered", name );
        return NULL;
    }

    mod_info_t* m = &g_modules[ slot ];
    if ( m->status != MODULE_STATUS_INITIALIZED )
    {
        ms_log( "[module] get_api(\"%s\"): module exists but is not yet initialized (status %d)", name, m->status );
        return NULL;
    }

    return m->api_slot; /* stable across reloads old: m->mod_api->func_api; */
}

/*==============================================================================================
    Public: Module Functions
==============================================================================================*/

bool /* public */
mod_static_load( const char* name, mod_api_t* mod_api )
{
    /* Register a statically-linked module (no DLL). */

    assert( mod_api != NULL );
    assert( mod_api->func_api != NULL && "'func_api' must not be NULL in a static module" );

    if ( slot_find( name ) >= 0 )
    {
        ms_log( "[module] static '%s' already registered.\n", name );
        return true;
    }

    int slot = slot_alloc();
    if ( slot < 0 )
    {
        set_error( "no free module slots" );
        return false;
    }

    mod_info_t* m = &g_modules[ slot ];
    memset( m, 0, sizeof( *m ) );
    strncpy( m->name, name, MODULE_NAME_MAX - 1 );
    m->name[ MODULE_NAME_MAX - 1 ] = '\0';
    m->is_static                   = true;
    m->mod_api                     = mod_api;
    m->status                      = MODULE_STATUS_LOADED;

    if ( !api_validate_func( name, mod_api->func_api, mod_api->func_api_size ) )
    {
        slot_free( slot );
        return false;
    }
    if ( state_ensure( m ) == false )
    {
        slot_free( slot );
        return false;
    }
    if ( api_slot_create( m ) == false )
    {
        if ( m->state )
        {
            ms_free( m->state );
            m->state = NULL;
        }
        slot_free( slot );
        return false;
    }

    ms_log( "[module] registered static '%s' (api v%d, state %d, api %d)", name, mod_api->version,
            mod_api->state_size, mod_api->func_api_size );
    return true;
}

/*============================================================================================*/

bool /* public */
mod_dynamic_load( const char* name )
{
    /* Register and load a module by name.  Returns true on success.  On failure the module is not
       registered and an error message is available via module_last_error(). */

    if ( slot_find( name ) >= 0 )
    {
        ms_log( "[module] '%s' already loaded.", name );
        return true;
    }

    int slot = slot_alloc();
    if ( slot < 0 )
    {
        set_error( "no free module slots" );
        return false;
    }

    mod_info_t* m = &g_modules[ slot ];
    memset( m, 0, sizeof( *m ) );
    strncpy( m->name, name, MODULE_NAME_MAX - 1 );
    m->name[ MODULE_NAME_MAX - 1 ] = '\0';
    m->status                      = MODULE_STATUS_ERROR; /* provisional until success */

    if ( slot_load_dll( m ) == false )
    {
        slot_free( slot );
        return false;
    }
    if ( state_ensure( m ) == false )
    {
        slot_unload_dll( m );
        slot_free( slot );
        return false;
    }
    if ( api_slot_create( m ) == false )
    {
        if ( m->state )
        {
            ms_free( m->state );
            m->state = NULL;
        }
        slot_unload_dll( m );
        slot_free( slot );
        return false;
    }

    m->status  = MODULE_STATUS_LOADED;
    m->version = 0;
    ms_log( "[module] loaded '%s' (api v%d, state %d, api %d)", name, m->mod_api->version,
            m->mod_api->state_size, m->mod_api->func_api_size );

    return true;
}

/*============================================================================================*/

bool /* public */
mod_unload( const char* name )
{
    /* Unload a module by name, remains registered but inactive */

    int slot = slot_find( name );
    if ( slot < 0 )
    {
        set_error( "mod_unload: '%s' not found", name );
        return false;
    }

    mod_info_t* m = &g_modules[ slot ];

    if ( m->status == MODULE_STATUS_INITIALIZED )
        call_exit( m );

    if ( m->state != NULL )
    {
        ms_free( m->state );
        m->state = NULL;
    }

    if ( m->is_static == false && m->api_slot != NULL )
    {
        ms_free( m->api_slot );
        m->api_slot = NULL;
    }

    if ( m->is_static == false )
    {
        slot_unload_dll( m );
    }

    ms_log( "[module] unloaded '%s'", name );
    slot_free( slot );
    return true;
}

/*==============================================================================================
    do_reload — Internal swap. Called only from mod_system_flush_reloads().
==============================================================================================*/

static bool
do_reload( mod_info_t* m )
{
    /* (body of the old mod_reload, starting from `if ( m->is_static )`,
       with `name` replaced by `m->name`) */

    if ( m->is_static )
    {
        ms_log( "[module] '%s' is static — reload is a no-op", m->name );
        return true;
    }

    mod_snapshot_t prev;
    snapshot_save( m, &prev );

    if ( m->status == MODULE_STATUS_INITIALIZED )
        call_exit( m );

    m->dll     = NULL;
    m->mod_api = NULL;

    if ( !slot_load_dll( m ) )
    {
        snapshot_rollback( m, &prev, "load failed" );
        return false;
    }

    if ( m->mod_api && m->mod_api->func_api_size != m->api_slot_size )
    {
        set_error( "'%s': func_api_size changed across reload (%d -> %d); requires full restart", m->name,
                   m->api_slot_size, m->mod_api->func_api_size );
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

    api_slot_refresh( m );

    if ( prev.dll )
    {
        sys_library_unload( prev.dll );
        char path[ MAX_PATH ];
        path_shadow( m->name, prev.shadow_count, path, sizeof( path ) );
        sys_file_delete( path );
    }

    m->version++;
    m->status = MODULE_STATUS_INITIALIZED;
    ms_log( "[module] reloaded '%s' (reload #%d)", m->name, m->version );
    return true;
}

/*==============================================================================================
    Debounce helpers
==============================================================================================*/

static pending_reload_t*
pending_find( const char* name )
{
    /* Find a pending reload entry by name, or return NULL if not found. */
    for ( int i = 0; i < g_pending_count; ++i )
    {
        if ( strncmp( g_pending[ i ].name, name, MODULE_NAME_MAX ) == 0 )
            return &g_pending[ i ];
    }
    return NULL;
}

static void
pending_flag( const char* name, uint64_t now_ms, uint64_t file_time )
{
    pending_reload_t* p = pending_find( name );

    if ( p )
    {
        /* File moved again before we got to flush — restart the debounce. */
        if ( file_time != p->last_seen_write )
        {
            p->flagged_at_ms   = now_ms;
            p->last_seen_write = file_time;
        }
        return;
    }

    if ( g_pending_count >= MAX_PENDING )
    {
        ms_log( "[module] WARN: pending reload table full, skipping '%s'", name );
        return;
    }

    pending_reload_t* slot = &g_pending[ g_pending_count++ ];
    strncpy( slot->name, name, MODULE_NAME_MAX - 1 );
    slot->name[ MODULE_NAME_MAX - 1 ] = '\0';
    slot->flagged_at_ms               = now_ms;
    slot->last_seen_write             = file_time;

    ms_log( "[module] '%s' queued for reload (%d ms debounce)", name, DEBOUNCE_MS );
}

static void
pending_remove( int idx )
{
    g_pending[ idx ] = g_pending[ --g_pending_count ];
}

bool /* public */
mod_reload( const char* name )
{
    int slot = slot_find( name );
    if ( slot < 0 )
    {
        set_error( "mod_reload: '%s' not registered", name );
        return false;
    }
    if ( g_modules[ slot ].is_static )
    {
        ms_log( "[module] '%s' is static — reload is a no-op", name );
        return true;
    }

    char dll_path[ MAX_PATH ];
    path_dll( name, dll_path, sizeof( dll_path ) );
    pending_flag( name, sys_time_ms(), sys_file_time( dll_path ) );
    return true;
}

/*==============================================================================================

    : Force-reload every dynamic module immediately, skips the file-watch debounce.

==============================================================================================*/

int /* public */
mod_reload_all( void )
{
    uint64_t now_ms = sys_time_ms();
    int      queued = 0;

    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY || m->is_static )
            continue;

        char dll_path[ MAX_PATH ];
        path_dll( m->name, dll_path, sizeof( dll_path ) );
        pending_flag( m->name, now_ms, sys_file_time( dll_path ) );
        ++queued;
    }

    ms_log( "[module] queued %d module(s) for reload", queued );
    return queued;
}

/*==============================================================================================

    : Called by the host's main loop once per frame. Checks the file timestamps of all loaded

==============================================================================================*/

void /* public */
mod_check_reloads( void )
{
    uint64_t now_ms = sys_time_ms();

    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        /* skip empty slots and static modules — only dynamic modules can be reloaded based on file changes */
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY || m->is_static )
            continue;

        /* Check the file timestamp of the original DLL (not the shadow) to detect changes. */
        char dll_path[ MAX_PATH ];
        path_dll( m->name, dll_path, sizeof( dll_path ) );
        uint64_t ft = sys_file_time( dll_path );

        /* If the file timestamp has changed since the last successful load, flag for reload. */
        if ( ft != 0 && ft != m->last_write )
        {
            pending_flag( m->name, now_ms, ft );
        }
    }
}

/*==============================================================================================

    : Called by the host's main loop after mod_check_reloads.
    : Walks the pending reloads and calls do_reload() for each one that is ready.
    : Returns the number of modules successfully reloaded.

==============================================================================================*/

/*==============================================================================================
    : Called by the host's main loop after mod_check_reloads.
    : Collects every pending entry whose debounce has expired (or that was forced),
    : sorts them by their position in g_init_order, and reloads in that dependency
    : order. This guarantees that when a shared-header rebuild flags both 'render'
    : and 'game' at once, 'render' (lower init rank) reloads first, so 'game's
    : reload() sees the new render_api in place.
    : Returns the number of modules successfully reloaded.
==============================================================================================*/

int /* public */
mod_system_flush_reloads( void )
{
    if ( g_pending_count == 0 )
        return 0;

    uint64_t now_ms = sys_time_ms();

    /* --- Step 1: collect ready entries with their dependency rank --- */

    typedef struct
    {
        int pending_idx; /* index into g_pending */
        int slot;        /* index into g_modules, or -1 if deregistered since queueing */
        int dep_rank;    /* position in g_init_order; g_init_count means "unranked" */
    } ready_entry_t;

    ready_entry_t ready[ MAX_PENDING ];
    int           ready_count = 0;

    for ( int i = 0; i < g_pending_count; ++i )
    {
        pending_reload_t* p = &g_pending[ i ];

        /* Wait out the debounce on every entry — manual reloads land here too. */
        if ( ( now_ms - p->flagged_at_ms ) < DEBOUNCE_MS )
            continue;

        int slot = slot_find( p->name );

        /* Default rank sorts after every dependency-ordered module. Covers
           the LOADED-but-not-yet-INITIALIZED case and any deregistered name. */

        int rank = g_init_count;
        if ( slot >= 0 )
        {
            for ( int k = 0; k < g_init_count; ++k )
            {
                if ( g_init_order[ k ] == slot )
                {
                    rank = k;
                    break;
                }
            }
        }

        ready[ ready_count ].pending_idx = i;
        ready[ ready_count ].slot        = slot;
        ready[ ready_count ].dep_rank    = rank;
        ++ready_count;
    }

    if ( ready_count == 0 )
        return 0;

    /* --- Step 2: sort by dep_rank ascending (insertion sort; ready_count <= 16) --- */

    for ( int i = 1; i < ready_count; ++i )
    {
        ready_entry_t key = ready[ i ];
        int           j   = i - 1;
        while ( j >= 0 && ready[ j ].dep_rank > key.dep_rank )
        {
            ready[ j + 1 ] = ready[ j ];
            --j;
        }
        ready[ j + 1 ] = key;
    }

    /* --- Step 3: reload in dependency order --- */

    int reloaded = 0;
    for ( int i = 0; i < ready_count; ++i )
    {
        if ( ready[ i ].slot < 0 )
            continue; /* module unregistered between queue and flush */

        if ( do_reload( &g_modules[ ready[ i ].slot ] ) )
            ++reloaded;
    }

    /* --- Step 4: compact g_pending, dropping the entries we just processed ---
       Failed reloads are removed too, matching the original behaviour. */

    bool processed[ MAX_PENDING ] = { false };
    for ( int i = 0; i < ready_count; ++i ) processed[ ready[ i ].pending_idx ] = true;

    int write = 0;
    for ( int read = 0; read < g_pending_count; ++read )
    {
        if ( !processed[ read ] )
        {
            if ( write != read )
                g_pending[ write ] = g_pending[ read ];
            ++write;
        }
    }
    g_pending_count = write;

    return reloaded;
}

/*==============================================================================================
    Dependency resolution — simple iterative topological sort

    The algorithm works like this:

    Start with no modules placed in the init order.
    Repeatedly scan all modules.
        If a module’s dependencies are already in the init order, place that module.
        Keep doing that until no more progress can be made.
    If any modules are still unplaced:
        either a dependency was missing
        or there is a circular dependency

==============================================================================================*/

static bool
build_init_order( void )
{
    bool placed[ MAX_MODULES ] = { false };
    g_init_count               = 0;

    /* mark empty slots as already placed so they are ignored */
    for ( int i = 0; i < MAX_MODULES; ++i )
        if ( g_modules[ i ].status == MODULE_STATUS_EMPTY )
            placed[ i ] = true;

    /* progress is used to detect whether we successfully placed
       at least one module during a full pass over the array.
       If we do a full pass and place nothing, the loop stops. */

    bool progress = true;
    while ( progress )
    {
        progress = false; /* no progress until we place something */

        /* scan every module slot looking for modules whose
           dependencies are already satisfied -- can be placed */

        for ( int i = 0; i < MAX_MODULES; ++i )
        {
            /* skip modules already placed, and skip empty slots. */
            if ( placed[ i ] )
                continue;

            mod_info_t* m = &g_modules[ i ];

            /* assume this module is ready to be placed unless we
               discover a dependency that is not ready yet. */
            bool ready = true;

            /* Check each declared dependency of this module. */
            for ( int d = 0; d < m->mod_api->dep_count; ++d )
            {
                const char* dep_name = m->mod_api->deps[ d ];

                /* ignore null strings in list */
                if ( !dep_name )
                    continue;

                bool found = false;

                /* search only the modules already added to g_init_order[0 .. g_init_count-1].
                   If the dependency is already there, then this dependency is satisfied. */

                for ( int k = 0; k < g_init_count; ++k )
                {
                    if ( strcmp( g_modules[ g_init_order[ k ] ].name, dep_name ) == 0 )
                    {
                        found = true;
                        break;
                    }
                }

                /* Dependency module not yet placed — also check it was even registered. */

                if ( !found )
                {
                    /* First ensure check if module was even added all. */
                    if ( slot_find( dep_name ) < 0 )
                    {
                        set_error( "'%s' depends on '%s' which is not registered", m->name, dep_name );
                        return false;
                    }

                    /* Dependency exists, but has not been placed yet (cannot be placed during this pass). */
                    ready = false;
                    break;
                }
            }

            /* If every dependency was already placed, then this module is now safe to
               initialize after them, so add it to the init order. */

            if ( ready )
            {
                g_init_order[ g_init_count++ ] = i;
                placed[ i ]                    = true;
                progress                       = true;
            }
        }
    }

    /* Any unplaced slot means a dependency cycle */
    /* After the loop finishes, if any slot is still unplaced, then
       we were unable to resolve all dependencies.

       Since missing dependencies were already caught earlier, the
       remaining cause is a dependency cycle, such as:

            A depends on B and B depends on A
            Or a longer cycle: A -> B -> C -> A
    */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( placed[ i ] == false )
        {
            set_error( "dependency cycle detected involving '%s'", g_modules[ i ].name );
            return false;
        }
    }

    /* debug: print resolved initialization order */
    ms_log( "[module] init order (%d):", g_init_count );
    for ( int i = 0; i < g_init_count; ++i )
    {
        int slot = g_init_order[ i ];
        ms_log( "  %d: %s", i, g_modules[ slot ].name );
    }

    return true;
}

/*==============================================================================================
    Public: init / tick / reload / exit
==============================================================================================*/

bool /* public */
mod_init_all( void )
{
    /* Call init() on all loaded modules in dependency order. Returns true on success. */

    if ( build_init_order() == false )
        return false;

    for ( int k = 0; k < g_init_count; ++k )
    {
        mod_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status != MODULE_STATUS_LOADED )
            continue;

        if ( call_init( m ) == false )
        {
            set_error( "'%s' init() returned false", m->name );
            m->status = MODULE_STATUS_ERROR;
            return false;
        }

        m->status = MODULE_STATUS_INITIALIZED;
        ms_log( "[module] initialized '%s'\n", m->name );
    }

    return true;
}

/*==============================================================================================
    System init / exit
==============================================================================================*/
/* Delete every *.tmp_*.dll left behind by a previous session that crashed before it could
   clean up its shadow copies.  Called once at startup, before any module is loaded.

   The pattern we look for is any file whose name contains ".tmp_" and ends with ".dll".
   That is specific enough to never touch real build outputs, and it matches exactly the
   names produced by path_shadow() — "<name>.tmp_<counter>.dll".

   Runs best-effort: individual delete failures are logged but do not abort startup. */

typedef struct
{
    int deleted;
    int failed;

} shadow_cleanup_ctx_t;

static bool
shadow_cleanup_cb( const char* filename, const char* full_path, void* userdata )
{
    shadow_cleanup_ctx_t* ctx = ( shadow_cleanup_ctx_t* )userdata;

    /* Belt-and-suspenders: the glob already filtered by pattern, but confirm the
       ".tmp_" signature so a future path_shadow() format change can't silently
       widen the sweep to unrelated files. */

    if ( !strstr( filename, ".tmp_" ) )
        return true;

    if ( sys_file_delete( full_path ) )
        ++ctx->deleted;
    else
    {
        ms_log( "[module] cleanup: could not delete stale shadow '%s'", filename );
        ++ctx->failed;
    }
    return true; /* keep iterating */
}

static void
cleanup_stale_shadows( void )
{
    shadow_cleanup_ctx_t ctx = { 0, 0 };
    sys_file_glob( g_root, "*.tmp_*.dll", shadow_cleanup_cb, &ctx );

    if ( ctx.deleted > 0 || ctx.failed > 0 )
        ms_log( "[module] stale shadow cleanup: %d deleted, %d failed", ctx.deleted, ctx.failed );
}

/*==============================================================================================
    System init / exit
==============================================================================================*/

void /* public */
mod_system_init()
{
    /* zero module slots (status = EMPTY) */
    memset( g_modules, 0, sizeof( g_modules ) );
    memset( g_pending, 0, sizeof( g_pending ) );
    memset( g_last_error, 0, sizeof( g_last_error ) );

    g_module_count    = 0;
    g_init_count      = 0;
    g_shadow_counter  = 0;
    g_pending_count   = 0;

    g_last_error[ 0 ] = '\0';

    /* wire up the get_api passed into every module init() */
    g_api_func = mod_get_api;

    /* set root path for module DLLs */
    sys_exe_dir( g_root, sizeof( g_root ) );
    ms_log( "[module] system init (root: %s)", g_root );

    /* purge shadow copies left over from any previous session that crashed */
    cleanup_stale_shadows();

    /* start file watcher via sys (no core needed) */
    if ( sys_filewatch_init( g_root ) == false )
    {
        ms_log( "[module] WARNING: file watch unavailable\n" );
    }
}

/*============================================================================================*/

void
mod_system_exit( void )
{
    sys_filewatch_shutdown();

    /* exit in reverse init order so dependencies outlive their dependents */
    for ( int k = g_init_count - 1; k >= 0; --k )
    {
        mod_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status != MODULE_STATUS_INITIALIZED )
            continue;

        call_exit( m );

        /* LOADED but not INITIALIZED anymore since we called exit() */
        m->status = MODULE_STATUS_LOADED;
        ms_log( "[module] exited '%s'", m->name );
    }

    /* unload DLLs and free state */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

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
    }

    memset( g_modules, 0, sizeof( g_modules ) );
    g_module_count  = 0;
    g_init_count    = 0;
    g_pending_count = 0;

    ms_log( "[module] system shutdown complete" );
}

/*==============================================================================================
    Debug
==============================================================================================*/

void
mod_list_all( void )
{
    static const char* status_str[] = { "EMPTY", "LOADED", "INITIALIZED", "ERROR" };

    ms_log( "---- modules (%d / %d) ----\n", g_module_count, MAX_MODULES );
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

        ms_log( "  [%2d]  %-24s  %-13s  api_v%-3d  reload# %-3d  state:%dB  %s", i, m->name,
                status_str[ m->status ], m->mod_api ? m->mod_api->version : 0, m->version, m->state_size,
                m->is_static ? "(static)" : "" );
    }
    ms_log( "\n" );
}

/*============================================================================================*/
