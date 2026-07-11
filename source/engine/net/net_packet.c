/*==============================================================================================

    net_packet.c -- wire packet primitives: crc32 validation, sequence math, salt
    generation, and the single seam every outgoing packet passes through.

==============================================================================================*/

#include "engine/net/net_internal.h"

/*==============================================================================================
    CRC32 (polynomial 0xEDB88320), table built on first use
==============================================================================================*/

static u32  s_crc_table[ 256 ];
static bool s_crc_table_ready = false;

static void
net_crc32_build_table( void )
{
    for ( u32 i = 0; i < 256; i++ )
    {
        u32 c = i;
        for ( i32 k = 0; k < 8; k++ ) c = ( c & 1 ) ? ( 0xEDB88320u ^ ( c >> 1 ) ) : ( c >> 1 );
        s_crc_table[ i ] = c;
    }
    s_crc_table_ready = true;
}

static u32
net_crc32( u32 seed, const void* data, i32 size )
{
    if ( !s_crc_table_ready ) net_crc32_build_table();

    const u8* bytes = ( const u8* )data;
    u32       c     = seed ^ 0xFFFFFFFFu;
    for ( i32 i = 0; i < size; i++ ) c = s_crc_table[ ( c ^ bytes[ i ] ) & 0xFF ] ^ ( c >> 8 );
    return c ^ 0xFFFFFFFFu;
}

/* crc over the datagram with its crc field zeroed, seeded by the protocol id. */
static u32
net_packet_crc( net_peer_t* p, const u8* packet, i32 size )
{
    u32 zero = 0;
    u32 c    = net_crc32( p->cfg.protocol_id, &zero, 4 );
    return net_crc32( c, packet + 4, size - 4 );
}

/*==============================================================================================
    Sequence math -- 16-bit, wrap-aware (half-range rule)
==============================================================================================*/

static bool
net_seq_greater( u16 a, u16 b )
{
    u16 delta = ( u16 )( a - b );
    return delta != 0 && delta < 0x8000;
}

static bool
net_seq_less( u16 a, u16 b )
{
    return net_seq_greater( b, a );
}

/*==============================================================================================
    Salt generation -- xorshift64*, seeded per peer at create
==============================================================================================*/

static u64
net_random_u64( net_peer_t* p )
{
    u64 x = p->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    p->rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static f32
net_random_f32( net_peer_t* p )
{
    return ( f32 )( net_random_u64( p ) >> 40 ) * ( 1.0f / 16777216.0f );
}

/*==============================================================================================
    Packet build + send
==============================================================================================*/

/* Start a packet in p->pkt_out: crc placeholder + type byte. Callers append fields, then
   net_packet_end() stamps the crc and returns the wire size. */
static void
net_packet_begin( net_peer_t* p, bit_writer_t* w, u8 type )
{
    bit_writer_init( w, p->pkt_out, NET_PACKET_CAP );
    bit_write_u32( w, 0 );    /* crc, stamped in net_packet_end */
    bit_write_u8( w, type );
}

static i32
net_packet_end( net_peer_t* p, bit_writer_t* w )
{
    i32 size = bit_writer_flush( w );
    if ( size < 0 ) return -1;

    u32 crc = net_packet_crc( p, p->pkt_out, size );
    memcpy( p->pkt_out, &crc, 4 );
    return size;
}

/* The single seam every outgoing packet passes through: stats, then the condition
   simulator (loss / duplication / latency) when enabled. `conn` may be NULL for
   pre-connection traffic (challenges, denies). */
static void
net_packet_send( net_peer_t* p, net_conn_slot_t* conn, const sys_addr_t* to, i32 size )
{
    if ( size <= 0 ) return;

    if ( conn )
    {
        conn->last_send_time = p->now;
        conn->stats.packets_sent++;
        conn->stats.bytes_sent += ( u64 )size;
    }

    bool sim_active = p->sim_queue && ( p->sim.loss > 0 || p->sim.duplicate > 0 ||
                                        p->sim.latency_max_seconds > 0 );
    if ( !sim_active )
    {
        sys_socket_send( p->socket, to, p->pkt_out, size );
        return;
    }

    if ( net_random_f32( p ) < p->sim.loss ) return;    /* the wire ate it */

    i32 copies = ( net_random_f32( p ) < p->sim.duplicate ) ? 2 : 1;
    for ( i32 c = 0; c < copies; c++ )
    {
        f32 span  = p->sim.latency_max_seconds - p->sim.latency_min_seconds;
        f32 delay = p->sim.latency_min_seconds + ( span > 0 ? net_random_f32( p ) * span : 0 );
        if ( delay <= 0 )
        {
            sys_socket_send( p->socket, to, p->pkt_out, size );
            continue;
        }

        net_sim_pkt_t* slot = NULL;
        for ( i32 i = 0; i < NET_SIM_QUEUE; i++ )
        {
            if ( !p->sim_queue[ i ].used )
            {
                slot = &p->sim_queue[ i ];
                break;
            }
        }
        if ( !slot )
        {
            sys_socket_send( p->socket, to, p->pkt_out, size );    /* queue full: no delay */
            continue;
        }

        slot->used         = true;
        slot->size         = size;
        slot->deliver_time = p->now + delay;
        slot->to           = *to;
        memcpy( slot->data, p->pkt_out, ( usize )size );
    }
}

/* Release matured simulator packets. Called once per peer_update. */
static void
net_sim_drain( net_peer_t* p )
{
    if ( !p->sim_queue ) return;

    for ( i32 i = 0; i < NET_SIM_QUEUE; i++ )
    {
        net_sim_pkt_t* slot = &p->sim_queue[ i ];
        if ( slot->used && slot->deliver_time <= p->now )
        {
            sys_socket_send( p->socket, &slot->to, slot->data, slot->size );
            slot->used = false;
        }
    }
}

/* Zero-pad the current packet content out to the configured packet size so handshake
   packets cost the sender at least as much as any reply (no amplification). Call between
   building fields and net_packet_end. */
static void
net_packet_pad( net_peer_t* p, bit_writer_t* w )
{
    while ( bit_writer_ok( w ) && w->bits_written < p->cfg.max_packet_bytes * 8 ) bit_write_u8( w, 0 );
}

/*============================================================================================*/
