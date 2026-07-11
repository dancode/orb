#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H
/*==============================================================================================

    engine/net/net_internal.h -- transport internals shared by the net unity build.

    Everything here is private to net.c's translation unit; the implementation files are
    unity-included in dependency order and cross-call through the static prototypes below.

==============================================================================================*/

#include "engine/net/net.h"

/*==============================================================================================
    Wire protocol

    Every packet:            [u32 crc][u8 type][...]
    NET_PKT_CONNECT_REQUEST  [hdr][u64 client_salt][zero pad to max_packet_bytes]
    NET_PKT_CHALLENGE        [hdr][u64 client_salt][u64 server_salt]
    NET_PKT_CHALLENGE_REPLY  [hdr][u64 salt_xor][zero pad to max_packet_bytes]
    NET_PKT_ACCEPT           [hdr][u64 salt_xor]
    NET_PKT_DENY             [hdr][u64 client_salt][u8 reason]
    NET_PKT_PAYLOAD          [hdr][u64 salt_xor][u16 seq][u16 ack][u32 ack_bits]
                             [message records ...][u8 NET_MSG_END]
    NET_PKT_DISCONNECT       [hdr][u64 salt_xor]

    Message records inside a payload:
        unreliable   [u8 tag][varint size][size bytes]
        sequenced    [u8 tag][varint msg_id][varint size][size bytes]  (newest id wins)
        reliable     [u8 tag][varint msg_id][varint size][size bytes]
    where tag = channel index | NET_TAG_FRAGMENT | NET_TAG_LAST. A reliable message larger
    than one packet ships as consecutive reliable fragments; in-order delivery makes
    reassembly a plain concatenation. Both ends must agree on channel types -- the record
    shape is decided by the receiver's config.

    The crc32 is seeded with the protocol id (which is never sent) and covers the whole
    datagram with the crc field zeroed -- a wrong-protocol or corrupt packet fails the
    check and is silently ignored. Connect request/reply are padded so an attacker gains
    no amplification from spoofed handshakes. salt_xor = client_salt ^ server_salt
    authenticates the established session address cheaply.

    Reliability: packets are never retransmitted. Each payload header acks the newest
    remote packet plus a 32-packet history bitfield; a reliable *message* is re-packed
    into a fresh packet until some packet that carried it is acked.

==============================================================================================*/

typedef enum net_pkt_type_e
{
    NET_PKT_CONNECT_REQUEST = 1,
    NET_PKT_CHALLENGE,
    NET_PKT_CHALLENGE_REPLY,
    NET_PKT_ACCEPT,
    NET_PKT_DENY,
    NET_PKT_PAYLOAD,
    NET_PKT_DISCONNECT,

} net_pkt_type_t;

typedef enum net_deny_reason_e
{
    NET_DENY_FULL = 1,

} net_deny_reason_t;

#define NET_PKT_HEADER_BYTES     5     // crc + type
#define NET_PAYLOAD_HEADER_BYTES 21    // crc + type + salt_xor + seq + ack + ack_bits
#define NET_MSG_END              0xFF  // message-list terminator inside a payload

#define NET_TAG_FRAGMENT 0x80          // record is one fragment of a larger message
#define NET_TAG_LAST     0x40          // final fragment -- deliver the reassembly
#define NET_TAG_CHANNEL  0x0F          // channel index bits

#define NET_CHALLENGE_MAX         64    // pending-handshake table size (server)
#define NET_CHALLENGE_TIMEOUT     5.0   // seconds before a pending handshake entry expires
#define NET_EVENT_CAP             4096  // events queued per update
#define NET_PACKETS_PER_UPDATE    64    // per-connection payload packet cap per update
#define NET_DISCONNECT_REDUNDANCY 3     // fire-and-forget disconnect packet count

#define NET_REL_WINDOW  256    // reliable messages in flight / buffered out-of-order, per channel
#define NET_SENT_WINDOW 512    // sent-packet ack tracking ring, per connection
#define NET_SIM_QUEUE   512    // condition-simulator delayed packet slots (allocated on enable)

/*==============================================================================================
    Channel state
==============================================================================================*/

/* One queued reliable message on the send side. Bytes live in net_chan_t.send_data. */
typedef struct net_rel_out_s
{
    bool used;
    bool acked;
    bool sent_once;
    u8   tag;
    u16  id;
    u16  last_sent_seq;    // packet that most recently carried this message
    f64  last_send_time;
    i32  offset;
    i32  size;

} net_rel_out_t;

/* One out-of-order buffered reliable message on the receive side (bytes in recv_data). */
typedef struct net_rel_in_s
{
    bool used;
    u8   tag;
    i32  offset;
    i32  size;

} net_rel_in_t;

typedef struct net_chan_s
{
    /* Send storage. Unreliable: [u16 size][bytes] records, fully drained every update.
       Reliable: raw message bytes addressed by out[] slots -- a FIFO freed by acks;
       send_head counts dead prefix bytes reclaimed by compaction on demand. */
    u8* send_data;
    i32 send_head;
    i32 send_used;
    i32 send_cap;

    net_rel_out_t out[ NET_REL_WINDOW ];
    u16           send_id;           // next reliable message id to assign
    u16           oldest_unacked;    // send window base

    /* Receive side (reliable channels only): out-of-order arrivals wait in recv_data
       until the gap heals; the whole store resets whenever no slot is buffered. */
    u8*          recv_data;
    i32          recv_used;
    i32          recv_cap;
    i32          in_count;
    net_rel_in_t in[ NET_REL_WINDOW ];
    u16          expected_id;        // next reliable id to deliver

    /* Fragment reassembly (reliable channels only). */
    u8*  frag_data;
    i32  frag_used;
    i32  frag_cap;
    bool frag_skip;                  // oversize stream: discard until the LAST fragment passes

    /* Sequenced channels: newest delivered id (0xFFFF before the first arrival). */
    u16 seq_in_latest;

} net_chan_t;

/*==============================================================================================
    Connection slot
==============================================================================================*/

typedef enum net_conn_state_e
{
    NET_CONN_FREE = 0,
    NET_CONN_CONNECTING,    // client: request sent, awaiting challenge
    NET_CONN_RESPONDING,    // client: challenge reply sent, awaiting accept
    NET_CONN_CONNECTED,

} net_conn_state_t;

/* Ack bookkeeping for one sent payload packet. */
typedef struct net_sent_entry_s
{
    bool used;
    bool acked;
    u16  seq;
    f64  send_time;

} net_sent_entry_t;

typedef struct net_conn_slot_s
{
    u8         state;                  // net_conn_state_t
    u16        generation;             // bumped on free; part of the public handle
    sys_addr_t addr;
    u64        client_salt;
    u64        server_salt;            // 0 until the challenge round-trip
    bool       is_outgoing;            // we initiated (client side)

    f64 last_recv_time;
    f64 last_send_time;
    f64 connect_start_time;            // outgoing: when peer_connect was called
    f64 last_handshake_send_time;

    u16 seq_local;                     // next outgoing payload sequence
    u16 seq_remote;                    // highest payload sequence seen
    u32 recv_bits;                     // arrival bitfield for seq_remote-1 .. seq_remote-32

    f32              rtt;              // smoothed; 0 until the first sample
    f64              bw_tokens;        // token bucket for the outgoing bandwidth cap
    f64              bw_last_time;     // last refill (0 = bucket not started)
    net_sent_entry_t sent[ NET_SENT_WINDOW ];

    net_chan_t  channels[ NET_MAX_CHANNELS ];
    net_stats_t stats;

} net_conn_slot_t;

/* Pending server-side handshake, before any connection slot is committed. */
typedef struct net_challenge_s
{
    bool       used;
    sys_addr_t addr;
    u64        client_salt;
    u64        server_salt;
    f64        time;

} net_challenge_t;

/*==============================================================================================
    Peer
==============================================================================================*/

/* One delayed packet held by the condition simulator. */
typedef struct net_sim_pkt_s
{
    bool       used;
    i32        size;
    f64        deliver_time;
    sys_addr_t to;
    u8         data[ NET_PACKET_CAP ];

} net_sim_pkt_t;

struct net_peer_s
{
    net_config_t cfg;              // defaults resolved at create
    sys_socket_t socket;
    f64          now;              // time of the current/most recent peer_update
    u64          rng;              // xorshift state for salts and the simulator

    net_conn_slot_t* conns;        // [cfg.max_connections]
    net_challenge_t  challenges[ NET_CHALLENGE_MAX ];

    /* Events + received-message storage, reset at the start of every update. */
    net_event_t* events;
    i32          event_count;
    i32          event_read;
    u8*          recv_arena;
    i32          recv_arena_used;

    net_sim_t      sim;
    net_sim_pkt_t* sim_queue;      // NULL until the simulator is first enabled

    /* Scratch wire buffers (word-padded for the bit reader). */
    u8 pkt_out[ NET_PACKET_CAP + 4 ];
    u8 pkt_in[ NET_PACKET_CAP + 4 ];
};

/*==============================================================================================
    Internal cross-file prototypes (single unity TU)
==============================================================================================*/

/* net_packet.c */
static u32  net_crc32( u32 seed, const void* data, i32 size );
static u32  net_packet_crc( net_peer_t* p, const u8* packet, i32 size );
static bool net_seq_greater( u16 a, u16 b );
static bool net_seq_less( u16 a, u16 b );
static u64  net_random_u64( net_peer_t* p );
static f32  net_random_f32( net_peer_t* p );
static void net_packet_send( net_peer_t* p, net_conn_slot_t* conn, const sys_addr_t* to, i32 size );
static void net_packet_begin( net_peer_t* p, bit_writer_t* w, u8 type );
static i32  net_packet_end( net_peer_t* p, bit_writer_t* w );
static void net_packet_pad( net_peer_t* p, bit_writer_t* w );
static void net_sim_drain( net_peer_t* p );

/* net_conn.c */
static net_conn_slot_t* net_conn_alloc( net_peer_t* p );
static void             net_connect_request_send( net_peer_t* p, net_conn_slot_t* conn );
static net_conn_slot_t* net_conn_by_addr( net_peer_t* p, const sys_addr_t* addr );
static net_conn_t       net_conn_handle( net_peer_t* p, net_conn_slot_t* conn );
static net_conn_slot_t* net_conn_resolve( net_peer_t* p, net_conn_t handle );
static void             net_conn_free( net_peer_t* p, net_conn_slot_t* conn );
static void             net_conn_touch_recv( net_peer_t* p, net_conn_slot_t* conn );
static void             net_handshake_receive( net_peer_t* p, bit_reader_t* r, u8 type,
                                               const sys_addr_t* from, i32 packet_size );
static void             net_disconnect_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from );
static void             net_conn_tick( net_peer_t* p, net_conn_slot_t* conn );
static void             net_conn_send_disconnect( net_peer_t* p, net_conn_slot_t* conn );

/* net_channel.c */
static void net_payload_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from, i32 packet_size );
static void net_channels_flush( net_peer_t* p, net_conn_slot_t* conn );
static void net_keepalive_send( net_peer_t* p, net_conn_slot_t* conn );
static i32  net_msg_max_bytes( net_peer_t* p );
static i32  net_rel_msg_max_bytes( net_peer_t* p );
static bool net_rel_enqueue( net_peer_t* p, net_conn_slot_t* conn, i32 channel, const void* data, i32 size );

/* net_peer.c */
static void net_event_push( net_peer_t* p, u8 type, net_conn_t conn, u8 channel, const void* data, i32 size );
static u8*  net_recv_arena_alloc( net_peer_t* p, i32 size );

/*============================================================================================*/
#endif    // NET_INTERNAL_H
