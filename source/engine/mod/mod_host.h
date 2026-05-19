#ifndef MOD_HOST_H
#define MOD_HOST_H
/*==============================================================================================

    engine/mod/mod_host.h — Module system management. Include in host executables only.

    Lifecycle
    ---------
        mod_system_init()           Boot; registers "mod" itself as a static module automatically.
        mod_static_load( name )     Register a statically-linked module (no DLL).
        mod_load( name )            Register and load a module (expands per build mode).
        mod_init_all()              Topo-sort deps, call each module's init().
        mod_check_reloads()         Poll file timestamps; enqueue changed DLLs.
        mod_system_flush_reloads()  Apply queued swaps; call once per frame at a safe point.

                                    Use these for on demand reloads.
        mod_reload( name )          Force-queue a single module for reload.
        mod_reload_all()            Force-queue every dynamic module for reload.
        
        mod_system_exit()           Exit in reverse dep order; unload DLLs; delete shadows.

    Deferred reload model
    ---------------------
    mod_reload, mod_reload_all, and the file-watcher all enqueue — they never swap
    immediately. The host calls mod_system_flush_reloads() at a single, well-defined
    point each frame (after simulation and rendering, when no module code is in flight).
    That is the only point where a DLL swap is provably safe.

    mod_load build variants
    -----------------------
        BUILD_STATIC defined:
            mod_load( render ) -> mod_static_load( "render", render_get_mod_desc() )

        BUILD_STATIC not defined:
            mod_load( render ) -> mod_dynamic_load( "render" )
                                  shadow-copy -> load -> resolve exports -> alloc state

    API access
    ----------
    Call sites are identical in both build modes:

        Static:  render()->begin_frame();   // zero-cost; LTO-devirtualizable
        Dynamic: render()->begin_frame();   // one pointer load

    See MOD_GATEWAY_STATIC / MOD_GATEWAY_DYNAMIC in mod.h.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/sys/sys_host.h"

// clang-format off
/*==============================================================================================
    MODULE LOAD — Build-mode-transparent module registration macro
==============================================================================================*/

#ifdef BUILD_STATIC
    #define mod_load( name ) mod_static_load( #name, name##_get_mod_desc() )
#else
    #define mod_load( name ) mod_dynamic_load( #name )
#endif

/* Always-static variant — use for modules that are never DLLs (sys, core, mod). */
#define mod_static( name ) mod_static_load( #name, name##_get_mod_desc() )

/*==============================================================================================
    MOD_HOST_FETCH_API — Populates a cached API pointer from outside a module init() callback.

    Valid only after mod_init_all(). Use inside on_ready() or host update loops.
    Prefer MOD_FETCH_API (in mod.h) inside module init() / reload() callbacks instead.

    Usage:
        MOD_HOST_FETCH_API( render_api_t, render );
==============================================================================================*/

#ifdef BUILD_STATIC
    #define MOD_HOST_FETCH_API( type, name ) ( 1 )
#else
    #define MOD_HOST_FETCH_API( type, name ) \
        ( ( g_##name##_api_ptr = ( const type* )mod_get_api( #name ) ) != NULL )
#endif

/*==============================================================================================
    Types
==============================================================================================*/

typedef struct mod_desc_s mod_desc_t;

typedef enum module_status_t
{
    MODULE_STATUS_EMPTY       = 0, /* slot unused      */
    MODULE_STATUS_LOADED      = 1, /* DLL loaded, API resolved, state allocated */
    MODULE_STATUS_INITIALIZED = 2, /* init() returned true */
    MODULE_STATUS_ERROR       = 3, /* load or init failed */

} module_status_t;

/*==============================================================================================
    System lifetime
==============================================================================================*/

void            mod_system_init             ( void );
void            mod_system_exit             ( void );
mod_desc_t*     mod_get_mod_desc            ( void );

/*==============================================================================================
    Registration and loading
==============================================================================================*/

bool            mod_static_load             ( const char* name, mod_desc_t* mod_desc );
bool            mod_dynamic_load            ( const char* name );
bool            mod_unload                  ( const char* name );
bool            mod_reload                  ( const char* name );
int             mod_reload_all              ( void );
void            mod_check_reloads           ( void );
int             mod_system_flush_reloads    ( void );

/*==============================================================================================
    Initialization
==============================================================================================*/

bool            mod_init_all                ( void );

/*==============================================================================================
    API access
==============================================================================================*/

const void*     mod_get_api                 ( const char* name );
bool            mod_is_loaded               ( const char* name );
const char*     mod_last_error              ( void );

/*==============================================================================================
    Module lifecycle callbacks

    Two hook points named for the lifecycle moments they bracket — not for the load/unload
    plumbing that triggers them. Both signatures are desc-based, so subscribers (reflection,
    profilers) need not care whether the module came from a DLL or was statically linked.

    pre_init  - fires immediately BEFORE a module's init() runs. Dispatched from
                mod_init_all() in dep order, once per newly-loaded module. Load itself
                (mod_static_load / mod_dynamic_load) is passive — no callbacks fire there.
                Subscribers therefore see modules in DEPENDENCY order, not registration
                order, which is what the rs stack-frame model wants.

    post_exit - fires immediately AFTER a module's exit() has run. Dispatched from
                mod_unload(). For DLLs this runs before the handle is released.

    Hot-reload is a self-contained mini-lifecycle outside mod_init_all. Its swap-commit
    point fires both callbacks for that one module:

        post_exit( name, old_desc )    // old instance is gone
        ... DLL swap ...
        pre_init ( name, new_desc )    // new instance about to come online

    Callbacks installed AFTER some modules are already initialised do not retroactively
    fire for those modules — install them once at host startup, before mod_init_all().
==============================================================================================*/

typedef void ( *mod_event_fn )( const char* name, const mod_desc_t* desc, void* user );

void            mod_set_pre_init_cb         ( mod_event_fn fn, void* user );
void            mod_set_post_exit_cb        ( mod_event_fn fn, void* user );

/*==============================================================================================
    Iteration

    Visits every non-empty module slot in load order.  mod_visitor_fn is defined in mod.h.
==============================================================================================*/

void            mod_each                    ( mod_visitor_fn visit, void* user );

/*==============================================================================================
    Debug
==============================================================================================*/

void            mod_list_all                ( void );

// clang-format on
/*============================================================================================*/
#endif    // MOD_HOST_H
