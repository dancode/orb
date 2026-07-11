/*==============================================================================================

    net_channel.c -- message channels: send-queue packing into payload packets, ack
    processing, reliable ordering with fragmentation, and the payload receive path that
    turns packets back into MESSAGE events.

    Reliable model (yojimbo-style): packets are never retransmitted. Each reliable message
    keeps the seq of the packet that last carried it; when any packet is acked, the
    messages it carried are done. Un-acked messages are re-packed into fresh packets after
    ~1.25 * RTT. The receiver delivers strictly in message-id order, buffering out-of-order
    arrivals, and reassembles fragmented messages by plain concatenation (in-order
    delivery guarantees fragment order).

==============================================================================================*/

#include "engine/net/net_internal.h"

/*==============================================================================================
    Payload header
==============================================================================================*/

/* Returns the packet sequence so reliable records can be tied to this packet's ack. */
static u16
net_payload_header_write( net_peer_t* p, net_conn_slot_t* conn, bit_writer_t* w )
{
    u16 seq = conn->seq_local++;

    net_packet_begin( p, w, NET_PKT_PAYLOAD );
    bit_write_u64( w, conn->client_salt ^ conn->server_salt );
    bit_write_u16( w, seq );
    bit_write_u16( w, conn->seq_remote );
    bit_write_u32( w, conn->recv_bits );

    net_sent_entry_t* e = &conn->sent[ seq % NET_SENT_WINDOW ];
    e->used             = true;
    e->acked            = false;
    e->seq              = seq;
    e->send_time        = p->now;
    return seq;
}

static void
net_payload_packet_send( net_peer_t* p, net_conn_slot_t* conn, bit_writer_t* w )
{
    bit_write_u8( w, NET_MSG_END );
    i32 size = net_packet_end( p, w );
    net_packet_send( p, conn, &conn->addr, size );
    conn->bw_tokens -= size;    /* may dip negative (keepalives always send); refill recovers */
}

/* An empty payload doubles as the keepalive and still carries fresh ack state. */
static void
net_keepalive_send( net_peer_t* p, net_conn_slot_t* conn )
{
    bit_writer_t w;
    net_payload_header_write( p, conn, &w );
    net_payload_packet_send( p, conn, &w );
}

/*==============================================================================================
    Message size budgets
==============================================================================================*/

/* Largest unreliable message that fits a payload packet: header + end marker + tag +
   2-byte varint length. */
static i32
net_msg_max_bytes( net_peer_t* p )
{
    return p->cfg.max_packet_bytes - NET_PAYLOAD_HEADER_BYTES - 1 - 3;
}

/* Reliable records also carry a message id (varint, up to 3 bytes). */
static i32
net_rel_msg_max_bytes( net_peer_t* p )
{
    return net_msg_max_bytes( p ) - 3;
}

/* Exact wire cost of one record. Reliable and sequenced ids are u16 -> varint 1..3 bytes. */
static i32
net_record_wire_bytes( bool has_id, u16 id, i32 size )
{
    i32 bytes = 1 + ( ( size < 128 ) ? 1 : 2 ) + size;
    if ( has_id ) bytes += ( id < 128 ) ? 1 : ( ( id < 16384 ) ? 2 : 3 );
    return bytes;
}

/*==============================================================================================
    Reliable send queue
==============================================================================================*/

/* Reclaim the dead prefix so the tail has room. Slot offsets shift down together. */
static void
net_rel_compact( net_chan_t* chan )
{
    if ( chan->send_head == 0 ) return;

    memmove( chan->send_data, chan->send_data + chan->send_head,
             ( usize )( chan->send_used - chan->send_head ) );
    for ( u16 id = chan->oldest_unacked; id != chan->send_id; id++ )
    {
        net_rel_out_t* m = &chan->out[ id % NET_REL_WINDOW ];
        if ( m->used ) m->offset -= chan->send_head;
    }
    chan->send_used -= chan->send_head;
    chan->send_head = 0;
}

/* Queue one reliable record (whole message or one fragment). False = backpressure. */
static bool
net_rel_enqueue_record( net_chan_t* chan, u8 tag, const void* data, i32 size )
{
    if ( ( u16 )( chan->send_id - chan->oldest_unacked ) >= NET_REL_WINDOW ) return false;
    if ( chan->send_used + size > chan->send_cap )
    {
        net_rel_compact( chan );
        if ( chan->send_used + size > chan->send_cap ) return false;
    }

    net_rel_out_t* m = &chan->out[ chan->send_id % NET_REL_WINDOW ];
    m->used          = true;
    m->acked         = false;
    m->sent_once     = false;
    m->tag           = tag;
    m->id            = chan->send_id++;
    m->last_sent_seq = 0;
    m->last_send_time = 0;
    m->offset        = chan->send_used;
    m->size          = size;

    memcpy( chan->send_data + chan->send_used, data, ( usize )size );
    chan->send_used += size;
    return true;
}

/* Queue a reliable message, fragmenting when it exceeds the per-packet budget.
   All-or-nothing: on insufficient window/buffer space nothing is queued. */
static bool
net_rel_enqueue( net_peer_t* p, net_conn_slot_t* conn, i32 channel, const void* data, i32 size )
{
    net_chan_t* chan     = &conn->channels[ channel ];
    i32         frag_max = net_rel_msg_max_bytes( p );
    u8          tag      = ( u8 )channel;

    if ( size <= frag_max ) return net_rel_enqueue_record( chan, tag, data, size );
    if ( size > p->cfg.max_message_bytes ) return false;

    i32 frag_count = ( size + frag_max - 1 ) / frag_max;
    i32 free_slots = NET_REL_WINDOW - ( u16 )( chan->send_id - chan->oldest_unacked );
    net_rel_compact( chan );
    if ( frag_count > free_slots ) return false;
    if ( chan->send_used + size > chan->send_cap ) return false;

    const u8* bytes = ( const u8* )data;
    for ( i32 i = 0; i < frag_count; i++ )
    {
        i32 chunk    = ( i == frag_count - 1 ) ? size - i * frag_max : frag_max;
        u8  frag_tag = tag | NET_TAG_FRAGMENT | ( ( i == frag_count - 1 ) ? NET_TAG_LAST : 0 );
        net_rel_enqueue_record( chan, frag_tag, bytes + i * frag_max, chunk );    /* space verified */
    }
    return true;
}

/*==============================================================================================
    Ack processing
==============================================================================================*/

/* Release acked head messages and reclaim their bytes. */
static void
net_rel_advance( net_chan_t* chan )
{
    while ( chan->oldest_unacked != chan->send_id )
    {
        net_rel_out_t* m = &chan->out[ chan->oldest_unacked % NET_REL_WINDOW ];
        if ( !m->used || !m->acked ) break;

        chan->send_head = m->offset + m->size;
        m->used         = false;
        chan->oldest_unacked++;
    }

    if ( chan->oldest_unacked == chan->send_id )
    {
        chan->send_head = 0;
        chan->send_used = 0;
    }
}

/* One packet seq confirmed received by the remote end. */
static void
net_ack_apply( net_peer_t* p, net_conn_slot_t* conn, u16 seq, bool rtt_sample )
{
    net_sent_entry_t* e = &conn->sent[ seq % NET_SENT_WINDOW ];
    if ( !e->used || e->seq != seq || e->acked ) return;
    e->acked = true;

    /* Only the header's direct ack field gives a tight RTT sample; history bits may
       have been sitting acked-but-unreported for a while. */
    if ( rtt_sample )
    {
        f32 sample = ( f32 )( p->now - e->send_time );
        conn->rtt  = ( conn->rtt <= 0 ) ? sample : conn->rtt + 0.1f * ( sample - conn->rtt );
    }

    for ( i32 ch = 0; ch < p->cfg.channel_count; ch++ )
    {
        if ( p->cfg.channels[ ch ] != NET_CHANNEL_RELIABLE_ORDERED ) continue;

        net_chan_t* chan = &conn->channels[ ch ];
        for ( u16 id = chan->oldest_unacked; id != chan->send_id; id++ )
        {
            net_rel_out_t* m = &chan->out[ id % NET_REL_WINDOW ];
            if ( m->used && !m->acked && m->sent_once && m->last_sent_seq == seq ) m->acked = true;
        }
        net_rel_advance( chan );
    }
}

static void
net_acks_process( net_peer_t* p, net_conn_slot_t* conn, u16 ack, u32 ack_bits )
{
    net_ack_apply( p, conn, ack, true );
    for ( i32 i = 0; i < 32; i++ )
    {
        if ( ack_bits & ( 1u << i ) ) net_ack_apply( p, conn, ( u16 )( ack - 1 - i ), false );
    }
}

/*==============================================================================================
    Send-queue packing
==============================================================================================*/

/* Builder state threaded through the flush: one packet open at a time, hard-capped
   packets per connection per update. */
typedef struct net_pack_ctx_s
{
    bit_writer_t w;
    bool         open;
    u16          pkt_seq;
    i32          packets_sent;
    i32          budget_bits;    /* bits available for records in one packet */

} net_pack_ctx_t;

/* Ensure an open packet with room for `record_bytes`. False when the packet cap or the
   bandwidth budget is exhausted for this update. */
static bool
net_pack_room( net_peer_t* p, net_conn_slot_t* conn, net_pack_ctx_t* ctx, i32 record_bytes )
{
    if ( ctx->open && ctx->w.bits_written + record_bytes * 8 > ctx->budget_bits )
    {
        net_payload_packet_send( p, conn, &ctx->w );
        ctx->packets_sent++;
        ctx->open = false;
    }

    if ( !ctx->open )
    {
        if ( ctx->packets_sent >= NET_PACKETS_PER_UPDATE ) return false;
        if ( p->cfg.send_bandwidth_bytes > 0 && conn->bw_tokens < ( f64 )p->cfg.max_packet_bytes )
            return false;
        ctx->pkt_seq = net_payload_header_write( p, conn, &ctx->w );
        ctx->open    = true;
    }
    return true;
}

/* Token-bucket refill: bandwidth accrues between updates, with a 100 ms burst ceiling. */
static void
net_bw_refill( net_peer_t* p, net_conn_slot_t* conn )
{
    if ( p->cfg.send_bandwidth_bytes <= 0 ) return;

    f64 burst = ( f64 )p->cfg.send_bandwidth_bytes * 0.1 + ( f64 )p->cfg.max_packet_bytes;
    if ( conn->bw_last_time <= 0 )
    {
        conn->bw_last_time = p->now;
        conn->bw_tokens    = burst;
        return;
    }

    conn->bw_tokens += ( f64 )p->cfg.send_bandwidth_bytes * ( p->now - conn->bw_last_time );
    if ( conn->bw_tokens > burst ) conn->bw_tokens = burst;
    conn->bw_last_time = p->now;
}

/* Resend cadence: a fraction over the measured RTT, clamped to something sane before
   the first sample and against pathological RTTs. */
static f64
net_rel_resend_interval( net_conn_slot_t* conn )
{
    f64 interval = ( f64 )conn->rtt * 1.25;
    if ( interval < 0.1 ) interval = 0.1;
    if ( interval > 1.0 ) interval = 1.0;
    return interval;
}

static void
net_channels_flush( net_peer_t* p, net_conn_slot_t* conn )
{
    net_pack_ctx_t ctx = { 0 };
    ctx.budget_bits    = ( p->cfg.max_packet_bytes - 1 /* end marker */ ) * 8;

    net_bw_refill( p, conn );

    for ( i32 ch = 0; ch < p->cfg.channel_count; ch++ )
    {
        net_chan_t* chan      = &conn->channels[ ch ];
        bool        reliable  = p->cfg.channels[ ch ] == NET_CHANNEL_RELIABLE_ORDERED;
        bool        sequenced = p->cfg.channels[ ch ] == NET_CHANNEL_UNRELIABLE_SEQUENCED;

        if ( reliable )
        {
            f64 resend = net_rel_resend_interval( conn );

            for ( u16 id = chan->oldest_unacked; id != chan->send_id; id++ )
            {
                net_rel_out_t* m = &chan->out[ id % NET_REL_WINDOW ];
                if ( !m->used || m->acked ) continue;
                if ( m->sent_once && p->now - m->last_send_time < resend ) continue;

                if ( !net_pack_room( p, conn, &ctx, net_record_wire_bytes( true, m->id, m->size ) ) )
                    break;    /* packet cap: reliable messages simply wait for the next update */

                bit_write_u8( &ctx.w, m->tag );
                bit_write_varint_u32( &ctx.w, m->id );
                bit_write_varint_u32( &ctx.w, ( u32 )m->size );
                bit_write_bytes( &ctx.w, chan->send_data + m->offset, m->size );

                m->sent_once     = true;
                m->last_sent_seq = ctx.pkt_seq;
                m->last_send_time = p->now;
                conn->stats.messages_sent++;
            }
        }
        else
        {
            /* Unreliable and sequenced: drain fully; sequenced records carry an id so the
               receiver can drop stale arrivals. */
            i32 cursor = 0;
            while ( cursor < chan->send_used )
            {
                u16 size;
                memcpy( &size, chan->send_data + cursor, 2 );
                cursor += 2;

                u16 id = chan->send_id;
                if ( !net_pack_room( p, conn, &ctx, net_record_wire_bytes( sequenced, id, size ) ) )
                {
                    /* Budget exhausted: unreliable semantics allow dropping the backlog. */
                    conn->stats.messages_dropped++;
                    cursor += size;
                    continue;
                }

                bit_write_u8( &ctx.w, ( u8 )ch );
                if ( sequenced )
                {
                    bit_write_varint_u32( &ctx.w, id );
                    chan->send_id++;
                }
                bit_write_varint_u32( &ctx.w, ( u32 )size );
                bit_write_bytes( &ctx.w, chan->send_data + cursor, size );
                cursor += size;
                conn->stats.messages_sent++;
            }
            chan->send_used = 0;
        }
    }

    if ( ctx.open ) net_payload_packet_send( p, conn, &ctx.w );
}

/*==============================================================================================
    Receive path
==============================================================================================*/

/* Record `seq` in the remote-arrival window that we echo back as ack state. */
static void
net_recv_window_insert( net_conn_slot_t* conn, u16 seq )
{
    if ( net_seq_greater( seq, conn->seq_remote ) )
    {
        u16 shift = ( u16 )( seq - conn->seq_remote );
        conn->recv_bits = ( shift >= 32 ) ? 0 : ( conn->recv_bits << shift ) | ( 1u << ( shift - 1 ) );
        conn->seq_remote = seq;
    }
    else
    {
        u16 back = ( u16 )( conn->seq_remote - seq );
        if ( back >= 1 && back <= 32 ) conn->recv_bits |= 1u << ( back - 1 );
    }
}

/* Hand one in-order reliable record to the application: whole messages copy into the
   per-update arena; fragments accumulate in the reassembly buffer until LAST. */
static void
net_rel_deliver( net_peer_t* p, net_conn_slot_t* conn, u8 channel, net_chan_t* chan,
                 u8 tag, const u8* data, i32 size )
{
    if ( !( tag & NET_TAG_FRAGMENT ) )
    {
        u8* dst = net_recv_arena_alloc( p, size );
        if ( !dst )
        {
            conn->stats.messages_dropped++;
            return;
        }
        memcpy( dst, data, ( usize )size );
        conn->stats.messages_received++;
        net_event_push( p, NET_EVENT_MESSAGE, net_conn_handle( p, conn ), channel, dst, size );
        return;
    }

    /* Oversize stream (config mismatch): discard fragments until it ends. */
    if ( chan->frag_skip || chan->frag_used + size > chan->frag_cap )
    {
        chan->frag_skip = !( tag & NET_TAG_LAST );
        chan->frag_used = 0;
        if ( tag & NET_TAG_LAST ) conn->stats.messages_dropped++;
        return;
    }

    memcpy( chan->frag_data + chan->frag_used, data, ( usize )size );
    chan->frag_used += size;

    if ( tag & NET_TAG_LAST )
    {
        u8* dst = net_recv_arena_alloc( p, chan->frag_used );
        if ( dst )
        {
            memcpy( dst, chan->frag_data, ( usize )chan->frag_used );
            conn->stats.messages_received++;
            net_event_push( p, NET_EVENT_MESSAGE, net_conn_handle( p, conn ), channel, dst,
                            chan->frag_used );
        }
        else
        {
            conn->stats.messages_dropped++;
        }
        chan->frag_used = 0;
    }
}

/* Deliver everything now contiguous from `expected_id` onward. */
static void
net_rel_drain_buffered( net_peer_t* p, net_conn_slot_t* conn, u8 channel, net_chan_t* chan )
{
    for ( ;; )
    {
        net_rel_in_t* slot = &chan->in[ chan->expected_id % NET_REL_WINDOW ];
        if ( !slot->used ) break;

        net_rel_deliver( p, conn, channel, chan, slot->tag, chan->recv_data + slot->offset, slot->size );
        slot->used = false;
        chan->in_count--;
        chan->expected_id++;
    }

    if ( chan->in_count == 0 ) chan->recv_used = 0;    /* gap healed: whole store resets */
}

/* One reliable record from the wire: deliver, buffer, or drop as a duplicate. */
static void
net_rel_receive( net_peer_t* p, net_conn_slot_t* conn, u8 channel, u8 tag, u16 id,
                 const u8* data, i32 size )
{
    net_chan_t* chan = &conn->channels[ channel ];

    if ( id == chan->expected_id )
    {
        net_rel_deliver( p, conn, channel, chan, tag, data, size );
        chan->expected_id++;
        net_rel_drain_buffered( p, conn, channel, chan );
        return;
    }

    if ( net_seq_less( id, chan->expected_id ) ) return;                    /* duplicate */
    if ( ( u16 )( id - chan->expected_id ) >= NET_REL_WINDOW ) return;      /* absurdly ahead */

    net_rel_in_t* slot = &chan->in[ id % NET_REL_WINDOW ];
    if ( slot->used ) return;                                               /* duplicate */
    if ( chan->recv_used + size > chan->recv_cap ) return;    /* full: the resend covers us */

    slot->used   = true;
    slot->tag    = tag;
    slot->offset = chan->recv_used;
    slot->size   = size;
    memcpy( chan->recv_data + chan->recv_used, data, ( usize )size );
    chan->recv_used += size;
    chan->in_count++;
}

static void
net_payload_receive( net_peer_t* p, bit_reader_t* r, const sys_addr_t* from, i32 packet_size )
{
    net_conn_slot_t* conn = net_conn_by_addr( p, from );
    if ( !conn ) return;

    u64 salt_xor = bit_read_u64( r );
    u16 seq      = bit_read_u16( r );
    u16 ack      = bit_read_u16( r );
    u32 ack_bits = bit_read_u32( r );
    if ( !bit_reader_ok( r ) ) return;
    if ( salt_xor != ( conn->client_salt ^ conn->server_salt ) ) return;

    /* A payload from a peer we are still waiting on an ACCEPT from means the ACCEPT was
       lost -- the payload itself proves the handshake completed. */
    if ( conn->state == NET_CONN_RESPONDING )
    {
        conn->state = NET_CONN_CONNECTED;
        net_event_push( p, NET_EVENT_CONNECTED, net_conn_handle( p, conn ), 0, NULL, 0 );
    }
    if ( conn->state != NET_CONN_CONNECTED ) return;

    net_conn_touch_recv( p, conn );
    net_recv_window_insert( conn, seq );
    net_acks_process( p, conn, ack, ack_bits );
    conn->stats.packets_received++;
    conn->stats.bytes_received += ( u64 )packet_size;

    /* Unpack message records. */
    for ( ;; )
    {
        u8 tag = bit_read_u8( r );
        if ( tag == NET_MSG_END || !bit_reader_ok( r ) ) break;

        u8 channel = tag & NET_TAG_CHANNEL;
        if ( channel >= p->cfg.channel_count ) break;
        bool reliable  = p->cfg.channels[ channel ] == NET_CHANNEL_RELIABLE_ORDERED;
        bool sequenced = p->cfg.channels[ channel ] == NET_CHANNEL_UNRELIABLE_SEQUENCED;

        u16 id = 0;
        if ( reliable || sequenced ) id = ( u16 )bit_read_varint_u32( r );

        i32 size = ( i32 )bit_read_varint_u32( r );
        if ( !bit_reader_ok( r ) ) break;
        if ( size <= 0 || size > net_msg_max_bytes( p ) ) break;

        u8 record[ NET_PACKET_CAP + 4 ];
        bit_read_bytes( r, record, size );
        if ( !bit_reader_ok( r ) ) break;

        if ( reliable )
        {
            net_rel_receive( p, conn, channel, tag, id, record, size );
            continue;
        }

        /* Sequenced: only ever move forward -- stale and duplicate ids drop here. */
        net_chan_t* chan = &conn->channels[ channel ];
        if ( sequenced && !net_seq_greater( id, chan->seq_in_latest ) ) continue;

        u8* dst = net_recv_arena_alloc( p, size );
        if ( !dst )
        {
            conn->stats.messages_dropped++;
            continue;
        }
        if ( sequenced ) chan->seq_in_latest = id;
        memcpy( dst, record, ( usize )size );
        conn->stats.messages_received++;
        net_event_push( p, NET_EVENT_MESSAGE, net_conn_handle( p, conn ), channel, dst, size );
    }
}

/*============================================================================================*/
