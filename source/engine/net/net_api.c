/*==============================================================================================

    net_api.c -- net module wiring.
    Implements the net_api_t vtable struct and the mod_desc_t lifecycle descriptor.
    The _impl functions live in the unity-included transport files.

==============================================================================================*/

/*==============================================================================================
    API Struct
==============================================================================================*/

const net_api_t g_net_api_struct = {
    .peer_create     = net_peer_create_impl,
    .peer_destroy    = net_peer_destroy_impl,
    .peer_addr       = net_peer_addr_impl,
    .peer_connect    = net_peer_connect_impl,
    .peer_disconnect = net_peer_disconnect_impl,
    .peer_update     = net_peer_update_impl,
    .peer_poll       = net_peer_poll_impl,
    .peer_send       = net_peer_send_impl,
    .peer_stats      = net_peer_stats_impl,
    .peer_sim        = net_peer_sim_impl,
};

/*==============================================================================================
    Direct-call wrappers (declared in net_host.h)
==============================================================================================*/

net_peer_t*
net_peer_create( const net_config_t* cfg )
{
    return net_peer_create_impl( cfg );
}

void
net_peer_destroy( net_peer_t* p )
{
    net_peer_destroy_impl( p );
}

bool
net_peer_addr( net_peer_t* p, sys_addr_t* out )
{
    return net_peer_addr_impl( p, out );
}

net_conn_t
net_peer_connect( net_peer_t* p, const sys_addr_t* addr )
{
    return net_peer_connect_impl( p, addr );
}

void
net_peer_disconnect( net_peer_t* p, net_conn_t conn )
{
    net_peer_disconnect_impl( p, conn );
}

void
net_peer_update( net_peer_t* p, f64 now_seconds )
{
    net_peer_update_impl( p, now_seconds );
}

bool
net_peer_poll( net_peer_t* p, net_event_t* ev )
{
    return net_peer_poll_impl( p, ev );
}

bool
net_peer_send( net_peer_t* p, net_conn_t conn, i32 channel, const void* data, i32 size )
{
    return net_peer_send_impl( p, conn, channel, data, size );
}

bool
net_peer_stats( net_peer_t* p, net_conn_t conn, net_stats_t* out )
{
    return net_peer_stats_impl( p, conn, out );
}

void
net_peer_sim( net_peer_t* p, const net_sim_t* sim )
{
    net_peer_sim_impl( p, sim );
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
net_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    return sys_net_init();
}

static void
net_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    sys_net_shutdown();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
net_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( net_api_t ),
        .func_api      = &g_net_api_struct,
        .deps          = { "sys" },
        .dep_count     = 1,
        .init          = net_mod_init,
        .exit          = net_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/
