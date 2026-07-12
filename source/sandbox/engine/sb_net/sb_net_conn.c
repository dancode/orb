/*==============================================================================================

    sb_net_conn.c -- Layer 2/4 tests: handshake, unreliable message exchange,
    denial, disconnect, and connect timeout between two live peers.

    Peers bind [::1] -- this machine permanently blackholes ~20% of fresh IPv4-loopback
    reply flows (see sb_net_socket.c header); IPv6 loopback is clean.

==============================================================================================*/

#define CONN_PROTOCOL_ID 0x0EB00001u

/* One peer plus counters accumulated from its events across pump cycles. */
typedef struct pump_ctx_s
{
    net_peer_t* peer;
    i32         connected;
    i32         disconnected;
    i32         messages;
    net_conn_t  last_conn;       // from the most recent CONNECTED event
    u8          last_channel;    // from the most recent MESSAGE event
    char        last_msg[ 128 ];

} pump_ctx_t;

static void
pump_drain( pump_ctx_t* c )
{
    net_event_t ev;
    while ( net_peer_poll( c->peer, &ev ) )
    {
        switch ( ev.type )
        {
            case NET_EVENT_CONNECTED:
                c->connected++;
                c->last_conn = ev.conn;
                break;

            case NET_EVENT_DISCONNECTED: c->disconnected++; break;

            case NET_EVENT_MESSAGE:
            {
                c->messages++;
                c->last_channel = ev.channel;
                i32 n           = ev.size < ( i32 )sizeof( c->last_msg ) - 1 ? ev.size : ( i32 )sizeof( c->last_msg ) - 1;
                memcpy( c->last_msg, ev.data, ( usize )n );
                c->last_msg[ n ] = 0;
                break;
            }

            default: break;
        }
    }
}

/* Update + drain both peers once with real time. */
static void
pump2( pump_ctx_t* a, pump_ctx_t* b )
{
    f64 now = sys_tick_seconds();
    net_peer_update( a->peer, now );
    pump_drain( a );
    if ( b )
    {
        net_peer_update( b->peer, now );
        pump_drain( b );
    }
    thread_sleep_ms( 1 );
}

/* Pump until `*counter` reaches `goal` or `timeout` real seconds pass. */
static bool
pump_until( pump_ctx_t* a, pump_ctx_t* b, const i32* counter, i32 goal, f64 timeout )
{
    f64 deadline = sys_tick_seconds() + timeout;
    while ( sys_tick_seconds() < deadline )
    {
        pump2( a, b );
        if ( *counter >= goal ) return true;
    }
    return false;
}

static net_config_t
conn_test_config( bool listen, u16 max_connections )
{
    net_config_t cfg    = { 0 };
    cfg.protocol_id     = CONN_PROTOCOL_ID;
    cfg.listen          = listen;
    cfg.max_connections = max_connections;
    cfg.channel_count   = 2;
    cfg.channels[ 0 ]   = NET_CHANNEL_UNRELIABLE;
    cfg.channels[ 1 ]   = NET_CHANNEL_UNRELIABLE;
    sys_addr_parse( "[::1]:0", &cfg.bind_addr );
    return cfg;
}

/*==============================================================================================
    Connect, exchange messages both ways, explicit disconnect
==============================================================================================*/

static void
net_test_conn_exchange( void )
{
    printf( "  connect + exchange + disconnect\n" );

    net_config_t server_cfg = conn_test_config( true, 4 );
    net_config_t client_cfg = conn_test_config( false, 1 );

    pump_ctx_t server = { 0 };
    pump_ctx_t client = { 0 };
    server.peer       = net_peer_create( &server_cfg );
    client.peer       = net_peer_create( &client_cfg );
    sb_check( server.peer && client.peer, "peers created" );
    if ( !server.peer || !client.peer ) return;

    sys_addr_t server_addr;
    sb_check( net_peer_addr( server.peer, &server_addr ), "server bound addr" );

    net_conn_t client_conn = net_peer_connect( client.peer, &server_addr );
    sb_check( client_conn != NET_CONN_INVALID, "connect returns handle" );

    sb_check( pump_until( &server, &client, &client.connected, 1, 5.0 ), "client sees CONNECTED" );
    sb_check( server.connected == 1, "server sees CONNECTED" );

    /* Client -> server on channel 1. */
    const char* ping = "ping from client";
    sb_check( net_peer_send( client.peer, client_conn, 1, ping, ( i32 )strlen( ping ) + 1 ), "client send" );
    sb_check( pump_until( &server, &client, &server.messages, 1, 5.0 ), "server got message" );
    sb_check( server.last_channel == 1 && strcmp( server.last_msg, ping ) == 0, "ping intact on channel 1" );

    /* Server -> client, three messages on channel 0 in one update. */
    const char* pong = "pong from server";
    for ( i32 i = 0; i < 3; i++ )
        sb_check( net_peer_send( server.peer, server.last_conn, 0, pong, ( i32 )strlen( pong ) + 1 ),
                  "server send" );
    sb_check( pump_until( &server, &client, &client.messages, 3, 5.0 ), "client got all three" );
    sb_check( client.last_channel == 0 && strcmp( client.last_msg, pong ) == 0, "pong intact on channel 0" );

    /* Bad sends are rejected cleanly. */
    sb_check( !net_peer_send( client.peer, client_conn, 5, "x", 2 ), "bad channel rejected" );
    sb_check( !net_peer_send( client.peer, 0xDEAD0000u, 0, "x", 2 ), "stale handle rejected" );

    net_stats_t cs, ss;
    sb_check( net_peer_stats( client.peer, client_conn, &cs ), "client stats" );
    sb_check( net_peer_stats( server.peer, server.last_conn, &ss ), "server stats" );
    sb_check( cs.messages_sent >= 1 && cs.messages_received >= 3, "client stat counts" );
    sb_check( ss.messages_sent >= 3 && ss.messages_received >= 1, "server stat counts" );
    sb_check( cs.packets_received > 0 && ss.packets_received > 0, "packets counted" );

    /* Explicit disconnect: the handle dies locally, the remote learns via DISCONNECT packet. */
    net_peer_disconnect( client.peer, client_conn );
    sb_check( pump_until( &server, &client, &server.disconnected, 1, 5.0 ), "server sees DISCONNECTED" );
    sb_check( !net_peer_stats( client.peer, client_conn, &cs ), "client handle dead after disconnect" );

    net_peer_destroy( client.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Server full -> DENY
==============================================================================================*/

static void
net_test_conn_deny( void )
{
    printf( "  server-full denial\n" );

    net_config_t server_cfg = conn_test_config( true, 1 );    /* one slot only */
    net_config_t client_cfg = conn_test_config( false, 1 );

    pump_ctx_t server   = { 0 };
    pump_ctx_t client_a = { 0 };
    pump_ctx_t client_b = { 0 };
    server.peer         = net_peer_create( &server_cfg );
    client_a.peer       = net_peer_create( &client_cfg );
    client_b.peer       = net_peer_create( &client_cfg );
    sb_check( server.peer && client_a.peer && client_b.peer, "peers created" );

    sys_addr_t server_addr;
    net_peer_addr( server.peer, &server_addr );

    net_peer_connect( client_a.peer, &server_addr );
    sb_check( pump_until( &server, &client_a, &client_a.connected, 1, 5.0 ), "first client connected" );

    net_conn_t conn_b = net_peer_connect( client_b.peer, &server_addr );
    sb_check( conn_b != NET_CONN_INVALID, "second connect starts" );
    sb_check( pump_until( &server, &client_b, &client_b.disconnected, 1, 5.0 ), "second client denied" );
    sb_check( client_b.connected == 0, "second client never connected" );
    sb_check( server.connected == 1, "server still has one connection" );

    net_peer_destroy( client_a.peer );
    net_peer_destroy( client_b.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Connect timeout -- nobody answers, wrong protocol never answers
==============================================================================================*/

static void
net_test_conn_timeout( void )
{
    printf( "  connect timeouts\n" );

    /* Nobody home: a bound-but-never-updated socket eats the requests. */
    net_config_t dead_cfg = conn_test_config( false, 1 );
    net_peer_t*  dead     = net_peer_create( &dead_cfg );
    sys_addr_t   dead_addr;
    net_peer_addr( dead, &dead_addr );

    net_config_t client_cfg            = conn_test_config( false, 1 );
    client_cfg.connect_timeout_seconds = 0.6f;
    client_cfg.connect_resend_seconds  = 0.1f;

    pump_ctx_t client = { 0 };
    client.peer       = net_peer_create( &client_cfg );

    net_conn_t conn = net_peer_connect( client.peer, &dead_addr );
    sb_check( conn != NET_CONN_INVALID, "connect to silence starts" );
    sb_check( pump_until( &client, NULL, &client.disconnected, 1, 3.0 ), "silent connect times out" );
    sb_check( client.connected == 0, "never connected to silence" );
    net_peer_destroy( client.peer );
    net_peer_destroy( dead );

    /* Wrong protocol id: the server drops every packet at the crc check. */
    net_config_t server_cfg = conn_test_config( true, 4 );
    pump_ctx_t   server     = { 0 };
    server.peer             = net_peer_create( &server_cfg );
    sys_addr_t server_addr;
    net_peer_addr( server.peer, &server_addr );

    net_config_t rogue_cfg             = conn_test_config( false, 1 );
    rogue_cfg.protocol_id              = CONN_PROTOCOL_ID + 1;
    rogue_cfg.connect_timeout_seconds  = 0.6f;
    rogue_cfg.connect_resend_seconds   = 0.1f;

    pump_ctx_t rogue = { 0 };
    rogue.peer       = net_peer_create( &rogue_cfg );

    net_peer_connect( rogue.peer, &server_addr );
    sb_check( pump_until( &server, &rogue, &rogue.disconnected, 1, 3.0 ), "wrong protocol times out" );
    sb_check( server.connected == 0, "server ignored wrong protocol" );

    net_peer_destroy( rogue.peer );
    net_peer_destroy( server.peer );
}

/*==============================================================================================
    Suite entry
==============================================================================================*/

static void
net_test_conn( void )
{
    net_test_conn_exchange();
    net_test_conn_deny();
    net_test_conn_timeout();
}

/*============================================================================================*/
