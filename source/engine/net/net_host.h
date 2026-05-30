#ifndef NET_HOST_H
#define NET_HOST_H
/*==============================================================================================

    engine/net/net_host.h -- Host-only net services.
    Includes net_api.h.

==============================================================================================*/

#include "engine/net/net_api.h"

/*==============================================================================================
    Module Descriptor

    Used by the host to register the net module:
        mod_static_load( "net", net_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_load( net );

==============================================================================================*/

mod_desc_t* net_get_mod_desc( void );

/*==============================================================================================
    Direct-call functions (host and sandbox use only)
==============================================================================================*/

void net_tick( float dt );    /* TODO: replace with real direct-call functions */

/*============================================================================================*/
#endif    // NET_HOST_H
