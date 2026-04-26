/*==============================================================================================

    module_sys.c

    Hot-reload module system implementation.

    Bootstrap
    --------------------
    The module system starts on its own bootstrap log/alloc so it can function
    before core exists.  Once module_init_all() runs core's init(), the system
    promotes itself to core's real implementations silently.

    Shadow-copy strategy
    --------------------
    The original DLL is never loaded directly — the build tool would lock it and
    prevent recompilation.  Instead we copy it to a uniquely named shadow file and
    load that.  The counter increments on each reload so there is never a name
    collision with the still-loaded previous copy.

        Original : <exe_dir>\render.dll
        Shadow 0 : <exe_dir>\render.tmp_0.dll
        Shadow 1 : <exe_dir>\render.tmp_1.dll   (shadow 0 deleted after unload)

    State ownership
    ---------------
    The system allocates `state` from the core allocator using module_api_t::state_size.
    The pointer is passed into every init/tick/exit/on_reload call and is NEVER freed
    across a hot-reload, so modules keep their runtime data through code changes.
    On first load the block is zeroed; on reload it arrives with its previous values.

    File watching
    -------------
    module_check_reloads() delegates all platform I/O to the file_watch interface.
    A debounce window (DEBOUNCE_MS) prevents reloading while the linker is still
    writing — the module is flagged on the first notification and reloaded only
    after DEBOUNCE_MS milliseconds have elapsed with no further notifications.

    Example usage (remove in production)

    module system

    Startup sequence:
        1. module_system_init()       — boot (no modules yet)
        2. module_register_static()   — core + engine (already running in the exe)
        3. module_dynamic_load()      — dynamic DLLs
        4. module_init_all()          — topo-sort deps → call every init()
        5. main loop                  — tick + hot-reload polling
        6. module_system_exit()       — reverse-order exit + DLL unload + shadow cleanup

     Depends on platform_sys (dll load, file watch, clock)

==============================================================================================*/

#include <stdio.h>     // snprintf
#include <stdlib.h>    // malloc, free
#include <stdarg.h>    // va_list, va_start, etc.
#include <assert.h>    // assert
#include <string.h>

#include "orb.h"
#include "base/base.h" /* types, arena, str — zero deps  */
#include "platform_sys/platform_sys.h"

#include "core/core_api.h"

#include "module_api.h"
#include "module.h"

/*==============================================================================================
    Platform layer
==============================================================================================*/

#define MAX_PATH 260

/*==============================================================================================
    Bootstrap implementations: Used only from module_system_init() until core's init() runs.
    After that g_fn.log / g_fn.alloc / g_fn.free point at core's real versions.
==============================================================================================*/

/* internal allocation — module_sys.c only */
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
    Internal state
==============================================================================================*/

#define MAX_MODULES 16

static module_info_t    g_modules[ MAX_MODULES ];
static int32_t          g_module_count = 0;

static int32_t          g_init_order[ MAX_MODULES ];
static int32_t          g_init_count        = 0;

static char             g_root[ MAX_PATH ]  = { 0 };
static char             g_last_error[ 256 ] = { 0 };

static uint32_t         g_shadow_counter    = 0; /* global incremented per shadow copy created */

static module_sys_api_t g_sys_api;

/* Passed into every init() / on_reload() call so DLLs can resolve APIs
   without linking against the exe. Defined after module_get_api(). */

#define DEBOUNCE_MS 150
#define MAX_PENDING 16

/* Debounce table for hot-reload */
typedef struct
{
    char     name[ MODULE_NAME_MAX ];
    uint64_t flagged_at_ms;

    /* time of first notification; if current time - flagged_at_ms > DEBOUNCE_MS,
       we reload and clear the flag */

} pending_reload_t;

static pending_reload_t g_pending[ MAX_PENDING ];
static int32_t          g_pending_count = 0;

/*==============================================================================================
    Error reporting (uses g_fn.log so it works in both bootstrap and promoted state)
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
module_last_error( void )
{
    return g_last_error;
}

/*==============================================================================================
    Path helpers
==============================================================================================*/

static void
path_dll( const char* name, char* out, size_t size )
{
    /* "<root>\<name>.dll" */
    snprintf( out, size, "%s\\%s.dll", g_root, name );
}

static void
path_shadow( const char* name, uint32_t counter, char* out, size_t size )
{
    /* "<root>\<name>.tmp_<counter>.dll" */
    snprintf( out, size, "%s\\%s.tmp_%u.dll", g_root, name, counter );
}

/*==============================================================================================
    Slot management
==============================================================================================*/

static int
slot_find( const char* name )
{
    /* find id of module if registered */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        bool is_empty = g_modules[ i ].status == MODULE_STATUS_EMPTY;
        bool is_equal = strcmp( g_modules[ i ].name, name ) == 0;
        if ( is_empty == false && is_equal == true )
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
    memset( &g_modules[ slot ], 0, sizeof( module_info_t ) );
    --g_module_count;
}

/*==============================================================================================
    Shadow-copy helpers
==============================================================================================*/

/* Copy <name>.dll → <name>.tmp_<counter>.dll.  Returns true on success. */
static bool
shadow_copy( const char* name, uint32_t counter )
{
    char src[ MAX_PATH ], dst[ MAX_PATH ];
    path_dll( name, src, sizeof( src ) );
    path_shadow( name, counter, dst, sizeof( dst ) );

    if ( platform_copy_file( src, dst ) == false )
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
    platform_delete_file( path ); /* best-effort; ignore failure */
}

/*==============================================================================================
    State helpers
==============================================================================================*/

static bool
state_ensure( module_info_t* m )
{
    /* Allocate (or reallocate) a module's persistent state block.
       On first call `m->state` is NULL. On reload we preserve the pointer and
       only realloc if the new api declares a larger state_size. */

    int32_t required = m->module_api->state_size;
    if ( required <= 0 )
    {
        /* stateless module — nothing to do */
        return true;
    }

    if ( m->state == NULL )
    {
        /* first load — allocate and zero */
        m->state      = ms_alloc( ( size_t )required );
        m->state_size = required;
        if ( !m->state )
        {
            set_error( "'%s': state alloc failed (%d bytes)", m->name, required );
            return false;
        }
        memset( m->state, 0, ( size_t )required );
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
slot_load_dll( module_info_t* m )
{
    /* Load the DLL for slot, resolve API.  State must already be allocated (or NULL). */

    const char* name = m->name;

    /* --- shadow copy --- */
    m->shadow_count = g_shadow_counter++;
    if ( !shadow_copy( name, m->shadow_count ) )
        return false;

    /* --- load shadow --- */
    char shadow[ MAX_PATH ];
    path_shadow( m->name, m->shadow_count, shadow, sizeof( shadow ) );

    m->dll = library_load( shadow ); /* platform: LoadLibraryA(full_path) */
    if ( !m->dll )
    {
        set_error( "LoadLibrary failed: %s", shadow );
        shadow_delete( m->name, m->shadow_count );
        return false;
    }

    /* --- resolve lifecycle struct --- */
    get_module_api_fn get_module_api = ( get_module_api_fn )library_get_symbol( m->dll, "get_module_api" );
    if ( !get_module_api )
    {
        set_error( "'%s' is missing 'get_module_api' export", name );
        goto fail;
    }
    m->module_api = get_module_api();
    if ( !m->module_api )
    {
        set_error( "'%s' get_module_api() returned NULL", name );
        goto fail;
    }

    /* --- resolve typed API struct --- */
    get_api_fn get_api = ( get_api_fn )library_get_symbol( m->dll, "get_api" );
    if ( !get_api )
    {
        set_error( "'%s' is missing 'get_api' export", name );
        goto fail;
    }
    m->exported_api = get_api();
    if ( !m->exported_api )
    {
        set_error( "'%s' get_api() returned NULL", name );
        goto fail;
    }

    /* --- record file-time for hot-reload detection --- */
    char dll_path[ MAX_PATH ];
    path_dll( name, dll_path, sizeof( dll_path ) );
    m->last_write = platform_file_time( dll_path );

    return true;

fail:

    /* -- cleanup on failure --- */
    library_unload( m->dll );
    m->dll        = NULL;
    m->module_api = NULL;
    shadow_delete( name, m->shadow_count );
    return false;
}

static void
slot_unload_dll( module_info_t* m )
{
    /* Unload the DLL for slot, delete shadow copy.  Does NOT free state or call exit(). */

    if ( !m->dll )
        return;

    library_unload( m->dll );
    shadow_delete( m->name, m->shadow_count );

    m->dll          = NULL;
    m->module_api   = NULL;
    m->exported_api = NULL;
}

/*==============================================================================================
    Lifecycle call helpers  (all use g_get_api so DLLs can call back into the system)
==============================================================================================*/

/* called by module_init_all and module_reload — one call site each */

static bool
call_init( module_info_t* m )
{
    if ( !m->module_api || !m->module_api->init )
        return true;

    return m->module_api->init( m->state, &g_sys_api );
}

static void
call_tick( module_info_t* m, float dt )
{
    if ( !m->module_api || !m->module_api->tick )
        return;

    m->module_api->tick( m->state, dt );
}

static void
call_exit( module_info_t* m )
{
    if ( !m->module_api || !m->module_api->exit )
        return;

    m->module_api->exit( m->state );
}

static void
call_on_reload( module_info_t* m )
{
    if ( !m->module_api || !m->module_api->on_reload )
        return;

    m->module_api->on_reload( m->state, &g_sys_api );
}

/*==============================================================================================
    Public: API accessor
==============================================================================================*/

void*
module_get_api( const char* name )
{
    /* Returns the typed API pointer exported by the module, or NULL if not found or not initialized.
      Caller casts to the expected type, e.g. (core_api_t*), (render_api_t*), etc. */

    int slot = slot_find( name );
    if ( slot < 0 )
        return NULL;

    module_info_t* m = &g_modules[ slot ];
    if ( m->status != MODULE_STATUS_INITIALIZED )
        return NULL;

    return m->exported_api;
}

/*==============================================================================================
    Public: load / register / unload / reload
==============================================================================================*/

bool
module_dynamic_load( const char* name )
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

    module_info_t* m = &g_modules[ slot ];
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

    m->status  = MODULE_STATUS_LOADED;
    m->version = 0;
    ms_log( "[module] loaded '%s' (api v%d, state %d B)", name, m->module_api->version, m->module_api->state_size );
    return true;
}

bool
module_register_static( const char* name, module_api_t* module_api, void* exported_api )
{
    /* Register a statically-linked module (no DLL). Must be called before module_init_all(). */

    assert( module_api != NULL );
    assert( exported_api != NULL );

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

    module_info_t* m = &g_modules[ slot ];
    memset( m, 0, sizeof( *m ) );
    strncpy( m->name, name, MODULE_NAME_MAX - 1 );
    m->name[ MODULE_NAME_MAX - 1 ] = '\0';
    m->is_static                   = true;
    m->dll                         = NULL;
    m->module_api                  = module_api;
    m->exported_api                = exported_api;
    m->status                      = MODULE_STATUS_LOADED;

    if ( state_ensure( m ) == false )
    {
        slot_free( slot );
        return false;
    }

    ms_log( "[module] registered static '%s'", name );
    return true;
}

bool
module_unload( const char* name )
{
    /* Unload a module by name, remains registered but inactive */

    int slot = slot_find( name );
    if ( slot < 0 )
    {
        set_error( "module_unload: '%s' not found", name );
        return false;
    }

    module_info_t* m = &g_modules[ slot ];

    if ( m->status == MODULE_STATUS_INITIALIZED )
        call_exit( m );

    if ( m->state != NULL )
    {
        ms_free( m->state );
        m->state = NULL;
    }

    if ( m->is_static == false )
        slot_unload_dll( m );

    ms_log( "[module] unloaded '%s'", name );
    slot_free( slot );
    return true;
}

bool
module_reload( const char* name )
{
    int slot = slot_find( name );
    if ( slot < 0 )
    {
        set_error( "module_reload: '%s' not found", name );
        return false;
    }

    module_info_t* m = &g_modules[ slot ];
    if ( m->is_static == true )
    {
        ms_log( "[module] '%s' is static, skipping reload.\n", name );
        return true;
    }

    /* ---- exit ---- */
    if ( m->status == MODULE_STATUS_INITIALIZED )
        call_exit( m );

    /* ---- swap DLL (state pointer is kept) ---- */
    slot_unload_dll( m );

    if ( slot_load_dll( m ) == false )
    {
        m->status = MODULE_STATUS_ERROR;
        return false;
    }

    /* grow state if the new version needs more room */
    if ( state_ensure( m ) == false )
    {
        m->status = MODULE_STATUS_ERROR;
        return false;
    }

    m->version++;
    m->status = MODULE_STATUS_LOADED;

    /* ---- re-init with preserved state ---- */

    if ( call_init( m ) == false )
    {
        set_error( "'%s' init failed after reload", name );
        m->status = MODULE_STATUS_ERROR;
        return false;
    }

    call_on_reload( m );

    m->status = MODULE_STATUS_INITIALIZED;
    ms_log( "[module] reloaded '%s' (reload #%d)\n", name, m->version );
    return true;
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

            module_info_t* m = &g_modules[ i ];

            /* assume this module is ready to be placed unless we
               discover a dependency that is not ready yet. */
            bool ready = true;

            /* Check each declared dependency of this module. */
            for ( int d = 0; d < m->module_api->dep_count; ++d )
            {
                const char* dep_name = m->module_api->deps[ d ];

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

bool
module_init_all( void )
{
    /* Call init() on all loaded modules in dependency order. Returns true on success. */

    if ( build_init_order() == false )
        return false;

    for ( int k = 0; k < g_init_count; ++k )
    {
        module_info_t* m = &g_modules[ g_init_order[ k ] ];
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

void
module_system_tick( float dt )
{
    for ( int k = 0; k < g_init_count; ++k )
    {
        module_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status == MODULE_STATUS_INITIALIZED )
            call_tick( m, dt );
    }
}

void
module_check_reloads( void )
{
    /* Check file times on all loaded modules and reload any that changed on disk. */

    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        module_info_t* m = &g_modules[ i ];

        if ( m->status == MODULE_STATUS_EMPTY || m->is_static )
            continue;

        char dll_path[ MAX_PATH ];
        path_dll( m->name, dll_path, sizeof( dll_path ) );

        uint64_t time = platform_file_time( dll_path );
        if ( time != 0 && time != m->last_write )
        {
            ms_log( "[module] '%s' changed — reloading...", m->name );
            module_reload( m->name );
        }
    }
}

/*==============================================================================================
    System init / exit
==============================================================================================*/

void
module_system_init()
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
    g_sys_api.get_api = module_get_api;

    /* set root path for module DLLs */
    platform_exe_dir( g_root, sizeof( g_root ) );
    ms_log( "[module] system init (root: %s)", g_root );

    /* start file watcher via platform_sys (no core needed) */
    if ( file_watch_init( g_root ) == false )
        ms_log( "[module] WARNING: file watch unavailable\n" );        
}

void
module_system_exit( void )
{
    file_watch_shutdown();

    /* exit in reverse init order so dependencies outlive their dependents */
    for ( int k = g_init_count - 1; k >= 0; --k )
    {
        module_info_t* m = &g_modules[ g_init_order[ k ] ];
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
        module_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

        if ( m->state )
        {
            ms_free( m->state );
            m->state = NULL;
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
module_list_all( void )
{
    static const char* status_str[] = { "EMPTY", "LOADED", "INITIALIZED", "ERROR" };

    ms_log( "---- modules (%d / %d) ----\n", g_module_count, MAX_MODULES );
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        module_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

        ms_log( "  [%2d]  %-24s  %-13s  api_v%-3d  reload#%-3d  state:%dB  %s", i, m->name,
                status_str[ m->status ], m->module_api ? m->module_api->version : 0, m->version,
                m->state_size, m->is_static ? "(static)" : "" );
    }
}

/*============================================================================================*/