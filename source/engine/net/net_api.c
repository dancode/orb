/*==============================================================================================

    net_api.c -- net module wiring.
    Implements the net_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
==============================================================================================*/

typedef struct net_state_s
{
    int32_t placeholder;    /* replace with real state fields */

} net_state_t;

/* static net_state_t* s = NULL; */

/*==============================================================================================
    Implementation
==============================================================================================*/

static void
net_tick_impl( float dt )
{
    ( void )dt;    /* TODO */
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const net_api_t g_net_api_struct = {
    .tick = net_tick_impl,
};

/*==============================================================================================
    Direct-call wrappers (declared in net_host.h)
==============================================================================================*/

void
net_tick( float dt )
{
    net_tick_impl( dt );
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
net_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    UNUSED( raw_state );
    /* s = ( net_state_t* )raw_state; */
    return true;
}

static void
net_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
net_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( net_state_t ),
        .func_api_size = sizeof( net_api_t ),
        .func_api      = &g_net_api_struct,
        .dep_count     = 0,
        .init          = net_mod_init,
        .exit          = net_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/

