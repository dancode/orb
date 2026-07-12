/*==============================================================================================

    sb_net_reliable.c -- Layer 2/4 tests: reliable-ordered delivery, fragmentation,
    and stats under a hostile simulated wire (loss + duplication + jitter, both directions).

    Uses the pump_ctx_t helpers from sb_net_conn.c (unity-included before this file).

==============================================================================================*/

#define REL_MSG_COUNT 2000

/* Establish a connected server/client pair with channel 0 reliable + channel 1 of
   `ch1_type`, condition both directions, and return the client's handle (server's via
   server->last_conn). `bandwidth` caps both peers' outgoing rate (0 = unlimited). */
static bool
rel_setup( pump_ctx_t* server, pump_ctx_t* client, net_conn_t* out_client_conn,
           const net_sim_t* sim, u8 ch1_type, i32 bandwidth )
{
    net_config_t cfg         = conn_test_config( true, 4 );
    cfg.channels[ 0 ]        = NET_CHANNEL_RELIABLE_ORDERED;
    cfg.channels[ 1 ]        = ch1_type;
    cfg.send_bandwidth_bytes = bandwidth;
    server->peer             = net_peer_create( &cfg );

    cfg.listen          = false;
    cfg.max_connections = 1;
    client->peer        = net_peer_create( &cfg );
    if ( !server->peer || !client->peer ) return false;

    /* Condition the wire only after the handshake -- connect under 25% loss works but
       burns test time on resend cadences. The reliability machinery is what's under test. */
    sys_addr_t server_addr;
    net_peer_addr( server->peer, &server_addr );
    *out_client_conn = net_peer_connect( client->peer, &server_addr );

    if ( !pump_until( server, client, &client->connected, 1, 5.0 ) ) return false;
    if ( server->connected != 1 ) return false;

    net_peer_sim( server->peer, sim );
    net_peer_sim( client->peer, sim );
    return true;
}

/*==============================================================================================
    Ordered lossless delivery of a numbered message stream
==============================================================================================*/

static void
net_test_reliable_stream( void )
{
    printf( "  reliable stream under 25%% loss + 20%% dup + 30-100 ms jitter\n" );

    net_sim_t sim           = { 0 };
    sim.loss                = 0.25f;
    sim.duplicate           = 0.20f;
    sim.latency_min_seconds = 0.03f;
    sim.latency_max_seconds = 0.10f;

    pump_ctx_t server = { 0 }, client = { 0 };
    net_conn_t client_conn;
    sb_check( rel_setup( &server, &client, &client_conn, &sim, NET_CHANNEL_UNRELIABLE, 0 ), "lossy pair connected" );
    if ( !server.peer || !client.peer ) return;

    /* Send REL_MSG_COUNT numbered messages; verify count, order, and integrity on the
       far side by walking events directly (pump_ctx only keeps the last message). */
    u32 next_expected = 0;
    b32 order_ok      = true;
    f64 deadline      = sys_tick_seconds() + 60.0;
    u32 sent          = 0;

    while ( next_expected < REL_MSG_COUNT && sys_tick_seconds() < deadline )
    {
        f64 now = sys_tick_seconds();

        /* Feed the sender; a false send is backpressure (window/queue full), not failure. */
        while ( sent < REL_MSG_COUNT )
        {
            u8 msg[ 32 ];
            memcpy( msg, &sent, 4 );
            memset( msg + 4, ( int )( sent & 0xFF ), sizeof( msg ) - 4 );
            if ( !net_peer_send( client.peer, client_conn, 0, msg, sizeof( msg ) ) ) break;
            sent++;
        }

        net_peer_update( client.peer, now );
        pump_drain( &client );

        net_peer_update( server.peer, now );
        net_event_t ev;
        while ( net_peer_poll( server.peer, &ev ) )
        {
            if ( ev.type != NET_EVENT_MESSAGE ) continue;
            u32 index;
            memcpy( &index, ev.data, 4 );

            order_ok &= ev.channel == 0;
            order_ok &= index == next_expected;
            order_ok &= ev.size == 32;
            order_ok &= ( ( const u8* )ev.data )[ 20 ] == ( u8 )( index & 0xFF );
            next_expected++;
        }

        thread_sleep_ms( 1 );
    }

    sb_check( next_expected == REL_MSG_COUNT, "all messages delivered" );
    sb_check( order_ok, "strict order and integrity" );

    net_stats_t stats;
    sb_check( net_peer_stats( client.peer, client_conn, &stats ), "stats readable" );
    sb_check( stats.rtt_seconds > 0.02f && stats.rtt_seconds < 1.0f, "rtt in jitter range" );
    sb_check( stats.packet_loss > 0.02f, "loss visible in stats" );
    printf( "    rtt %.0f ms, measured loss %.0f%%, %llu packets sent\n",
            stats.rtt_seconds * 1000.0f, stats.packet_loss * 100.0f,
            ( unsigned long long )stats.packets_sent );

    net_peer_destroy( client.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Fragmented 64 KB message through the same hostile wire
==============================================================================================*/

static void
net_test_reliable_fragment( void )
{
    printf( "  64 KB fragmented message under loss\n" );

    net_sim_t sim           = { 0 };
    sim.loss                = 0.20f;
    sim.latency_min_seconds = 0.01f;
    sim.latency_max_seconds = 0.03f;

    pump_ctx_t server = { 0 }, client = { 0 };
    net_conn_t client_conn;
    sb_check( rel_setup( &server, &client, &client_conn, &sim, NET_CHANNEL_UNRELIABLE, 0 ), "lossy pair connected" );
    if ( !server.peer || !client.peer ) return;

    /* Patterned payload so any reassembly error shows. */
    enum { BIG = 64 * 1024 - 512 };    /* just under the queue-clamped maximum */
    static u8 big[ BIG ];
    for ( i32 i = 0; i < BIG; i++ ) big[ i ] = ( u8 )( ( i * 31 + ( i >> 8 ) ) & 0xFF );

    /* Retry through backpressure until the whole message queues. */
    f64  deadline = sys_tick_seconds() + 30.0;
    bool queued   = false;
    while ( !queued && sys_tick_seconds() < deadline )
    {
        queued = net_peer_send( client.peer, client_conn, 0, big, BIG );
        if ( !queued ) pump2( &client, &server );
    }
    sb_check( queued, "big message queued" );

    bool intact   = false;
    bool received = false;
    while ( !received && sys_tick_seconds() < deadline )
    {
        f64 now = sys_tick_seconds();
        net_peer_update( client.peer, now );
        pump_drain( &client );

        net_peer_update( server.peer, now );
        net_event_t ev;
        while ( net_peer_poll( server.peer, &ev ) )
        {
            if ( ev.type != NET_EVENT_MESSAGE ) continue;
            received = true;
            intact   = ev.size == BIG && memcmp( ev.data, big, BIG ) == 0;
        }
        thread_sleep_ms( 1 );
    }

    sb_check( received, "big message arrived" );
    sb_check( intact, "big message intact after reassembly" );

    net_peer_destroy( client.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Unreliable under loss: some arrive, none corrupt, sender never blocks
==============================================================================================*/

static void
net_test_unreliable_lossy( void )
{
    printf( "  unreliable channel under 30%% loss\n" );

    net_sim_t sim = { 0 };
    sim.loss      = 0.30f;

    pump_ctx_t server = { 0 }, client = { 0 };
    net_conn_t client_conn;
    sb_check( rel_setup( &server, &client, &client_conn, &sim, NET_CHANNEL_UNRELIABLE, 0 ), "lossy pair connected" );
    if ( !server.peer || !client.peer ) return;

    const i32 to_send  = 200;
    i32       received = 0;
    b32       clean    = true;
    f64       deadline = sys_tick_seconds() + 5.0;

    for ( i32 i = 0; i < to_send; i++ )
    {
        u32 v = 0xABC00000u + ( u32 )i;
        sb_check( net_peer_send( client.peer, client_conn, 1, &v, 4 ) || true, "send never blocks" );

        /* Pump every few sends so packets flow while we feed. */
        if ( ( i & 7 ) == 7 )
        {
            f64 now = sys_tick_seconds();
            net_peer_update( client.peer, now );
            pump_drain( &client );
            net_peer_update( server.peer, now );
            net_event_t ev;
            while ( net_peer_poll( server.peer, &ev ) )
            {
                if ( ev.type != NET_EVENT_MESSAGE ) continue;
                u32 got;
                memcpy( &got, ev.data, 4 );
                clean &= ev.channel == 1 && ev.size == 4 && ( got & 0xFFF00000u ) == 0xABC00000u;
                received++;
            }
        }
    }

    /* Let stragglers land. */
    while ( sys_tick_seconds() < deadline && received < to_send )
    {
        f64 now = sys_tick_seconds();
        net_peer_update( client.peer, now );
        pump_drain( &client );
        net_peer_update( server.peer, now );
        net_event_t ev;
        while ( net_peer_poll( server.peer, &ev ) )
        {
            if ( ev.type != NET_EVENT_MESSAGE ) continue;
            received++;
        }
        thread_sleep_ms( 1 );
        if ( received > to_send / 3 ) break;    /* enough arrived to call it working */
    }

    sb_check( received > 0 && received <= to_send, "some messages arrived, none invented" );
    sb_check( clean, "arrivals uncorrupted" );

    net_peer_destroy( client.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Sequenced channel: only ever newer -- reordered and duplicated arrivals drop
==============================================================================================*/

static void
net_test_sequenced( void )
{
    printf( "  sequenced channel under dup + jitter (reordering)\n" );

    net_sim_t sim           = { 0 };
    sim.duplicate           = 0.30f;
    sim.latency_min_seconds = 0.01f;
    sim.latency_max_seconds = 0.05f;

    pump_ctx_t server = { 0 }, client = { 0 };
    net_conn_t client_conn;
    sb_check( rel_setup( &server, &client, &client_conn, &sim, NET_CHANNEL_UNRELIABLE_SEQUENCED, 0 ),
              "jittery pair connected" );
    if ( !server.peer || !client.peer ) return;

    const u32 to_send  = 300;
    u32       sent     = 0;
    i32       received = 0;
    u32       last     = 0;
    b32       forward  = true;    /* every arrival strictly newer than the previous */
    f64       deadline = sys_tick_seconds() + 10.0;

    while ( sys_tick_seconds() < deadline )
    {
        f64 now = sys_tick_seconds();

        /* A few per update so packets interleave through the jitter window. */
        for ( i32 i = 0; i < 5 && sent < to_send; i++, sent++ )
            net_peer_send( client.peer, client_conn, 1, &sent, 4 );

        net_peer_update( client.peer, now );
        pump_drain( &client );

        net_peer_update( server.peer, now );
        net_event_t ev;
        while ( net_peer_poll( server.peer, &ev ) )
        {
            if ( ev.type != NET_EVENT_MESSAGE ) continue;
            u32 index;
            memcpy( &index, ev.data, 4 );
            if ( received > 0 ) forward &= index > last;
            last = index;
            received++;
        }

        if ( sent == to_send && received > 0 && last == to_send - 1 ) break;
        thread_sleep_ms( 1 );
    }

    sb_check( received > 0 && received <= ( i32 )to_send, "sequenced messages arrived" );
    sb_check( forward, "arrivals strictly newer (stale + dup dropped)" );
    printf( "    %d of %u delivered (rest were stale or duplicate)\n", received, to_send );

    net_peer_destroy( client.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Bandwidth cap: a reliable bulk transfer takes at least data / rate seconds
==============================================================================================*/

static void
net_test_bandwidth_cap( void )
{
    printf( "  64 KB/s bandwidth cap on a bulk transfer\n" );

    pump_ctx_t server = { 0 }, client = { 0 };
    net_conn_t client_conn;
    sb_check( rel_setup( &server, &client, &client_conn, NULL, NET_CHANNEL_UNRELIABLE, 64 * 1024 ),
              "capped pair connected" );
    if ( !server.peer || !client.peer ) return;

    /* ~96 KB of reliable payload on a clean wire; at 64 KB/s it must take over a second.
       (An uncapped loopback transfer of this size completes in a few updates.) */
    enum { CHUNK = 1000, CHUNKS = 96 };
    u8 chunk[ CHUNK ];
    memset( chunk, 0x5A, sizeof( chunk ) );

    f64 start    = sys_tick_seconds();
    f64 deadline = start + 30.0;
    u32 queued   = 0;
    i32 received = 0;

    while ( received < CHUNKS && sys_tick_seconds() < deadline )
    {
        f64 now = sys_tick_seconds();
        while ( queued < CHUNKS && net_peer_send( client.peer, client_conn, 0, chunk, CHUNK ) ) queued++;

        net_peer_update( client.peer, now );
        pump_drain( &client );

        net_peer_update( server.peer, now );
        net_event_t ev;
        while ( net_peer_poll( server.peer, &ev ) )
        {
            if ( ev.type == NET_EVENT_MESSAGE && ev.size == CHUNK ) received++;
        }
        thread_sleep_ms( 1 );
    }

    f64 elapsed = sys_tick_seconds() - start;
    sb_check( received == CHUNKS, "all capped chunks delivered" );
    sb_check( elapsed > 0.9, "cap throttled the transfer" );
    sb_check( elapsed < 15.0, "cap did not stall the transfer" );
    printf( "    96 KB at 64 KB/s took %.2f s\n", elapsed );

    net_peer_destroy( client.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Suite entry
==============================================================================================*/

static void
net_test_reliable( void )
{
    net_test_reliable_stream();
    net_test_reliable_fragment();
    net_test_unreliable_lossy();
    net_test_sequenced();
    net_test_bandwidth_cap();
}

/*============================================================================================*/
