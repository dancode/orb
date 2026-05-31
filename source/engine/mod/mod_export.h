/*==============================================================================================

    engine/mod_export.h :  Module implementation header.

    This is the only header a module author needs when implementing a module.

    It provides:

      - mod_desc_t: the descriptor every module returns from name_get_mod_desc()
      - Lifecycle callback typedefs (mod_init_fn, mod_exit_fn, mod_reload_fn)
      - get_api_fn: the callback passed into init() and reload() for sibling API lookup
      - MOD_EXPORT / MOD_DEFINE_EXPORTS: DLL export machinery

    Include ONLY in a module's .c implementation. Hosts and sibling modules
    include mod_import.h instead.

    Module authoring
    ----------------
    Every module must provide two functions derived from its name:

        mod_desc_t*  <name>_get_mod_desc( void )  — lifecycle descriptor (static + dynamic)
        get_mod_desc( void )                     — generic DLL export (dynamic only)

    The generic export is emitted by placing this once at the bottom of the module's .c:

        MOD_DEFINE_EXPORTS( <name> )

    State ownership
    ---------------
    The system allocates state_size bytes and zeroes them on first load. State is
    preserved across hot-reloads and freed only on final unload. Modules must not
    free their own state.

    Reload semantics
    ----------------
    First load:    init()      — called with a zeroed state block.
    Hot-reload:    reload()    — called with the preserved state block.

    Use reload() to re-cache sibling API pointers that may have changed after a DLL swap.
    Use init() for one-time setup that must not repeat on reload.

    func_api stability
    ------------------
    The system copies the module's func_api struct into a stable, system-owned memory block
    at load time and on every successful reload. Consumers cache the address of that block —
    it never changes — so hot-reload automatically routes all calls to new function pointers
    without any manual pointer re-cache in reload().

    Static modules skip the copy: their func_api is a linked-in const struct at a stable
    address, so the system points directly at it.

    func_api_size MUST NOT change across a reload. Adding or removing API functions
    requires a full host restart.

==============================================================================================*/
#ifndef MOD_EXPORT_H
#define MOD_EXPORT_H

#include "orb.h"
#include "engine/mod/mod_import.h"

/*==============================================================================================
    get_api_fn — System callback passed into every module's init() and reload().

    The only system symbol a DLL ever calls back into — modules never link against the exe.
    Use it inside init() or reload() to resolve sibling API pointers:

        if ( !MOD_FETCH_API( core_api_t, core ) ) return false;

    In static builds MOD_FETCH_API is a no-op (structs are linked in directly).
==============================================================================================*/

typedef const void* ( *get_api_fn )( const char* name );

/*==============================================================================================
    Lifecycle Callback Typedefs
==============================================================================================*/

#define MOD_MAX_DEPS 8

/* Called on first load. Return false to abort the load.
   `state` is zeroed on first load and preserved across reloads. */
typedef bool ( *mod_init_fn )( void* state, get_api_fn get_api );

/* Called on unload. Use only for cleanup of live resources (threads, GPU objects).
   Do not free `state` — the system owns it. */
typedef void ( *mod_exit_fn )( void* state );

/* Called after a new DLL is loaded but before the old one is unloaded.
   Return false to abort the reload and keep the old DLL active. */
typedef bool ( *mod_reload_fn )( void* state, get_api_fn get_api );

/*==============================================================================================
    mod_desc_t — Module descriptor. Every module returns one from name_get_mod_desc().
==============================================================================================*/

typedef struct mod_desc_s
{
    int32_t       version;              /* bump when ABI changes */
    int32_t       state_size;           /* persistent state size or 0 = stateless */
    int32_t       func_api_size;        /* sizeof(<name>_api_t) — system snapshots this many bytes */
    int32_t       dep_count;            /* number of dependencies in deps[] */
    const void*   func_api;             /* pointer to the module's exported API struct */
    const char*   deps[ MOD_MAX_DEPS ]; /* names of modules to init before this one */

    mod_init_fn   init;
    mod_exit_fn   exit;
    mod_reload_fn reload;

    /* Optional reflection entry point — opaque to the mod system.
       Set to the generated <name>_ref_register (via MOD_REFLECT_FUNC) when the module
       has reflected types; NULL otherwise. The pointer lives in the same image as the
       desc — exe for statics, DLL for dynamics — so calling through it works for both
       build modes with no symbol lookup. See MOD_REFLECT_FUNC below. */

    const void* ref_register; /* void (*)( const ref_reg_api_t* ) or NULL */

} mod_desc_t;

/*==============================================================================================
    MOD_API_FUNC / MOD_REFLECT_FUNC — Wire a module's exports into its mod_desc_t.

    MOD_API_FUNC( name )     — points func_api at the generated g_<name>_api_struct.
    MOD_REFLECT_FUNC( name ) — points ref_register at the generated <name>_ref_register.

    Both macros cast to const void* so mod_export.h stays independent of ref.h and the
    generated headers. The function pointers always live in the same image as the desc
    (exe for statics, DLL for dynamics) — no symbol lookup needed in either build mode.

    Usage:

        #include "<name>.generated.h"

        static mod_desc_t s_<name>_mod_desc = {
            .version       = 1,
            .func_api      = MOD_API_FUNC( <name> ),
            .func_api_size = sizeof( <name>_api_t ),
            .ref_register   = MOD_REFLECT_FUNC( <name> ),
            ...
        };
==============================================================================================*/

#define MOD_API_FUNC( name )     ( ( const void* )( &g_##name##_api_struct ) )
#define MOD_REFLECT_FUNC( name ) ( ( const void* )( name##_ref_register ) )

/* DLL entry-point typedef resolved via LoadLibrary / dlopen. */
typedef mod_desc_t* ( *get_mod_desc_fn )( void );

/*==============================================================================================
    MOD_EXPORT — Marks a symbol for DLL export on platforms that require it.
==============================================================================================*/

#if defined( _WIN32 ) && !defined( BUILD_STATIC )
    #define MOD_EXPORT __declspec( dllexport )
#else
    #define MOD_EXPORT
#endif

/*==============================================================================================
    MOD_DEFINE_EXPORTS — Emits the undecorated "get_mod_desc" export resolved at runtime.

    Place once at the bottom of the module's .c, outside any function:

        MOD_DEFINE_EXPORTS( render )

    In static builds this expands to nothing; the system calls name##_get_mod_desc directly.
==============================================================================================*/

#ifdef BUILD_STATIC
    #define MOD_DEFINE_EXPORTS( name )
#else
    #define MOD_DEFINE_EXPORTS( name ) \
        MOD_EXPORT mod_desc_t* get_mod_desc( void ) { return name##_get_mod_desc(); }
#endif

/*============================================================================================*/
#endif    // MOD_EXPORT_H
