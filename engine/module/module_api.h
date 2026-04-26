#ifndef MODULE_API_H
#define MODULE_API_H
/*==============================================================================================

    module_api.h

    The only header module authors need to implement a module (DLL or static).

    Every module DLL must export two C symbols:

        module_api_t*  get_module_api( void )   -- lifecycle (init/tick/exit)
        void*          get_api( void )          -- the module's own typed API struct

    The module system owns all state memory.  Modules never malloc their own
    persistent state — the system allocates api->state_size bytes, zeroes it on
    first load, and preserves it across hot-reloads.

==============================================================================================*/

#include "module/module_sys_api.h"

/*==============================================================================================
    Module lifecycle callbacks
==============================================================================================*/

#define MODULE_MAX_DEPS 8

/* init : called on first load and again after every hot-reload.
         state: persistent memory block (zeroed on first load, preserved on reload).
         sys: use to pull any registered API by name.
         return: false to signal a fatal error; the system will mark the module ERROR. */
typedef bool ( *mod_init_fn )( void* state, module_sys_api_t* sys );

/* tick : called every frame while the module is INITIALIZED. */
typedef void ( *mod_tick_fn )( void* state, float dt );

/* exit : called before unload or reload. Do NOT free `state` — the system owns it. */
typedef void ( *mod_exit_fn )( void* state );

/* on_reload : optional, called after init() on a hot-reload.
               Use to re-cache pointers that changed when the DLL was swapped. */
typedef void ( *mod_on_reload_fn )( void* state, module_sys_api_t* sys );

/*==============================================================================================
    module_api_t : struct every module must provide via get_module_api()
==============================================================================================*/

typedef struct module_api_s
{
    int32_t          version;                 /* bump when ABI changes */
    int32_t          state_size;              /* bytes to allocate for persistent state; 0 = stateless */
    const char*      deps[ MODULE_MAX_DEPS ]; /* names of modules that must init before this one */
    int32_t          dep_count;

    mod_init_fn      init;
    mod_tick_fn      tick;
    mod_exit_fn      exit;
    mod_on_reload_fn on_reload; /* optional */

} module_api_t;

/*==============================================================================================
    DLL export typedefs
==============================================================================================*/

/* Lifecycle struct — system calls this once at load time. */
typedef module_api_t* ( *get_module_api_fn )( void );

/* Typed API struct — returned to callers via module_sys_api_t::get_api(). */
typedef void* ( *get_api_fn )( void );

/*============================================================================================*/
#endif    // MODULE_API_H