#ifndef NET_API_H
#define NET_API_H
/*==============================================================================================

    engine/net/net_api.h -- net module API struct and gateway macro.
    net is always statically linked into the host.

==============================================================================================*/

#include "engine/net/net.h"
#include "engine/mod/mod_import.h"

/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct net_api_s
{
    /* Create a peer from `cfg` (zeroed fields take defaults -- see net_config_t).
       One allocation; fixed pools thereafter. NULL on socket or memory failure. */
    net_peer_t* ( *peer_create )( const net_config_t* cfg );

    /* Sends fire-and-forget disconnects to live connections and releases everything. */
    void ( *peer_destroy )( net_peer_t* p );

    /* The socket's bound address -- resolves the actual port after an ephemeral bind. */
    bool ( *peer_addr )( net_peer_t* p, sys_addr_t* out );

    /* Begin an outgoing handshake. Returns a handle immediately; CONNECTED or
       DISCONNECTED (timeout/denial) arrives later as an event. */
    net_conn_t ( *peer_connect )( net_peer_t* p, const sys_addr_t* addr );

    /* Notify the remote end (fire-and-forget) and free the connection. The handle dies
       immediately; no local DISCONNECTED event is emitted for a caller-initiated close. */
    void ( *peer_disconnect )( net_peer_t* p, net_conn_t conn );

    /* Pump the peer: drain the socket, run handshakes/timeouts, flush send queues,
       refill the event queue. Call once per tick; poll all events after each call --
       the previous update's events and message payloads are invalidated here. */
    void ( *peer_update )( net_peer_t* p, f64 now_seconds );

    /* Drain one event. MESSAGE data points into peer memory, valid until the next update. */
    bool ( *peer_poll )( net_peer_t* p, net_event_t* ev );

    /* Queue a message on `channel`. Copies the data; delivery per the channel type.
       False when the handle is dead, the channel is bad, or the queue is full. */
    bool ( *peer_send )( net_peer_t* p, net_conn_t conn, i32 channel, const void* data, i32 size );

    /* Connection transport statistics. False for a dead handle. */
    bool ( *peer_stats )( net_peer_t* p, net_conn_t conn, net_stats_t* out );

    /* Install (or clear with NULL) the outgoing-packet condition simulator. Dev only. */
    void ( *peer_sim )( net_peer_t* p, const net_sim_t* sim );

} net_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( NET_STATIC )
    MOD_GATEWAY_STATIC( net_api_t, net )
    #define MOD_USE_NET    /* static build */
    #define MOD_FETCH_NET  true
#else
    MOD_GATEWAY_DYNAMIC( net_api_t, net )
    #define MOD_USE_NET    MOD_DEFINE_API_PTR( net_api_t, net )
    #define MOD_FETCH_NET  MOD_FETCH_API( net_api_t, net )
#endif

/*============================================================================================*/
#endif    // NET_API_H
