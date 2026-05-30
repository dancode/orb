#ifndef NET_HOST_H
#define NET_HOST_H
/*==============================================================================================

    engine/net/net_host.h -- Host-only net services.
    Includes net_api.h.

==============================================================================================*/

#include "engine/net/net_api.h"

/*==============================================================================================
    Direct-call functions (host and sandbox use only)
==============================================================================================*/

// void        net_tick( float dt );    /* TODO: replace with real direct-call functions */
mod_desc_t* net_get_mod_desc( void );

/*============================================================================================*/
#endif    // NET_HOST_H
