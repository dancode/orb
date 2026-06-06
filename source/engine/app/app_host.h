/*==============================================================================================

    engine/app/app_host.h — Host-only app API: module descriptor for static registration.
    Includes app.h.

==============================================================================================*/
#ifndef APP_HOST_H
#define APP_HOST_H

#include "engine/app/app_api.h"
#include "engine/mod/mod_host.h"

/*==============================================================================================

    Module Descriptor

    Used by the host to register the app module:
        mod_static_load( "app", app_get_mod_desc() );
        mod_static( app );
    or via the build-mode-transparent macro:
        mod_load( app );

==============================================================================================*/

mod_desc_t* app_get_mod_desc( void );

/* Route app log output through core. Call after mod_init_all(). */
void app_set_log_fn( log_fn_t fn );

/*============================================================================================*/
#endif    // APP_HOST_H
