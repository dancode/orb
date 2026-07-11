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

    Twins of the net_api_t vtable -- see net_api.h for the contracts.

==============================================================================================*/

net_peer_t* net_peer_create( const net_config_t* cfg );
void        net_peer_destroy( net_peer_t* p );
bool        net_peer_addr( net_peer_t* p, sys_addr_t* out );
net_conn_t  net_peer_connect( net_peer_t* p, const sys_addr_t* addr );
void        net_peer_disconnect( net_peer_t* p, net_conn_t conn );
void        net_peer_update( net_peer_t* p, f64 now_seconds );
bool        net_peer_poll( net_peer_t* p, net_event_t* ev );
bool        net_peer_send( net_peer_t* p, net_conn_t conn, i32 channel, const void* data, i32 size );
bool        net_peer_stats( net_peer_t* p, net_conn_t conn, net_stats_t* out );
void        net_peer_sim( net_peer_t* p, const net_sim_t* sim );

/*============================================================================================*/
#endif    // NET_HOST_H
