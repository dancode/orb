/*==============================================================================================

    engine/core/core_host.h - Host-only core API: module descriptor for static registration.
    Includes core.h.

==============================================================================================*/
#ifndef CORE_HOST_H
#define CORE_HOST_H

#include "engine/core/core_api.h"
#include "engine/mod/mod_host.h"

/*==============================================================================================

    Module Descriptor

    Used by the host to register the core module:
        mod_static_load( "core", core_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_load( core );

==============================================================================================*/

mod_desc_t* core_get_mod_desc( void );

/* Enable skip mode: ORB_ASSERT prints but does not trap. Used by test suites. */
void core_assert_set_skip( bool skip );

/* Log sink adapter: routes log_fn_t calls through core's logger.
   Pass this to mod_set_log_fn / app_set_log_fn after mod_init_all(). */
void core_log_fn( int level, const char* tag, const char* msg );

/*============================================================================================*/
#endif    // CORE_HOST_H
