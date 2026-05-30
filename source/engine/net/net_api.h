#ifndef NET_API_H
#define NET_API_H
/*==============================================================================================

    engine/net/net_api.h -- net module API struct and gateway macro.
    always statically linked into the host.

==============================================================================================*/

#include "engine/net/net.h"
#include "engine/mod/mod_import.h"

/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct net_api_s
{
    void ( *tick )( float dt );    /* TODO: replace with real API functions */

} net_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( NET_STATIC )
    MOD_GATEWAY_STATIC( net_api_t, net )
#else
    MOD_GATEWAY_DYNAMIC( net_api_t, net )
#endif

#if defined( BUILD_STATIC ) || defined( NET_STATIC )
    #define MOD_USE_NET    /* static build */
    #define MOD_FETCH_NET  true
#else
    #define MOD_USE_NET    MOD_DEFINE_API_PTR( net_api_t, net )
    #define MOD_FETCH_NET  MOD_FETCH_API( net_api_t, net )
#endif

/*============================================================================================*/
#endif    // NET_API_H
