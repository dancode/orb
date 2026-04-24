/*==============================================================================================

    module_sys.c

    Hot-reload module system implementation.

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

    Example usage (remove in production)

    module system

    Startup sequence:
        1. module_system_init()       — boot (no modules yet)
        2. module_register_static()   — core + engine (already running in the exe)
        3. module_load()              — dynamic DLLs
        4. module_init_all()          — topo-sort deps → call every init()
        5. main loop                  — tick + hot-reload polling
        6. module_system_exit()       — reverse-order exit + DLL unload + shadow cleanup

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "core/core.h"
#include "core/core_api.h"
#include "platform/platform.h"
#include "engine_api.h"
#include "module_api.h"
#include "module_sys.h"

// #include "platform/platform.h"

/*==============================================================================================
    Platform layer
==============================================================================================*/

#ifdef PLATFORM_WINDOWS

#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

static HANDLE     g_dir_watch  = NULL;     // directory handle
static OVERLAPPED g_overlapped = { 0 };    // overlapped io structure

#else

#    define MAX_PATH 260
#    error "module_sys: platform not implemented"

#endif

/*============================================================================================*/

#ifdef PLATFORM_WINDOWS

static uint64_t
platform_file_time( const char* path )
{
    /* Returns 0 on error or if the file does not exist. */

    WIN32_FILE_ATTRIBUTE_DATA data;
    if ( !GetFileAttributesExA( path, GetFileExInfoStandard, &data ) )
        return 0;

    ULARGE_INTEGER uli;
    uli.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    uli.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return uli.QuadPart;
}

static bool
platform_copy_file( const char* src, const char* dst )
{
    return CopyFileA( src, dst, FALSE ) != FALSE;
}

static bool
platform_delete_file( const char* path )
{
    return DeleteFileA( path ) != 0;
}

static void
platform_exe_dir( char* out, int size )
{
    assert( size >= MAX_PATH );
    DWORD len = GetModuleFileNameA( NULL, out, ( DWORD )size );
    if ( len == 0 || len == ( DWORD )size )
    {
        assert( 0 && "GetModuleFileNameA failed" );
        return;
    }
    /* strip the filename, keep the trailing slash */
    for ( DWORD i = len; i > 0; --i )
    {
        if ( out[ i ] == '\\' || out[ i ] == '/' )
        {
            out[ i ] = '\0';
            break;
        }
    }
}

#endif

/*==============================================================================================
    Internal state
==============================================================================================*/

#define MAX_MODULES 32

static core_api_t*   g_core   = NULL;
static engine_api_t* g_engine = NULL;

static module_info_t g_modules[ MAX_MODULES ];
static int32_t       g_module_count = 0;

/* Dependency-resolved initialization order (indices into g_modules). */
static int32_t  g_init_order[ MAX_MODULES ];
static int32_t  g_init_count;

static char     g_root[ MAX_PATH ]  = { 0 }; /* directory containing the exe + DLLs */
static char     g_last_error[ 256 ] = { 0 }; /* save error messages if needed */

static uint32_t g_shadow_counter    = 0; /* global; incremented per shadow copy created */

/* Passed into every init() / on_reload() call so DLLs can resolve APIs
   without linking against the exe. Defined after module_get_api(). */

static module_sys_api_t g_sys_api;

/*==============================================================================================
    Path helpers
==============================================================================================*/

static void
ensure_root_path( void )
{
    /* set platform specific global base path for module loading */

    if ( g_root[ 0 ] != '\0' )
        return; /* already set */

    platform_exe_dir( g_root, sizeof( g_root ) );
}

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
    Error reporting
==============================================================================================*/

static void
set_error( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( g_last_error, sizeof( g_last_error ), fmt, args );
    va_end( args );

    if ( g_core )
        g_core->log( "[module] ERROR: %s", g_last_error );
}

const char* /* public */
module_last_error( void )
{
    return g_last_error;
}

/*==============================================================================================
    Slot management
==============================================================================================*/

static int
slot_find( const char* name )
{
    /* find module name id */
    sid_t name_sid = sid_find_cstr( name );
    if ( sid_equals( name_sid, SID_INVALID ) )
    {
        return -1; /* name not registered */
    }

    /* find id of module if registered */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( sid_equals( g_modules[ i ].name, name_sid ) )
            return i; /* found module */
    }

    /* this should never happen */
    assert( 0 && "module not found" );
    return -1;
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

    if ( !platform_copy_file( src, dst ) )
    {
#ifdef PLATFORM_WINDOWS
        set_error( "copy failed (0x%lx): %s > %s", GetLastError(), src, dst );
#else
        set_error( "copy failed: %s > %s", src, dst );
#endif
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
        m->state      = g_core->alloc( ( size_t )required );
        m->state_size = required;
        if ( !m->state )
        {
            set_error( "'%s': state alloc failed (%d bytes)", sid_cstr( m->name ), required );
            return false;
        }
        memset( m->state, 0, ( size_t )required );
    }
    else if ( required > m->state_size )
    {
        /* state grew across a reload — realloc and zero the new region */
        void* new_state = g_core->alloc( ( size_t )required );
        if ( !new_state )
        {
            set_error( "'%s': state realloc failed (%d bytes)", sid_cstr( m->name ), required );
            return false;
        }
        memcpy( new_state, m->state, ( size_t )m->state_size );
        memset( ( char* )new_state + m->state_size, 0, ( size_t )( required - m->state_size ) );
        g_core->free( m->state );
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

    const char* name = sid_cstr( m->name );

    /* --- shadow copy --- */
    m->shadow_count = g_shadow_counter++;
    if ( !shadow_copy( name, m->shadow_count ) )
        return false;

    /* --- load shadow --- */
    char shadow[ MAX_PATH ];
    path_shadow( name, m->shadow_count, shadow, sizeof( shadow ) );

    m->dll = library_load( shadow ); /* platform: LoadLibraryA(full_path) */
    if ( !m->dll )
    {
        set_error( "LoadLibrary failed (0x%lx): %s", GetLastError(), shadow );
        shadow_delete( name, m->shadow_count );
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
    shadow_delete( sid_cstr( m->name ), m->shadow_count );

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
module_load( const char* name )
{
    /* Register and load a module by name.  Returns true on success.  On failure the module is not
       registered and an error message is available via module_last_error(). */

    if ( slot_find( name ) >= 0 )
    {
        g_core->log( "[module] '%s' already loaded.", name );
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
    m->name   = sid_intern_cstr( name );
    m->status = MODULE_STATUS_ERROR; /* provisional until success */

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
    g_core->log( "[module] loaded '%s' (api v%d, state %d B)", name, m->module_api->version,
                 m->module_api->state_size );
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
        g_core->log( "[module] static '%s' already registered.\n", name );
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
    m->name         = sid_intern_cstr( name );
    m->is_static    = true;
    m->dll          = NULL;
    m->module_api   = module_api;
    m->exported_api = exported_api;
    m->status       = MODULE_STATUS_LOADED;

    if ( state_ensure( m ) == false )
    {
        slot_free( slot );
        return false;
    }

    g_core->log( "[module] registered static '%s'", name );
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
        g_core->free( m->state );
        m->state = NULL;
    }

    if ( m->is_static == false )
        slot_unload_dll( m );

    g_core->log( "[module] unloaded '%s'", name );
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
        g_core->log( "[module] '%s' is static, skipping reload.\n", name );
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
    g_core->log( "[module] reloaded '%s' (reload #%d)\n", name, m->version );
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

                sid_t dep_sid = sid_find_cstr( dep_name );
                bool  found   = false;

                /* search only the modules already added to g_init_order[0 .. g_init_count-1].
                   If the dependency is already there, then this dependency is satisfied. */

                for ( int k = 0; k < g_init_count; ++k )
                {
                    if ( sid_equals( g_modules[ g_init_order[ k ] ].name, dep_sid ) )
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
                        set_error( "'%s' depends on '%s' which is not registered", sid_cstr( m->name ), dep_name );
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
            set_error( "dependency cycle detected involving '%s'", sid_cstr( g_modules[ i ].name ) );
            return false;
        }
    }

    /* debug: print resolved initialization order */
    g_core->log( "[module] init order (%d):", g_init_count );
    for ( int i = 0; i < g_init_count; ++i )
    {
        int slot = g_init_order[ i ];
        g_core->log( "  %d: %s", i, sid_cstr( g_modules[ slot ].name ) );
    }

    return true;
}

/*==============================================================================================
    Public: init / tick / reload / exit
==============================================================================================*/

bool
module_init_all( void )
{
    if ( build_init_order() == false )
        return false;

    for ( int k = 0; k < g_init_count; ++k )
    {
        module_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status != MODULE_STATUS_LOADED )
            continue;

        if ( call_init( m ) == false )
        {
            set_error( "'%s' init() returned false", sid_cstr( m->name ) );
            m->status = MODULE_STATUS_ERROR;
            return false;
        }

        m->status = MODULE_STATUS_INITIALIZED;
        g_core->log( "[module] initialized '%s'", sid_cstr( m->name ) );
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
        path_dll( sid_cstr( m->name ), dll_path, sizeof( dll_path ) );

        uint64_t time = platform_file_time( dll_path );
        if ( time != 0 && time != m->last_write )
        {
            g_core->log( "[module] '%s' changed — reloading...", sid_cstr( m->name ) );
            module_reload( sid_cstr( m->name ) );
        }
    }
}

/*==============================================================================================
    System init / exit
==============================================================================================*/

void
module_system_init( core_api_t* core, engine_api_t* engine )
{
    assert( core != NULL );
    assert( engine != NULL );

    /* zero module slots (status = EMPTY) */
    memset( g_modules, 0, sizeof( g_modules ) );
    g_module_count    = 0;
    g_init_count      = 0;
    g_last_error[ 0 ] = '\0';
    g_shadow_counter  = 0;

    /* save core and engine API pointers for modules to use during init and tick */
    g_core   = core;
    g_engine = engine;

    /* wire up the get_api passed into every module init() */
    g_sys_api.get_api = module_get_api;

    /* set root path for module DLLs */
    ensure_root_path();

    g_core->log( "[module] system init (root: %s)", g_root );
}

void
module_system_exit( void )
{
    /* exit in reverse init order so dependencies outlive their dependents */
    for ( int k = g_init_count - 1; k >= 0; --k )
    {
        module_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status != MODULE_STATUS_INITIALIZED )
            continue;

        call_exit( m );

        /* LOADED but not INITIALIZED anymore since we called exit() */
        m->status = MODULE_STATUS_LOADED;
        g_core->log( "[module] exited '%s'", sid_cstr( m->name ) );
    }

    /* unload DLLs and free state */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        module_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

        if ( m->state )
        {
            g_core->free( m->state );
            m->state = NULL;
        }

        if ( !m->is_static )
            slot_unload_dll( m );
    }

    memset( g_modules, 0, sizeof( g_modules ) );
    g_module_count = 0;
    g_init_count   = 0;
    g_core->log( "[module] system shutdown complete" );
}

/*==============================================================================================
    Debug
==============================================================================================*/

void
module_list_all( void )
{
    static const char* status_str[] = { "EMPTY", "LOADED", "INITIALIZED", "ERROR" };

    g_core->log( "---- modules (%d / %d) ----\n", g_module_count, MAX_MODULES );
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        module_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

        g_core->log( "  [%2d]  %-24s  %-13s  api_v%-3d  reload#%-3d  state:%dB  %s", i, sid_cstr( m->name ),
                     status_str[ m->status ], m->module_api ? m->module_api->version : 0, m->version,
                     m->state_size, m->is_static ? "(static)" : "" );
    }
}

/*============================================================================================*/