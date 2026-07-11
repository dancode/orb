/*==============================================================================================

    net_peer.c -- peer lifecycle and the per-tick update loop.

    A peer is one UDP socket plus fixed connection slots, event queue, and receive arena,
    all carved from a single allocation at create. peer_update() drains the socket,
    dispatches packets, ticks connection housekeeping (handshake resends, timeouts,
    send-queue flush, keepalive), and refills the event queue that peer_poll() drains.

==============================================================================================*/

#include <stdlib.h>

#include "engine/net/net_internal.h"

/*==============================================================================================
    Event queue + receive arena (reset every update; consumers copy what they keep)
==============================================================================================*/

static void
net_event_push( net_peer_t* p, u8 type, net_conn_t conn, u8 channel, const void* data, i32 size )
{
    if ( p->event_count >= NET_EVENT_CAP ) return;

    net_event_t* ev = &p->events[ p->event_count++ ];
    ev->type        = type;
    ev->channel     = channel;
    ev->conn        = conn;
    ev->data        = data;
    ev->size        = size;
}

static u8*
net_recv_arena_alloc( net_peer_t* p, i32 size )
{
    /* Word-align each block so bit_read_bytes lands on clean boundaries for consumers. */
    i32 aligned = ( size + 3 ) & ~3;
    if ( p->recv_arena_used + aligned > p->cfg.recv_arena_bytes ) return NULL;

    u8* block = p->recv_arena + p->recv_arena_used;
    p->recv_arena_used += aligned;
    return block;
}

/*==============================================================================================
    Create / destroy
==============================================================================================*/

static void
net_config_resolve( net_config_t* cfg )
{
    if ( cfg->max_connections == 0 ) cfg->max_connections = 1;
    if ( cfg->channel_count <= 0 )
    {
        cfg->channel_count = 1;
        cfg->channels[ 0 ] = NET_CHANNEL_UNRELIABLE;
    }
    if ( cfg->channel_count > NET_MAX_CHANNELS ) cfg->channel_count = NET_MAX_CHANNELS;

    if ( cfg->timeout_seconds <= 0 ) cfg->timeout_seconds = 10.0f;
    if ( cfg->keepalive_seconds <= 0 ) cfg->keepalive_seconds = 0.25f;
    if ( cfg->connect_resend_seconds <= 0 ) cfg->connect_resend_seconds = 0.5f;
    if ( cfg->connect_timeout_seconds <= 0 ) cfg->connect_timeout_seconds = 6.0f;

    if ( cfg->max_packet_bytes <= 0 ) cfg->max_packet_bytes = 1200;
    if ( cfg->max_packet_bytes > NET_PACKET_CAP ) cfg->max_packet_bytes = NET_PACKET_CAP;
    if ( cfg->max_packet_bytes < 256 ) cfg->max_packet_bytes = 256;

    if ( cfg->send_queue_bytes <= 0 ) cfg->send_queue_bytes = 64 * 1024;
    if ( cfg->recv_arena_bytes <= 0 ) cfg->recv_arena_bytes = 256 * 1024;
    if ( cfg->max_message_bytes <= 0 ) cfg->max_message_bytes = 64 * 1024;
    if ( cfg->max_message_bytes > cfg->send_queue_bytes ) cfg->max_message_bytes = cfg->send_queue_bytes;
}

/* Reliable channels carry three buffers (send FIFO, out-of-order store, reassembly);
   unreliable channels only the send queue. */
static usize
net_chan_buffer_bytes( const net_config_t* cfg, i32 ch )
{
    usize bytes = ( usize )cfg->send_queue_bytes;
    if ( cfg->channels[ ch ] == NET_CHANNEL_RELIABLE_ORDERED )
        bytes += ( usize )cfg->send_queue_bytes + ( usize )cfg->max_message_bytes;
    return bytes;
}

static net_peer_t*
net_peer_create_impl( const net_config_t* cfg_in )
{
    net_config_t cfg = *cfg_in;
    net_config_resolve( &cfg );

    /* One allocation: peer + slots + events + receive arena + all channel buffers. */
    usize conns_bytes  = sizeof( net_conn_slot_t ) * cfg.max_connections;
    usize events_bytes = sizeof( net_event_t ) * NET_EVENT_CAP;
    usize queues_bytes = 0;
    for ( i32 ch = 0; ch < cfg.channel_count; ch++ )
        queues_bytes += net_chan_buffer_bytes( &cfg, ch ) * cfg.max_connections;
    usize total = sizeof( net_peer_t ) + conns_bytes + events_bytes + ( usize )cfg.recv_arena_bytes +
                  queues_bytes;

    u8* memory = ( u8* )malloc( total );
    if ( !memory ) return NULL;
    memset( memory, 0, total );

    net_peer_t* p = ( net_peer_t* )memory;
    u8*         at = memory + sizeof( net_peer_t );

    p->cfg   = cfg;
    p->conns = ( net_conn_slot_t* )at;
    at += conns_bytes;
    p->events = ( net_event_t* )at;
    at += events_bytes;
    p->recv_arena = at;
    at += cfg.recv_arena_bytes;

    for ( i32 i = 0; i < cfg.max_connections; i++ )
    {
        p->conns[ i ].generation = 1;
        for ( i32 ch = 0; ch < cfg.channel_count; ch++ )
        {
            net_chan_t* chan = &p->conns[ i ].channels[ ch ];
            chan->send_data  = at;
            chan->send_cap   = cfg.send_queue_bytes;
            at += cfg.send_queue_bytes;

            if ( cfg.channels[ ch ] == NET_CHANNEL_RELIABLE_ORDERED )
            {
                chan->recv_data = at;
                chan->recv_cap  = cfg.send_queue_bytes;
                at += cfg.send_queue_bytes;
                chan->frag_data = at;
                chan->frag_cap  = cfg.max_message_bytes;
                at += cfg.max_message_bytes;
            }
        }
    }

    p->rng = ( u64 )sys_tick_nanoseconds() ^ ( ( u64 )( uintptr_t )p << 16 ) ^ 0x9E3779B97F4A7C15ull;

    const sys_addr_t* bind_addr = ( cfg.bind_addr.type != SYS_ADDR_NONE ) ? &p->cfg.bind_addr : NULL;
    p->socket                   = sys_socket_open_udp( bind_addr );
    if ( p->socket == SYS_SOCKET_INVALID )
    {
        free( memory );
        return NULL;
    }

    return p;
}

static void
net_peer_destroy_impl( net_peer_t* p )
{
    if ( !p ) return;

    /* Tell live peers we are going away; fire-and-forget. */
    for ( i32 i = 0; i < p->cfg.max_connections; i++ )
    {
        if ( p->conns[ i ].state == NET_CONN_CONNECTED ) net_conn_send_disconnect( p, &p->conns[ i ] );
    }

    sys_socket_close( p->socket );
    free( p->sim_queue );
    free( p );
}

static bool
net_peer_addr_impl( net_peer_t* p, sys_addr_t* out )
{
    return sys_socket_bound_addr( p->socket, out );
}

/*==============================================================================================
    Connect / disconnect
==============================================================================================*/

static net_conn_t
net_peer_connect_impl( net_peer_t* p, const sys_addr_t* addr )
{
    if ( !p || !addr || addr->type == SYS_ADDR_NONE ) return NET_CONN_INVALID;
    if ( net_conn_by_addr( p, addr ) ) return NET_CONN_INVALID;    /* one connection per address */

    net_conn_slot_t* conn = net_conn_alloc( p );
    if ( !conn ) return NET_CONN_INVALID;

    conn->state       = NET_CONN_CONNECTING;
    conn->addr        = *addr;
    conn->client_salt = net_random_u64( p );
    conn->server_salt = 0;
    conn->is_outgoing = true;

    /* p->now is only meaningful once peer_update has run; a connect issued before that
       (or long after the last update) must not age against a stale clock. The sentinel
       is resolved to the current time by the first conn tick. */
    conn->connect_start_time = -1.0;
    conn->last_recv_time     = p->now;

    net_connect_request_send( p, conn );
    return net_conn_handle( p, conn );
}

static void
net_peer_disconnect_impl( net_peer_t* p, net_conn_t handle )
{
    net_conn_slot_t* conn = net_conn_resolve( p, handle );
    if ( !conn ) return;

    /* No local DISCONNECTED event -- the caller initiated this and the handle dies here.
       Events only ever surface from peer_update (remote disconnects, denials, timeouts). */
    if ( conn->state == NET_CONN_CONNECTED ) net_conn_send_disconnect( p, conn );
    net_conn_free( p, conn );
}

/*==============================================================================================
    Update / poll
==============================================================================================*/

static void
net_peer_update_impl( net_peer_t* p, f64 now_seconds )
{
    if ( !p ) return;
    p->now = now_seconds;

    /* Last update's events and message payloads die here. */
    p->event_count     = 0;
    p->event_read      = 0;
    p->recv_arena_used = 0;

    /* Release condition-simulator packets whose artificial latency has matured. */
    net_sim_drain( p );

    /* Drain the socket. */
    for ( ;; )
    {
        sys_addr_t from;
        int        size = sys_socket_recv( p->socket, &from, p->pkt_in, p->cfg.max_packet_bytes );
        if ( size <= 0 ) break;
        if ( size < NET_PKT_HEADER_BYTES ) continue;

        u32 wire_crc;
        memcpy( &wire_crc, p->pkt_in, 4 );
        if ( wire_crc != net_packet_crc( p, p->pkt_in, size ) ) continue;    /* corrupt or foreign */

        bit_reader_t r;
        bit_reader_init( &r, p->pkt_in, size );
        bit_read_u32( &r );    /* crc, already checked */
        u8 type = bit_read_u8( &r );

        switch ( type )
        {
            case NET_PKT_PAYLOAD:    net_payload_receive( p, &r, &from, size ); break;
            case NET_PKT_DISCONNECT: net_disconnect_receive( p, &r, &from ); break;
            default:                 net_handshake_receive( p, &r, type, &from, size ); break;
        }
    }

    /* Housekeeping per connection: handshake resends, timeouts, queue flush, keepalive. */
    for ( i32 i = 0; i < p->cfg.max_connections; i++ )
    {
        if ( p->conns[ i ].state != NET_CONN_FREE ) net_conn_tick( p, &p->conns[ i ] );
    }
}

static bool
net_peer_poll_impl( net_peer_t* p, net_event_t* ev )
{
    if ( !p || p->event_read >= p->event_count ) return false;
    *ev = p->events[ p->event_read++ ];
    return true;
}

/*==============================================================================================
    Send / stats / sim
==============================================================================================*/

static bool
net_peer_send_impl( net_peer_t* p, net_conn_t handle, i32 channel, const void* data, i32 size )
{
    net_conn_slot_t* conn = net_conn_resolve( p, handle );
    if ( !conn || conn->state != NET_CONN_CONNECTED ) return false;
    if ( channel < 0 || channel >= p->cfg.channel_count ) return false;
    if ( !data || size <= 0 ) return false;

    /* Reliable: fragment as needed; a false return is backpressure, retry next tick. */
    if ( p->cfg.channels[ channel ] == NET_CHANNEL_RELIABLE_ORDERED )
        return net_rel_enqueue( p, conn, channel, data, size );

    /* Unreliable: must fit one packet, queue records are [u16 size][bytes]. */
    if ( size > net_msg_max_bytes( p ) )
    {
        conn->stats.messages_dropped++;
        return false;
    }

    net_chan_t* chan = &conn->channels[ channel ];
    if ( chan->send_used + 2 + size > chan->send_cap )
    {
        conn->stats.messages_dropped++;
        return false;
    }

    u16 size16 = ( u16 )size;
    memcpy( chan->send_data + chan->send_used, &size16, 2 );
    memcpy( chan->send_data + chan->send_used + 2, data, ( usize )size );
    chan->send_used += 2 + size;
    return true;
}

static bool
net_peer_stats_impl( net_peer_t* p, net_conn_t handle, net_stats_t* out )
{
    net_conn_slot_t* conn = net_conn_resolve( p, handle );
    if ( !conn || !out ) return false;

    *out              = conn->stats;
    out->rtt_seconds  = conn->rtt;

    /* Loss over the recent window: sent packets old enough that their ack should have
       arrived, young enough to still be in the ring. */
    i32 counted = 0, lost = 0;
    for ( i32 i = 0; i < NET_SENT_WINDOW; i++ )
    {
        const net_sent_entry_t* e   = &conn->sent[ i ];
        f64                     age = p->now - e->send_time;
        if ( !e->used || age < 0.3 || age > 3.0 ) continue;
        counted++;
        if ( !e->acked ) lost++;
    }
    out->packet_loss = counted ? ( f32 )lost / ( f32 )counted : 0.0f;
    return true;
}

static void
net_peer_sim_impl( net_peer_t* p, const net_sim_t* sim )
{
    static const net_sim_t k_sim_off = { 0 };
    p->sim = sim ? *sim : k_sim_off;

    bool active = p->sim.loss > 0 || p->sim.duplicate > 0 || p->sim.latency_max_seconds > 0;
    if ( active && !p->sim_queue )
    {
        p->sim_queue = ( net_sim_pkt_t* )malloc( sizeof( net_sim_pkt_t ) * NET_SIM_QUEUE );
        if ( p->sim_queue ) memset( p->sim_queue, 0, sizeof( net_sim_pkt_t ) * NET_SIM_QUEUE );
    }
}

/*============================================================================================*/
