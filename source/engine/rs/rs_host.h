#ifndef RS_HOST_H
#define RS_HOST_H
/*==============================================================================================

    engine/rs/rs_host.h - rs_ module host interface.  Include in host executables only.
    Analogous to engine/mod/mod_host.h.

    Reflection wires up identically for static and dynamic modules. Each reflected module
    sets one slot in its mod_desc_t:

        #include "<name>.generated.h"          // declares <name>_rs_register

        static mod_desc_t s_<name>_mod_desc = {
            .func_api    = &g_<name>_api_struct,
            .rs_register = MOD_REFLECT_FUNC( <name> ),
            ...
        };

    The host installs one pair of mod-system callbacks via rs_wire_mod_callbacks(). From
    that point on, every module the host registers — static or dynamic, including
    hot-reload swaps — gets its types pushed into and popped from the rs registry
    automatically. No DLL symbol lookups, no separate static path, no per-module helpers.

    The rs registry self-bootstraps on first touch (see rs_ensure_init in rs_registry.c),
    so rs_wire_mod_callbacks can be called BEFORE rs.mod_init has run.

    Module loads are passive — the load callback does NOT fire from mod_static_load /
    mod_dynamic_load. It fires from mod_init_all() in DEPENDENCY ORDER, once per newly-
    loaded module, immediately before that module's init() runs. This means:

        - Reflection frame order on the rs stack matches the dep order. Dependencies are
          registered before dependents, so cross-module type references resolve cleanly.
        - A module's init() can already query its own reflected types AND those of any
          module it depends on.
        - Popping the top frame on hot-reload is always safe — by construction the
          reloading module sits above anything that depends on it.

    Host boot sequence:

        mod_system_init();
        rs_wire_mod_callbacks();              // install hooks; nothing fires yet
        mod_static_load( "sys", ... );        // PASSIVE — just registers the desc
        mod_static_load( "rs",  ... );        // PASSIVE
        mod_static_load( "run", ... );        // PASSIVE
        mod_static_load( "core", ... );       // PASSIVE
        load_all( user_modules );             // PASSIVE for every module
        mod_init_all();                       // pass 1: load cb in dep order (reflection)
                                              // pass 2: init() in same order

==============================================================================================*/

#include "engine/rs/rs.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"    /* mod_desc_t (carries the rs_register slot) */

/*==============================================================================================
    Lifecycle handlers — one signature, fires for static and dynamic modules alike.
==============================================================================================*/

static inline void
rs_host_on_pre_init( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( user );
    rs_register_module( name, desc );    /* no-op when desc->rs_register is NULL */
}

static inline void
rs_host_on_post_exit( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( desc );
    UNUSED( user );
    rs_unregister_module( name );        /* silent when no frame exists for `name` */
}

/*==============================================================================================
    rs_wire_mod_callbacks

    Install the pre_init / post_exit hooks. Safe to call immediately after
    mod_system_init() — the rs registry self-bootstraps on first reflected load,
    so there's no ordering dependency on rs.mod_init.
==============================================================================================*/

static inline void
rs_wire_mod_callbacks( void )
{
    mod_set_pre_init_cb ( rs_host_on_pre_init,  NULL );
    mod_set_post_exit_cb( rs_host_on_post_exit, NULL );
}

/*============================================================================================*/
#endif    // RS_HOST_H
