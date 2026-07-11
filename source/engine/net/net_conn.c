/*==============================================================================================

    net_conn.c -- connection lifecycle: salt handshake (both sides), slot management,
    keepalive, timeout, and disconnect.

    Handshake:  client CONNECT_REQUEST (padded, carries client_salt)
             -> server CHALLENGE (echoes client_salt, adds server_salt; no slot committed)
             -> client CHALLENGE_REPLY (padded, proves it saw both salts)
             -> server commits a slot, replies ACCEPT (or DENY when full)

    The server stores pending handshakes in a small challenge table, so a spoofed address
    can never pin a connection slot. Every post-handshake packet carries salt_xor.

==============================================================================================*/

#include "engine/net/net_internal.h"

/*==============================================================================================
    Slot management
==============================================================================================*/

static net_conn_slot_t*
net_conn_by_addr( net_peer_t* p, const sys_addr_t* addr )
{
    for ( i32 i = 0; i < p->cfg.max_connections; i++ )
    {
        net_conn_slot_t* conn = &p->conns[ i ];
        if ( conn->state != NET_CONN_FREE && sys_addr_equal( &conn->addr, addr ) ) return conn;
    }
    return NULL;
}

/* Reset per-session channel state; buffer pointers and capacities survive across sessions. */
static void
net_chan_reset( net_chan_t* chan )
{
    chan->send_head      = 0;
    chan->send_used      = 0;
    chan->send_id        = 0;
    chan->oldest_unacked = 0;
    chan->recv_used      = 0;
    chan->in_count       = 0;
    chan->expected_id    = 0;
    chan->frag_used      = 0;
    chan->frag_skip      = false;
    chan->seq_in_latest  = 0xFFFF;    /* wrap math accepts id 0 as newer */
    memset( chan->out, 0, sizeof( chan->out ) );
    memset( chan->in, 0, sizeof( chan->in ) );
}

static net_conn_slot_t*
net_conn_alloc( net_peer_t* p )
{
    for ( i32 i = 0; i < p->cfg.max_connections; i++ )
    {
        net_conn_slot_t* conn = &p->conns[ i ];
        if ( conn->state != NET_CONN_FREE ) continue;

        /* Reset session state; generation and channel buffer wiring survive. */
        memset( &conn->addr, 0, sizeof( conn->addr ) );
        conn->client_salt              = 0;
        conn->server_salt              = 0;
        conn->is_outgoing              = false;
        conn->last_recv_time           = 0;
        conn->last_send_time           = 0;
        conn->connect_start_time       = 0;
        conn->last_handshake_send_time = 0;
        conn->seq_local                = 0;
        conn->seq_remote               = 0;
        conn->recv_bits                = 0;
        conn->rtt                      = 0;
        conn->bw_tokens                = 0;
        conn->bw_last_time             = 0;
        memset( conn->sent, 0, sizeof( conn->sent ) );
        memset( &conn->stats, 0, sizeof( conn->stats ) );
        for ( i32 ch = 0; ch < NET_MAX_CHANNELS; ch++ ) net_chan_reset( &conn->channels[ ch ] );
        return conn;
    }
    return NULL;
}

static void
net_conn_free( net_peer_t* p, net_conn_slot_t* conn )
{
    UNUSED( p );
    conn->state = NET_CONN_FREE;
    conn->generation++;    /* invalidates outstanding handles */
    if ( conn->generation == 0 ) conn->generation = 1;
}

/* Public handle <-> slot. Generation 0 is never issued, so a zero handle never resolves. */
static net_conn_t
net_conn_handle( net_peer_t* p, net_conn_slot_t* conn )
{
    return ( ( net_conn_t )conn->generation << 16 ) | ( net_conn_t )( conn - p->conns );
}

static net_conn_slot_t*
net_conn_resolve( net_peer_t* p, net_conn_t handle )
{
    u16 slot       = ( u16 )( handle & 0xFFFF );
    u16 generation = ( u16 )( handle >> 16 );
    if ( slot >= p->cfg.max_connections ) return NULL;

    net_conn_slot_t* conn = &p->conns[ slot ];
    if ( conn->state == NET_CONN_FREE || conn->generation != generation ) return NULL;
    return conn;
}

static void
net_conn_touch_recv( net_peer_t* p, net_conn_slot_t* conn )
{
    conn->last_recv_time = p->now;
}

/*==============================================================================================
    Outgoing handshake packets
==============================================================================================*/

static void
net_connect_request_send( net_peer_t* p, net_conn_slot_t* conn )
{
    bit_writer_t w;
    net_packet_begin( p, &w, NET_PKT_CONNECT_REQUEST );
    bit_write_u64( &w, conn->client_salt );
    net_packet_pad( p, &w );
    net_packet_send( p, conn, &conn->addr, net_packet_end( p, &w ) );
    conn->last_handshake_send_time = p->now;
}

static void
net_challenge_reply_send( net_peer_t* p, net_conn_slot_t* conn )
{
    bit_writer_t w;
    net_packet_begin( p, &w, NET_PKT_CHALLENGE_REPLY );
    bit_write_u64( &w, conn->client_salt ^ conn->server_salt );
    net_packet_pad( p, &w );
    net_packet_send( p, conn, &conn->addr, net_packet_end( p, &w ) );
    conn->last_handshake_send_time = p->now;
}

static void
net_accept_send( net_peer_t* p, net_conn_slot_t* conn )
{
    bit_writer_t w;
    net_packet_begin( p, &w, NET_PKT_ACCEPT );
    bit_write_u64( &w, conn->client_salt ^ conn->server_salt );
    net_packet_send( p, conn, &conn->addr, net_packet_end( p, &w ) );
}

static void
net_deny_send( net_peer_t* p, const sys_addr_t* to, u64 client_salt, u8 reason )
{
    bit_writer_t w;
    net_packet_begin( p, &w, NET_PKT_DENY );
    bit_write_u64( &w, client_salt );
    bit_write_u8( &w, reason );
    net_packet_send( p, NULL, to, net_packet_end( p, &w ) );
}

static void
net_conn_send_disconnect( net_peer_t* p, net_conn_slot_t* conn )
{
    bit_writer_t w;
    for ( i32 i = 0; i < NET_DISCONNECT_REDUNDANCY; i++ )
    {
        net_packet_begin( p, &w, NET_PKT_DISCONNECT );
        bit_write_u64( &w, conn->client_salt ^ conn->server_salt );
        net_packet_send( p, conn, &conn->addr, net_packet_end( p, &w ) );
    }
}

/*==============================================================================================
    Server-side handshake receive
==============================================================================================*/

static net_challenge_t*
net_challenge_find( net_peer_t* p, const sys_addr_t* addr )
{
    for ( i32 i = 0; i < NET_CHALLENGE_MAX; i++ )
    {
        net_challenge_t* c = &p->challenges[ i ];
        if ( c->used && sys_addr_equal( &c->addr, addr ) ) return c;
    }
    return NULL;
}

static net_challenge_t*
net_challenge_alloc( net_peer_t* p, const sys_addr_t* addr )
{
    net_challenge_t* oldest = &p->challenges[ 0 ];
    for ( i32 i = 0; i < NET_CHALLENGE_MAX; i++ )
    {
        net_challenge_t* c = &p->challenges[ i ];
        if ( !c->used || p->now - c->time > NET_CHALLENGE_TIMEOUT )
        {
            oldest = c;
            break;
        }
        if ( c->time < oldest->time ) oldest = c;
    }
    oldest->used = true;
    oldest->addr = *addr;
    return oldest;
}

static void
net_connect_request_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from, i32 packet_size )
{
    if ( !p->cfg.listen ) return;
    if ( packet_size < p->cfg.max_packet_bytes ) return;    /* unpadded: refuse to amplify */

    u64 client_salt = bit_read_u64( r );
    if ( !bit_reader_ok( r ) ) return;

    /* Duplicate request from an established connection: the ACCEPT was lost. */
    net_conn_slot_t* existing = net_conn_by_addr( p, from );
    if ( existing )
    {
        if ( existing->state == NET_CONN_CONNECTED && existing->client_salt == client_salt )
            net_accept_send( p, existing );
        return;
    }

    net_challenge_t* challenge = net_challenge_find( p, from );
    if ( !challenge || challenge->client_salt != client_salt )
    {
        challenge              = net_challenge_alloc( p, from );
        challenge->client_salt = client_salt;
        challenge->server_salt = net_random_u64( p );
    }
    challenge->time = p->now;

    bit_writer_t w;
    net_packet_begin( p, &w, NET_PKT_CHALLENGE );
    bit_write_u64( &w, challenge->client_salt );
    bit_write_u64( &w, challenge->server_salt );
    net_packet_send( p, NULL, from, net_packet_end( p, &w ) );
}

static void
net_challenge_reply_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from, i32 packet_size )
{
    if ( !p->cfg.listen ) return;
    if ( packet_size < p->cfg.max_packet_bytes ) return;

    u64 salt_xor = bit_read_u64( r );
    if ( !bit_reader_ok( r ) ) return;

    /* Duplicate reply after the slot was committed: re-send the (lost) ACCEPT. */
    net_conn_slot_t* existing = net_conn_by_addr( p, from );
    if ( existing )
    {
        if ( existing->state == NET_CONN_CONNECTED &&
             ( existing->client_salt ^ existing->server_salt ) == salt_xor )
            net_accept_send( p, existing );
        return;
    }

    net_challenge_t* challenge = net_challenge_find( p, from );
    if ( !challenge || ( challenge->client_salt ^ challenge->server_salt ) != salt_xor ) return;

    net_conn_slot_t* conn = net_conn_alloc( p );
    if ( !conn )
    {
        net_deny_send( p, from, challenge->client_salt, NET_DENY_FULL );
        challenge->used = false;
        return;
    }

    conn->state          = NET_CONN_CONNECTED;
    conn->addr           = *from;
    conn->client_salt    = challenge->client_salt;
    conn->server_salt    = challenge->server_salt;
    conn->is_outgoing    = false;
    conn->last_recv_time = p->now;
    conn->last_send_time = p->now;
    challenge->used      = false;

    net_accept_send( p, conn );
    net_event_push( p, NET_EVENT_CONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
}

/*==============================================================================================
    Client-side handshake receive
==============================================================================================*/

static void
net_challenge_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from )
{
    u64 client_salt = bit_read_u64( r );
    u64 server_salt = bit_read_u64( r );
    if ( !bit_reader_ok( r ) ) return;

    net_conn_slot_t* conn = net_conn_by_addr( p, from );
    if ( !conn || !conn->is_outgoing || conn->client_salt != client_salt ) return;
    if ( conn->state != NET_CONN_CONNECTING && conn->state != NET_CONN_RESPONDING ) return;

    conn->server_salt = server_salt;
    conn->state       = NET_CONN_RESPONDING;
    net_conn_touch_recv( p, conn );
    net_challenge_reply_send( p, conn );
}

static void
net_accept_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from )
{
    u64 salt_xor = bit_read_u64( r );
    if ( !bit_reader_ok( r ) ) return;

    net_conn_slot_t* conn = net_conn_by_addr( p, from );
    if ( !conn || !conn->is_outgoing || conn->state != NET_CONN_RESPONDING ) return;
    if ( ( conn->client_salt ^ conn->server_salt ) != salt_xor ) return;

    conn->state = NET_CONN_CONNECTED;
    net_conn_touch_recv( p, conn );
    net_event_push( p, NET_EVENT_CONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
}

static void
net_deny_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from )
{
    u64 client_salt = bit_read_u64( r );
    u8  reason      = bit_read_u8( r );
    UNUSED( reason );
    if ( !bit_reader_ok( r ) ) return;

    net_conn_slot_t* conn = net_conn_by_addr( p, from );
    if ( !conn || !conn->is_outgoing || conn->client_salt != client_salt ) return;
    if ( conn->state == NET_CONN_CONNECTED ) return;    /* denies cannot kill a live session */

    net_event_push( p, NET_EVENT_DISCONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
    net_conn_free( p, conn );
}

/*==============================================================================================
    Shared receive dispatch + tick
==============================================================================================*/

static void
net_handshake_receive( net_peer_t* p, bit_reader_t* r, u8 type, const sys_addr_t* from, i32 packet_size )
{
    switch ( type )
    {
        case NET_PKT_CONNECT_REQUEST: net_connect_request_receive( p, r, from, packet_size ); break;
        case NET_PKT_CHALLENGE:       net_challenge_receive( p, r, from ); break;
        case NET_PKT_CHALLENGE_REPLY: net_challenge_reply_receive( p, r, from, packet_size ); break;
        case NET_PKT_ACCEPT:          net_accept_receive( p, r, from ); break;
        case NET_PKT_DENY:            net_deny_receive( p, r, from ); break;
        default: break;
    }
}

static void
net_disconnect_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from )
{
    u64 salt_xor = bit_read_u64( r );
    if ( !bit_reader_ok( r ) ) return;

    net_conn_slot_t* conn = net_conn_by_addr( p, from );
    if ( !conn || conn->state != NET_CONN_CONNECTED ) return;
    if ( ( conn->client_salt ^ conn->server_salt ) != salt_xor ) return;

    net_event_push( p, NET_EVENT_DISCONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
    net_conn_free( p, conn );
}

/* Per-connection housekeeping, called once per update after the socket drain. */
static void
net_conn_tick( net_peer_t* p, net_conn_slot_t* conn )
{
    switch ( conn->state )
    {
        case NET_CONN_CONNECTING:
        case NET_CONN_RESPONDING:
        {
            /* Resolve the "connected before the clock was valid" sentinel (see peer_connect). */
            if ( conn->connect_start_time < 0 )
            {
                conn->connect_start_time       = p->now;
                conn->last_handshake_send_time = p->now;
            }

            if ( p->now - conn->connect_start_time > p->cfg.connect_timeout_seconds )
            {
                net_event_push( p, NET_EVENT_DISCONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
                net_conn_free( p, conn );
                return;
            }
            if ( p->now - conn->last_handshake_send_time >= p->cfg.connect_resend_seconds )
            {
                if ( conn->state == NET_CONN_CONNECTING )
                    net_connect_request_send( p, conn );
                else
                    net_challenge_reply_send( p, conn );
            }
            break;
        }

        case NET_CONN_CONNECTED:
        {
            if ( p->now - conn->last_recv_time > p->cfg.timeout_seconds )
            {
                net_event_push( p, NET_EVENT_DISCONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
                net_conn_free( p, conn );
                return;
            }

            net_channels_flush( p, conn );

            if ( p->now - conn->last_send_time >= p->cfg.keepalive_seconds )
                net_keepalive_send( p, conn );
            break;
        }

        default: break;
    }
}

/*============================================================================================*/
