/*==============================================================================================

    imgui_api.c -- imgui module wiring.
    Implements the imgui_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers

    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),
    then fetch in init() and reload() with MOD_FETCH_<NAME>:

        MOD_USE_CORE;                                    // file scope
        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()
==============================================================================================*/

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
==============================================================================================*/

typedef struct imgui_state_s
{
    int32_t placeholder;    /* replace with real state fields */

} imgui_state_t;

static imgui_state_t* g_state = NULL;

/*==============================================================================================
    Implementation
==============================================================================================*/

static void
imgui_tick_impl( float dt )
{
    if ( !g_state ) return;
    ( void )dt;    /* TODO */
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const imgui_api_t g_imgui_api_struct = {
    .tick = imgui_tick_impl,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
imgui_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );    /* remove when fetching module APIs */
    g_state = ( imgui_state_t* )raw_state;
    return true;
}

static bool
imgui_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );    /* remove when fetching module APIs */
    g_state = ( imgui_state_t* )raw_state;
    return true;
}

static void
imgui_exit( void* raw_state )
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
        .state_size    = sizeof( imgui_state_t ),
        .func_api_size = sizeof( imgui_api_t ),
        .func_api      = &g_imgui_api_struct,
        .dep_count     = 0,
        .init          = imgui_init,
        .exit          = imgui_exit,
        .reload        = imgui_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( imgui )

/*============================================================================================*/
