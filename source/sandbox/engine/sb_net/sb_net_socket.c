/*==============================================================================================

    sb_net_socket.c -- Layer 1 tests: address utilities and UDP loopback exchange.

    NOTE: on some Windows machines (observed on Win11 26200 with Defender active) a fresh
    IPv4-loopback UDP flow is permanently blackholed in the reply direction ~25% of the time
    -- resends on the same socket pair never arrive, while new socket pairs work. Verified
    machine-level with an independent .NET repro; IPv6 loopback and real adapter addresses
    are unaffected. The exchange test therefore retries with a fresh socket pair.

==============================================================================================*/

/* Send `msg` from tx to `to` and poll rx until it lands. UDP guarantees nothing, so the
   send is retried a few times with a ~100 ms recv window each. Returns the received byte
   count, 0 on total loss. */
static int
send_recv_wait( sys_socket_t tx, sys_socket_t rx, const sys_addr_t* to, const void* msg, int len,
                sys_addr_t* from, void* buf, int cap )
{
    for ( int attempt = 0; attempt < 3; attempt++ )
    {
        if ( sys_socket_send( tx, to, msg, len ) != len ) continue;

        for ( int i = 0; i < 100; i++ )
        {
            int got = sys_socket_recv( rx, from, buf, cap );
            if ( got != 0 ) return got;
            thread_sleep_ms( 1 );
        }
    }
    return 0;
}

/*==============================================================================================
    Address parse / format / compare
==============================================================================================*/

static void
net_test_addr( void )
{
    printf( "  address utilities\n" );

    sys_addr_t a, b;
    char       str[ 64 ];

    sb_check( sys_addr_parse( "127.0.0.1:5000", &a ), "parse ipv4:port" );
    sb_check( a.type == SYS_ADDR_IPV4 && a.port == 5000, "ipv4 fields" );
    sb_check( a.ip[ 0 ] == 127 && a.ip[ 3 ] == 1, "ipv4 octets" );

    sys_addr_to_string( &a, str, sizeof( str ) );
    sb_check( strcmp( str, "127.0.0.1:5000" ) == 0, "ipv4 to_string round-trip" );

    b = sys_addr_ipv4( 127, 0, 0, 1, 5000 );
    sb_check( sys_addr_equal( &a, &b ), "ipv4 ctor equals parse" );

    b.port = 5001;
    sb_check( !sys_addr_equal( &a, &b ), "port mismatch detected" );

    sb_check( sys_addr_parse( "[::1]:9000", &a ), "parse [ipv6]:port" );
    sb_check( a.type == SYS_ADDR_IPV6 && a.port == 9000, "ipv6 fields" );
    sys_addr_to_string( &a, str, sizeof( str ) );
    sb_check( strcmp( str, "[::1]:9000" ) == 0, "ipv6 to_string round-trip" );

    sb_check( sys_addr_parse( "fe80::1", &a ), "parse bare ipv6" );
    sb_check( a.type == SYS_ADDR_IPV6 && a.port == 0, "bare ipv6 fields" );

    sb_check( sys_addr_parse( "10.0.0.1", &a ), "parse bare ipv4" );
    sb_check( a.type == SYS_ADDR_IPV4 && a.port == 0, "bare ipv4 fields" );

    sb_check( !sys_addr_parse( "not an address", &a ), "garbage rejected" );
    sb_check( !sys_addr_parse( "", &a ), "empty rejected" );
}

/*==============================================================================================
    Loopback exchange -- two ephemeral sockets talk to each other
==============================================================================================*/

/* One full exchange on a fresh socket pair: open, a->b, b->a reply, close.
   Returns true when both legs delivered intact payloads from the expected senders. */
static bool
loopback_exchange_once( const sys_addr_t* bind_addr, bool* out_open_failed )
{
    *out_open_failed = false;

    sys_socket_t sock_a = sys_socket_open_udp( bind_addr );
    sys_socket_t sock_b = sys_socket_open_udp( bind_addr );
    if ( sock_a == SYS_SOCKET_INVALID || sock_b == SYS_SOCKET_INVALID )
    {
        *out_open_failed = true;
        sys_socket_close( sock_a );
        sys_socket_close( sock_b );
        return false;
    }

    sys_addr_t addr_a = { 0 }, addr_b = { 0 };
    bool       ok = sys_socket_bound_addr( sock_a, &addr_a ) && sys_socket_bound_addr( sock_b, &addr_b ) &&
              addr_a.port != 0 && addr_b.port != 0 && addr_a.port != addr_b.port;

    /* Nothing pending on a fresh socket. */
    char buf[ 256 ];
    ok = ok && sys_socket_recv( sock_a, NULL, buf, sizeof( buf ) ) == 0;

    /* A -> B */
    const char* msg  = "hello from a";
    int         len  = ( int )strlen( msg ) + 1;
    sys_addr_t  from = { 0 };

    if ( ok )
    {
        int got = send_recv_wait( sock_a, sock_b, &addr_b, msg, len, &from, buf, sizeof( buf ) );
        ok      = got == len && strcmp( buf, msg ) == 0 && from.port == addr_a.port;
    }

    /* B -> A, replying straight to the observed source address. */
    if ( ok )
    {
        const char* reply    = "hello back from b";
        sys_addr_t  reply_to = from;
        len                  = ( int )strlen( reply ) + 1;

        int got = send_recv_wait( sock_b, sock_a, &reply_to, reply, len, &from, buf, sizeof( buf ) );
        ok      = got == len && strcmp( buf, reply ) == 0 && from.port == addr_b.port;
    }

    sys_socket_close( sock_a );
    sys_socket_close( sock_b );
    return ok;
}

static void
net_test_loopback( const sys_addr_t* bind_addr, const char* label )
{
    printf( "  %s loopback\n", label );

    /* A fresh pair re-rolls the flow, dodging the per-flow blackhole (see file header). */
    for ( int pair = 0; pair < 5; pair++ )
    {
        bool open_failed = false;
        if ( loopback_exchange_once( bind_addr, &open_failed ) )
        {
            if ( pair > 0 ) printf( "    note: passed on socket pair %d (flow blackhole dodged)\n", pair + 1 );
            sb_check( true, "loopback exchange" );
            return;
        }
        if ( open_failed )
        {
            sb_check( false, "sockets open" );
            return;
        }
    }
    sb_check( false, "loopback exchange (5 socket pairs)" );
}

/*==============================================================================================
    Suite entry
==============================================================================================*/

static void
net_test_socket( void )
{
    sb_check( sys_net_init(), "sys_net_init" );

    net_test_addr();

    sys_addr_t loop4 = sys_addr_ipv4( 127, 0, 0, 1, 0 );
    net_test_loopback( &loop4, "ipv4" );

    sys_addr_t loop6;
    if ( sys_addr_parse( "[::1]:0", &loop6 ) )
        net_test_loopback( &loop6, "ipv6" );

    sys_net_shutdown();
}

/*============================================================================================*/
