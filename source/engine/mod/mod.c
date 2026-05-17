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
    A counter increments on each reload so the new shadow never collides with the
    still-loaded previous copy.

        Original : <exe_dir>\render.dll
        Shadow 0 : <exe_dir>\render.tmp_0.dll
        Shadow 1 : <exe_dir>\render.tmp_1.dll  (shadow 0 deleted after unload)

    State ownership
    ---------------
    The system allocates state_size bytes from the bootstrap allocator and zeroes them on
    first load. State is preserved across hot-reloads and never freed until final unload.
    If state_size grows across a reload, the tail bytes are zeroed; the block never shrinks.
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
              mod_static_load( "sys",  sys_get_mod_desc() );
              mod_static_load( "core", core_get_mod_desc() );

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

#include "mod.h"
#include "mod_export.h"
#include "mod_host.h"

/*==============================================================================================

    mod_internal.c — Module system internals.

    Unity include for mod.c — not compiled directly. All symbols are file-local (static)
    or internal helpers consumed only within that translation unit.

    Sections
    --------
      Constants & Types         – module slot, pending reload, reload snapshot
      Global State              – module table, pending queue, root path
      Bootstrap Allocator       – ms_log / ms_alloc / ms_free
      Error                     – set_error (writes g_last_error)
      Path Helpers              – path_dll, path_shadow, dll_file_time
      Shadow File Helpers       – shadow_copy, shadow_delete
      API Validation            – api_validate (NULL-free func_api check)
      DLL Load / Unload         – slot_load_dll, slot_unload_dll
      Slot Management           – slot_find, slot_alloc, slot_free, slot_create, slot_destroy
      State Helper              – state_ensure (alloc / grow persistent state)
      API Slot Helpers          – api_slot_create, api_slot_refresh
      Lifecycle Invocations     – call_init, call_exit, call_reload
      Reload Snapshot           – snapshot_save, snapshot_rollback
      Pending Queue             – pending_find, pending_flag
      Reload Core               – do_reload (single-module DLL swap with rollback)
      Dependency Resolution     – build_init_order (iterative topological sort)
      Stale Shadow Cleanup      – shadow_cleanup (purge crashed-session leftovers)

==============================================================================================*/

static mod_dll_event_fn g_dll_load_fn     = NULL;
static void*            g_dll_load_user   = NULL;
static mod_dll_event_fn g_dll_unload_fn   = NULL;
static void*            g_dll_unload_user = NULL;

#include "mod_internal.c"

/*==============================================================================================
    Public: System Lifetime
==============================================================================================*/

void 
mod_system_init( void )
{
    memset( g_modules, 0, sizeof( g_modules ) );
    memset( g_pending, 0, sizeof( g_pending ) );
    memset( g_last_error, 0, sizeof( g_last_error ) );

    g_module_count   = 0;
    g_init_count     = 0;
    g_pending_count  = 0;
    g_shadow_counter = 0;
    g_api_func       = mod_get_api;

    sys_exe_dir( g_root, sizeof( g_root ) );
    ms_log( "[module] system init (root: %s)", g_root );

    /* purge shadow copies left over from any previous session that crashed */
    shadow_cleanup();

    if ( !sys_filewatch_init( g_root ) )
        ms_log( "[module] WARNING: file watch unavailable" );

    mod_static_load( "mod", mod_get_mod_desc() );
}

void 
mod_system_exit( void )
{
    sys_filewatch_shutdown();

    /* exit() in reverse init order so dependencies outlive their dependents */
    for ( int k = g_init_count - 1; k >= 0; --k )
    {
        mod_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status != MODULE_STATUS_INITIALIZED )
            continue;

        call_exit( m );
        m->status = MODULE_STATUS_LOADED;
        ms_log( "[module] exited '%s'", m->name );
    }

    /* destroy every remaining slot: free state, free api_slot, unload DLL */
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        if ( g_modules[ i ].status != MODULE_STATUS_EMPTY )
            slot_destroy( &g_modules[ i ] );
    }

    g_init_count    = 0;
    g_pending_count = 0;
    ms_log( "[module] system shutdown complete" );
}

/*==============================================================================================
    Public: Registration
==============================================================================================*/

bool 
mod_static_load( const char* name, mod_desc_t* mod_api )
{
    assert( mod_api != NULL );
    assert( mod_api->func_api != NULL && "func_api must not be NULL in a static module" );

    if ( slot_find( name ) >= 0 )
    {
        ms_log( "[module] static '%s' already registered", name );
        return true;
    }

    if ( !api_validate( name, mod_api->func_api, mod_api->func_api_size ) )
        return false;

    mod_info_t* m = slot_create( name );
    if ( !m )
        return false;

    m->is_static = true;
    m->mod_api   = mod_api;

    if ( !state_ensure( m ) || !api_slot_create( m ) )
    {
        slot_destroy( m );
        return false;
    }

    m->status = MODULE_STATUS_LOADED;
    ms_log( "[module] registered static '%s' (api v%d, state %d, api %d)", name, mod_api->version,
            mod_api->state_size, mod_api->func_api_size );
    return true;
}

bool 
mod_dynamic_load( const char* name )
{
    if ( slot_find( name ) >= 0 )
    {
        ms_log( "[module] '%s' already loaded", name );
        return true;
    }

    mod_info_t* m = slot_create( name );
    if ( !m )
        return false;

    if ( !slot_load_dll( m ) || !state_ensure( m ) || !api_slot_create( m ) )
    {
        slot_destroy( m );
        return false;
    }

    m->status  = MODULE_STATUS_LOADED;
    m->version = 0;
    ms_log( "[module] loaded '%s' (api v%d, state %d, api %d)", name, m->mod_api->version,
            m->mod_api->state_size, m->mod_api->func_api_size );

    if ( g_dll_load_fn )
        g_dll_load_fn( name, m->dll, g_dll_load_user );

    return true;
}

bool 
mod_unload( const char* name )
{
    int slot = slot_find( name );
    if ( slot < 0 )
    {
        set_error( "mod_unload: '%s' not found", name );
        return false;
    }

    mod_info_t* m = &g_modules[ slot ];
    if ( m->status == MODULE_STATUS_INITIALIZED )
        call_exit( m );

    if ( !m->is_static && g_dll_unload_fn )
        g_dll_unload_fn( m->name, m->dll, g_dll_unload_user );

    slot_destroy( m );
    ms_log( "[module] unloaded '%s'", name );
    return true;
}

/*==============================================================================================
    Public: Initialization
==============================================================================================*/

bool 
mod_init_all( void )
{
    if ( !build_init_order() )
        return false;

    for ( int k = 0; k < g_init_count; ++k )
    {
        mod_info_t* m = &g_modules[ g_init_order[ k ] ];
        if ( m->status != MODULE_STATUS_LOADED )
            continue;

        if ( !call_init( m ) )
        {
            set_error( "'%s' init() returned false", m->name );
            m->status = MODULE_STATUS_ERROR;
            return false;
        }

        m->status = MODULE_STATUS_INITIALIZED;
        ms_log( "[module] initialized '%s'", m->name );
    }

    return true;
}

/*==============================================================================================
    Public: Reload Queueing and Flushing
==============================================================================================*/

bool 
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

    pending_flag( name, sys_time_ms(), dll_file_time( name ) );
    return true;
}

int 
mod_reload_all( void )
{
    uint64_t now_ms = sys_time_ms();
    int      queued = 0;

    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY || m->is_static )
            continue;

        pending_flag( m->name, now_ms, dll_file_time( m->name ) );
        ++queued;
    }

    ms_log( "[module] queued %d module(s) for reload", queued );
    return queued;
}

void 
mod_check_reloads( void )
{
    uint64_t now_ms = sys_time_ms();

    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY || m->is_static )
            continue;

        uint64_t ft = dll_file_time( m->name );
        if ( ft != 0 && ft != m->last_write )
            pending_flag( m->name, now_ms, ft );
    }
}

/* Called once per frame at a quiescent point. Collects every pending entry whose debounce
   has expired, sorts by position in g_init_order, and reloads in that order — so when a
   shared-header rebuild flags both 'render' and 'game' at once, 'render' (lower init rank)
   reloads first and 'game's reload() sees the new render_api in place. */

int 
mod_system_flush_reloads( void )
{
    if ( g_pending_count == 0 )
        return 0;

    uint64_t now_ms = sys_time_ms();

    /* --- 1. collect ready entries with their dependency rank --- */

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
        const pending_reload_t* p = &g_pending[ i ];
        if ( ( now_ms - p->flagged_at_ms ) < DEBOUNCE_MS )
            continue;

        int slot = slot_find( p->name );

        /* Default rank sorts after every dependency-ordered module. Covers the
           LOADED-but-not-yet-INITIALIZED case and any deregistered name. */
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

    /* --- 2. sort by dep_rank ascending (insertion sort; ready_count <= 16) --- */

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

    /* --- 3. reload in dependency order --- */

    int reloaded = 0;
    for ( int i = 0; i < ready_count; ++i )
    {
        if ( ready[ i ].slot < 0 )
            continue; /* module unregistered between queue and flush */

        if ( do_reload( &g_modules[ ready[ i ].slot ] ) )
            ++reloaded;
    }

    /* --- 4. compact g_pending, dropping every entry we processed (success or failure) --- */

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
    Public: DLL event callbacks
==============================================================================================*/

void
mod_set_dll_load_cb( mod_dll_event_fn fn, void* user )
{
    g_dll_load_fn   = fn;
    g_dll_load_user = user;
}

void
mod_set_dll_unload_cb( mod_dll_event_fn fn, void* user )
{
    g_dll_unload_fn   = fn;
    g_dll_unload_user = user;
}

/*==============================================================================================
    Public: API Access
==============================================================================================*/

bool
mod_is_loaded( const char* name )
{
    return slot_find( name ) >= 0;
}

const void*
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

    return m->api_slot; /* stable across reloads */
}

const char*
mod_last_error( void )
{
    return g_last_error;
}

/*==============================================================================================
    Public: self-registration as a discoverable module

    The mod system registers itself so DLL modules can fetch mod_api_t via the
    standard MOD_FETCH_API / mod_api() pattern, enabling plugin management from DLLs
    (e.g. an editor loading its own plugin DLLs) without special host wiring.

    mod_get_mod_desc is called once from mod_system_init().  Hosts that need to pass the
    descriptor to tooling (e.g. mod inspector) may also call it directly.
==============================================================================================*/

const mod_api_t g_mod_api_struct = {
    .dynamic_load = mod_dynamic_load,
    .unload       = mod_unload,
    .get_api      = mod_get_api,
    .reload       = mod_reload,
    .is_loaded    = mod_is_loaded,
    .each         = mod_each,
    .last_error   = mod_last_error,
};

mod_desc_t*
mod_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( mod_api_t ),
        .func_api      = &g_mod_api_struct,
        .dep_count     = 0,
        .init          = NULL,
        .exit          = NULL,
        .reload        = NULL,
    };
    return &api;
}

/*==============================================================================================
    Public: iteration over loaded modules
==============================================================================================*/

void
mod_each( mod_visitor_fn visit, void* user )
{
    if ( !visit ) return;
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY ) continue;
        visit( m->name, m->mod_api, user );
    }
}

/*==============================================================================================
    Public: Debug
==============================================================================================*/

void 
mod_list_all( void )
{
    static const char* status_str[] = { "EMPTY", "LOADED", "INITIALIZED", "ERROR" };

    ms_log( "---- modules (%d / %d) ----", g_module_count, MAX_MODULES );
    for ( int i = 0; i < MAX_MODULES; ++i )
    {
        mod_info_t* m = &g_modules[ i ];
        if ( m->status == MODULE_STATUS_EMPTY )
            continue;

        ms_log( "  [%2d]  %-24s  %-13s  api_v%-3d  reload# %-3d  state:%dB  %s", i, m->name,
                status_str[ m->status ], m->mod_api ? m->mod_api->version : 0, m->version, m->state_size,
                m->is_static ? "(static)" : "" );
    }
}

/*============================================================================================*/