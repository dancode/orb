#ifndef NET_H
#define NET_H
/*==============================================================================================

    engine/net/net.h -- net module types.
    Include in DLL modules that use net through the vtable (net()->...).
    Include net_host.h instead for direct-call access (host, sandbox).

    The transport model: a net_peer_t owns one UDP socket and a fixed set of connection
    slots. A client is a peer with one outgoing connection; a server is a listening peer
    with many. Connections handshake with a salt challenge (spoof-safe), then exchange
    messages on typed channels. The caller pumps peer_update() once per tick and drains
    peer_poll() events after each update; everything is single-threaded by contract.

==============================================================================================*/

#include "orb.h"
#include "engine/sys/sys.h"
#include "engine/net/net_bit.h"

/*==============================================================================================
    Limits
==============================================================================================*/

#define NET_MAX_CHANNELS 8
#define NET_PACKET_CAP   1400    // hard wire-packet ceiling; net_config_t.max_packet_bytes <= this

/*==============================================================================================
    Channels
==============================================================================================*/

typedef enum net_channel_type_e
{
    NET_CHANNEL_UNRELIABLE = 0,       // fire and forget; lost or late packets simply drop
    NET_CHANNEL_UNRELIABLE_SEQUENCED, // newest-only: stale arrivals are dropped (snapshots)
    NET_CHANNEL_RELIABLE_ORDERED,     // delivered exactly once, in order (events, chat, setup)

} net_channel_type_t;

/*==============================================================================================
    Connection handle -- u16 slot | u16 generation; stale handles fail safely
==============================================================================================*/

typedef u32 net_conn_t;

#define NET_CONN_INVALID ( ( net_conn_t )0 )    // generations start at 1, so 0 never matches

/*==============================================================================================
    Peer configuration
==============================================================================================*/

typedef struct net_config_s
{
    u32        protocol_id;                  // app + version magic; both ends must match
    sys_addr_t bind_addr;                    // zeroed = any ipv4 interface, ephemeral port
    bool       listen;                       // accept incoming connections (server)
    u16        max_connections;              // connection slots (0 defaults to 1)
    i32        channel_count;                // 1..NET_MAX_CHANNELS (0 defaults to 1 unreliable)
    u8         channels[ NET_MAX_CHANNELS ]; // net_channel_type_t per channel

    /* Tuning -- zero means default. */
    f32 timeout_seconds;           // drop a silent connection (default 10)
    f32 keepalive_seconds;         // idle ping cadence (default 0.25)
    f32 connect_resend_seconds;    // handshake retry cadence (default 0.5)
    f32 connect_timeout_seconds;   // give up on an unanswered connect (default 6)
    i32 max_packet_bytes;          // wire MTU budget (default 1200, clamped to NET_PACKET_CAP)
    i32 send_queue_bytes;          // per-connection per-channel send queue (default 64 KB)
    i32 recv_arena_bytes;          // per-peer arena for one update's received messages (default 256 KB)
    i32 max_message_bytes;         // largest reliable message, fragmented on the wire (default 64 KB)
    i32 send_bandwidth_bytes;      // per-connection outgoing cap in bytes/second (0 = unlimited)

} net_config_t;

/*==============================================================================================
    Events -- drained with peer_poll() after each peer_update()
==============================================================================================*/

typedef enum net_event_type_e
{
    NET_EVENT_NONE = 0,
    NET_EVENT_CONNECTED,       // handshake completed (both sides)
    NET_EVENT_DISCONNECTED,    // explicit disconnect, denial, or timeout; conn handle is dead
    NET_EVENT_MESSAGE,         // one received message

} net_event_type_t;

typedef struct net_event_s
{
    u8          type;       // net_event_type_t
    u8          channel;    // MESSAGE only
    net_conn_t  conn;
    const void* data;       // MESSAGE only; valid until the next peer_update()
    i32         size;       // MESSAGE only

} net_event_t;

/*==============================================================================================
    Stats and network condition simulation
==============================================================================================*/

typedef struct net_stats_s
{
    f32 rtt_seconds;           // smoothed round-trip estimate (0 until measured)
    f32 packet_loss;           // 0..1 over the recent ack window
    u64 packets_sent, packets_received;
    u64 bytes_sent, bytes_received;
    u64 messages_sent, messages_received;
    u64 messages_dropped;      // send-queue overruns and oversize rejects

} net_stats_t;

/* Dev-only outgoing-packet conditioner, applied at the socket seam of a peer. */
typedef struct net_sim_s
{
    f32 loss;                   // 0..1 chance a packet is dropped
    f32 duplicate;              // 0..1 chance a packet is sent twice
    f32 latency_min_seconds;    // uniform per-packet delay range
    f32 latency_max_seconds;

} net_sim_t;

/*==============================================================================================
    Peer -- opaque; created and driven through the API
==============================================================================================*/

typedef struct net_peer_s net_peer_t;

/*============================================================================================*/
#endif    // NET_H
