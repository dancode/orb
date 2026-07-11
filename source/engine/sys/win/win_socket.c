/*==============================================================================================

    win_socket.c - Windows (Winsock2) implementation of the UDP socket API.

    All sockets are non-blocking. Byte-order conversion between sys_addr_t (ip bytes in
    network order, port in host order) and sockaddr happens only in this file.

==============================================================================================*/

_Static_assert( sizeof( SOCKET ) <= sizeof( sys_socket_t ), "SOCKET larger than sys_socket_t" );

/* Not exposed by every SDK header set; the canonical value from mstcpip.h. */
#ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET _WSAIOW( IOC_VENDOR, 12 )
#endif

static int s_net_refcount = 0;

bool
sys_net_init( void )
{
    if ( s_net_refcount == 0 )
    {
        WSADATA data;
        if ( WSAStartup( MAKEWORD( 2, 2 ), &data ) != 0 ) return false;
    }
    s_net_refcount++;
    return true;
}

void
sys_net_shutdown( void )
{
    if ( s_net_refcount <= 0 ) return;
    s_net_refcount--;
    if ( s_net_refcount == 0 ) WSACleanup();
}

/*==============================================================================================
    sockaddr conversion
==============================================================================================*/

/* sys_addr_t -> sockaddr. Returns the sockaddr byte size, or 0 for an invalid address. */
static int
_addr_to_sockaddr( const sys_addr_t* a, struct sockaddr_storage* out )
{
    memset( out, 0, sizeof( *out ) );

    if ( a->type == SYS_ADDR_IPV4 )
    {
        struct sockaddr_in* sa = ( struct sockaddr_in* )out;
        sa->sin_family         = AF_INET;
        sa->sin_port           = htons( a->port );
        memcpy( &sa->sin_addr, a->ip, 4 );
        return sizeof( struct sockaddr_in );
    }

    if ( a->type == SYS_ADDR_IPV6 )
    {
        struct sockaddr_in6* sa = ( struct sockaddr_in6* )out;
        sa->sin6_family         = AF_INET6;
        sa->sin6_port           = htons( a->port );
        memcpy( &sa->sin6_addr, a->ip, 16 );
        return sizeof( struct sockaddr_in6 );
    }

    return 0;
}

static void
_sockaddr_to_addr( const struct sockaddr* sa, sys_addr_t* out )
{
    memset( out, 0, sizeof( *out ) );

    if ( sa->sa_family == AF_INET )
    {
        const struct sockaddr_in* sa4 = ( const struct sockaddr_in* )sa;
        out->type                     = SYS_ADDR_IPV4;
        out->port                     = ntohs( sa4->sin_port );
        memcpy( out->ip, &sa4->sin_addr, 4 );
    }
    else if ( sa->sa_family == AF_INET6 )
    {
        const struct sockaddr_in6* sa6 = ( const struct sockaddr_in6* )sa;
        out->type                      = SYS_ADDR_IPV6;
        out->port                      = ntohs( sa6->sin6_port );
        memcpy( out->ip, &sa6->sin6_addr, 16 );
    }
}

/*==============================================================================================
    Socket lifecycle
==============================================================================================*/

sys_socket_t
sys_socket_open_udp( const sys_addr_t* bind_addr )
{
    /* NULL binds 0.0.0.0 with an ephemeral port. */
    sys_addr_t any4 = { 0 };
    if ( !bind_addr )
    {
        any4.type = SYS_ADDR_IPV4;
        bind_addr = &any4;
    }

    int family = ( bind_addr->type == SYS_ADDR_IPV6 ) ? AF_INET6 : AF_INET;

    SOCKET s = socket( family, SOCK_DGRAM, IPPROTO_UDP );
    if ( s == INVALID_SOCKET ) return SYS_SOCKET_INVALID;

    /* One address family per socket -- deterministic behavior, no dual-stack surprises. */
    if ( family == AF_INET6 )
    {
        DWORD v6only = 1;
        setsockopt( s, IPPROTO_IPV6, IPV6_V6ONLY, ( const char* )&v6only, sizeof( v6only ) );
    }

    /* Large kernel buffers so packet bursts survive a slow frame. */
    int buf_size = 256 * 1024;
    setsockopt( s, SOL_SOCKET, SO_RCVBUF, ( const char* )&buf_size, sizeof( buf_size ) );
    setsockopt( s, SOL_SOCKET, SO_SNDBUF, ( const char* )&buf_size, sizeof( buf_size ) );

    /* Disable the ICMP port-unreachable -> WSAECONNRESET behavior. Without this a reset can
       poison the socket and stall delivery; every UDP game stack turns it off. */
    {
        BOOL  behavior = FALSE;
        DWORD bytes    = 0;
        WSAIoctl( s, SIO_UDP_CONNRESET, &behavior, sizeof( behavior ), NULL, 0, &bytes, NULL, NULL );
    }

    struct sockaddr_storage sa;
    int                     sa_size = _addr_to_sockaddr( bind_addr, &sa );
    if ( bind( s, ( struct sockaddr* )&sa, sa_size ) == SOCKET_ERROR )
    {
        closesocket( s );
        return SYS_SOCKET_INVALID;
    }

    u_long non_blocking = 1;
    if ( ioctlsocket( s, FIONBIO, &non_blocking ) == SOCKET_ERROR )
    {
        closesocket( s );
        return SYS_SOCKET_INVALID;
    }

    return ( sys_socket_t )s;
}

void
sys_socket_close( sys_socket_t s )
{
    if ( s == SYS_SOCKET_INVALID ) return;
    closesocket( ( SOCKET )s );
}

bool
sys_socket_bound_addr( sys_socket_t s, sys_addr_t* out )
{
    struct sockaddr_storage sa;
    int                     sa_size = sizeof( sa );
    if ( getsockname( ( SOCKET )s, ( struct sockaddr* )&sa, &sa_size ) == SOCKET_ERROR ) return false;
    _sockaddr_to_addr( ( const struct sockaddr* )&sa, out );
    return true;
}

/*==============================================================================================
    Send / receive
==============================================================================================*/

int
sys_socket_send( sys_socket_t s, const sys_addr_t* to, const void* data, int size )
{
    struct sockaddr_storage sa;
    int                     sa_size = _addr_to_sockaddr( to, &sa );
    if ( sa_size == 0 ) return -1;

    int sent = sendto( ( SOCKET )s, ( const char* )data, size, 0, ( struct sockaddr* )&sa, sa_size );
    if ( sent == SOCKET_ERROR )
    {
        /* A full send buffer drops the datagram -- normal UDP behavior, not an error. */
        return ( WSAGetLastError() == WSAEWOULDBLOCK ) ? 0 : -1;
    }
    return sent;
}

int
sys_socket_recv( sys_socket_t s, sys_addr_t* from, void* buf, int cap )
{
    for ( ;; )
    {
        struct sockaddr_storage sa;
        int                     sa_size = sizeof( sa );

        int got = recvfrom( ( SOCKET )s, ( char* )buf, cap, 0, ( struct sockaddr* )&sa, &sa_size );
        if ( got > 0 )
        {
            if ( from ) _sockaddr_to_addr( ( const struct sockaddr* )&sa, from );
            return got;
        }
        if ( got == 0 ) continue;    /* zero-length datagram -- drop, keep draining */

        int err = WSAGetLastError();
        if ( err == WSAEWOULDBLOCK ) return 0;
        if ( err == WSAECONNRESET ) continue;    /* ICMP port unreachable -- skip, keep draining */
        if ( err == WSAEMSGSIZE ) continue;      /* datagram larger than cap -- drop it */
        return -1;
    }
}

/*==============================================================================================
    Address utilities
==============================================================================================*/

bool
sys_addr_parse( const char* str, sys_addr_t* out )
{
    memset( out, 0, sizeof( *out ) );
    if ( !str || !str[ 0 ] ) return false;

    char host[ 64 ];

    /* "[ipv6]:port" */
    if ( str[ 0 ] == '[' )
    {
        const char* end = strchr( str, ']' );
        if ( !end ) return false;

        usize len = ( usize )( end - str ) - 1;
        if ( len == 0 || len >= sizeof( host ) ) return false;
        memcpy( host, str + 1, len );
        host[ len ] = 0;

        struct in6_addr v6;
        if ( inet_pton( AF_INET6, host, &v6 ) != 1 ) return false;

        out->type = SYS_ADDR_IPV6;
        memcpy( out->ip, &v6, 16 );
        if ( end[ 1 ] == ':' ) out->port = ( u16 )atoi( end + 2 );
        return true;
    }

    /* More than one ':' means a bare ipv6 address with no port. */
    const char* first_colon = strchr( str, ':' );
    const char* last_colon  = strrchr( str, ':' );
    if ( first_colon && first_colon != last_colon )
    {
        struct in6_addr v6;
        if ( inet_pton( AF_INET6, str, &v6 ) != 1 ) return false;

        out->type = SYS_ADDR_IPV6;
        memcpy( out->ip, &v6, 16 );
        return true;
    }

    /* "ipv4" or "ipv4:port" */
    const char* host_str = str;
    if ( last_colon )
    {
        usize len = ( usize )( last_colon - str );
        if ( len == 0 || len >= sizeof( host ) ) return false;
        memcpy( host, str, len );
        host[ len ] = 0;
        host_str  = host;
        out->port = ( u16 )atoi( last_colon + 1 );
    }

    struct in_addr v4;
    if ( inet_pton( AF_INET, host_str, &v4 ) != 1 )
    {
        out->port = 0;
        return false;
    }

    out->type = SYS_ADDR_IPV4;
    memcpy( out->ip, &v4, 4 );
    return true;
}

void
sys_addr_to_string( const sys_addr_t* a, char* out, int size )
{
    if ( size <= 0 ) return;
    out[ 0 ] = 0;

    char ip[ 64 ] = "";

    if ( a->type == SYS_ADDR_IPV4 )
    {
        struct in_addr v4;
        memcpy( &v4, a->ip, 4 );
        inet_ntop( AF_INET, &v4, ip, sizeof( ip ) );

        if ( a->port )
            snprintf( out, ( usize )size, "%s:%u", ip, a->port );
        else
            snprintf( out, ( usize )size, "%s", ip );
    }
    else if ( a->type == SYS_ADDR_IPV6 )
    {
        struct in6_addr v6;
        memcpy( &v6, a->ip, 16 );
        inet_ntop( AF_INET6, &v6, ip, sizeof( ip ) );

        if ( a->port )
            snprintf( out, ( usize )size, "[%s]:%u", ip, a->port );
        else
            snprintf( out, ( usize )size, "%s", ip );
    }
    else
    {
        snprintf( out, ( usize )size, "<none>" );
    }
}

bool
sys_addr_equal( const sys_addr_t* a, const sys_addr_t* b )
{
    if ( a->type != b->type || a->port != b->port ) return false;
    if ( a->type == SYS_ADDR_IPV4 ) return memcmp( a->ip, b->ip, 4 ) == 0;
    if ( a->type == SYS_ADDR_IPV6 ) return memcmp( a->ip, b->ip, 16 ) == 0;
    return true;
}

sys_addr_t
sys_addr_ipv4( u8 a, u8 b, u8 c, u8 d, u16 port )
{
    sys_addr_t addr = { 0 };
    addr.type       = SYS_ADDR_IPV4;
    addr.ip[ 0 ]    = a;
    addr.ip[ 1 ]    = b;
    addr.ip[ 2 ]    = c;
    addr.ip[ 3 ]    = d;
    addr.port       = port;
    return addr;
}

bool
sys_addr_resolve( const char* hostname, u16 port, sys_addr_t* out )
{
    memset( out, 0, sizeof( *out ) );

    struct addrinfo hints = { 0 };
    hints.ai_family       = AF_UNSPEC;
    hints.ai_socktype     = SOCK_DGRAM;

    struct addrinfo* results = NULL;
    if ( getaddrinfo( hostname, NULL, &hints, &results ) != 0 || !results ) return false;

    /* Prefer ipv4; fall back to the first result of any family. */
    const struct addrinfo* pick = results;
    for ( const struct addrinfo* it = results; it; it = it->ai_next )
    {
        if ( it->ai_family == AF_INET )
        {
            pick = it;
            break;
        }
    }

    _sockaddr_to_addr( pick->ai_addr, out );
    out->port = port;

    freeaddrinfo( results );
    return out->type != SYS_ADDR_NONE;
}

/*============================================================================================*/
