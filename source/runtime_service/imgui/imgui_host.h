#ifndef IMGUI_HOST_H
#define IMGUI_HOST_H
/*==============================================================================================

    runtime_service/imgui/imgui_host.h -- Host-only imgui services.
    Includes imgui_api.h.

==============================================================================================*/

#include "runtime_service/imgui/imgui_api.h"

/*==============================================================================================
    Module Descriptor

    Used by the host to register the imgui module:
        mod_static_load( "imgui", imgui_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_load( imgui );

==============================================================================================*/

mod_desc_t* imgui_get_mod_desc( void );

/*==============================================================================================
    Direct-call functions (host and sandbox use only)
==============================================================================================*/

void imgui_tick( float dt );    /* TODO: replace with real direct-call functions */

/*============================================================================================*/
#endif    // IMGUI_HOST_H
