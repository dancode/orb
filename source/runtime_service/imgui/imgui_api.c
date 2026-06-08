/*==============================================================================================

    imgui_api.c -- imgui module wiring.
    Implements the imgui_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Implementation
==============================================================================================*/

static void
imgui_tick_impl( float dt )
{
    ( void )dt;    /* TODO */
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const imgui_api_t g_imgui_api_struct = {
    .tick = imgui_tick_impl,
};

/*==============================================================================================
    Direct-call wrappers (declared in imgui_host.h)
==============================================================================================*/

void
imgui_tick( float dt )
{
    imgui_tick_impl( dt );
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
imgui_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    UNUSED( raw_state );
    return true;
}

static void
imgui_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
imgui_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = 0,
        .func_api_size = sizeof( imgui_api_t ),
        .func_api      = &g_imgui_api_struct,
        .dep_count     = 0,
        .init          = imgui_mod_init,
        .exit          = imgui_mod_exit,
        .reload        = NULL,
    };
    return &desc;
}

/*============================================================================================*/
