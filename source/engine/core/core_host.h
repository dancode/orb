/*==============================================================================================

    engine/core/core_host.h - Host-only core API: module descriptor for static registration.
    Includes core.h.

==============================================================================================*/
#ifndef CORE_HOST_H
#define CORE_HOST_H

#include "engine/core/core_api.h"
#include "engine/mod/mod_host.h"
#include "engine/core/debug/crash.h" /* core_crash_install / core_crash_report_now */

/*==============================================================================================

    Module Descriptor

    Used by the host to register the core module:
        mod_static_load( "core", core_get_mod_desc() ); OR
        mod_static( core );
    or via the build-mode-transparent macro:
        mod_load( core );

==============================================================================================*/

//          Enable skip mode: ORB_ASSERT prints but does not trap. Used by test suites.
void        core_assert_set_skip( bool skip );

//          Log sink adapter: routes log_fn_t calls through core's logger.
//          Pass this to mod_set_log_fn / app_set_log_fn after mod_init_all().
void        core_log_fn( int level, const char* tag, const char* msg );

//          Register the 'log' console command (per-channel verbosity).  core_init calls
//          this; exposed for sandboxes that assemble core subsystems manually (sb_core).
void        log_register_commands( void );

/*==============================================================================================
    Mod Callbacks

    Host-side glue between core and the mod system (same pattern as ref_wire_mod_callbacks
    and core_log_fn -> mod_set_log_fn): the mod symbols are referenced only from this
    host-compiled inline, so core.lib itself never links against mod.

    Wires two things:
      - cvar callback registrations get stamped with their owning module id
      - a mod unload hook drops module-owned cvar callbacks before the DLL is released

    Call once at host startup, alongside ref_wire_mod_callbacks(). Safe before
    mod_init_all(); hosts without the mod system simply never call it.
==============================================================================================*/

static inline void
core_host_on_mod_unload( int32_t module_id, const char* name )
{
    UNUSED( name );
    cvar_callback_unregister_by_module( module_id );
}

static inline void
core_wire_mod_callbacks( void )
{
    cvar_set_module_id_fn( mod_current_id );
    mod_unload_hook_register( core_host_on_mod_unload );
}

/*============================================================================================*/
#endif    // CORE_HOST_H
