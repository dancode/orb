/*==============================================================================================

    physics_api.c -- physics module wiring.
    Implements the physics_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers

    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),
    then fetch in init() and reload() with MOD_FETCH_<NAME>:

        MOD_USE_CORE;                                    // file scope
        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()
==============================================================================================*/

MOD_USE_CORE;

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
==============================================================================================*/

typedef struct physics_state_s
{
    int frame_count;

} physics_state_t;

static physics_state_t* g_state = NULL;

/*==============================================================================================
    Implementation
==============================================================================================*/

static void
physics_function_impl( void )
{
    if ( !g_state )
        return;

    g_state->frame_count++;
    LOG_TRACE( "physics_function called (frame_count=%d)", g_state->frame_count );
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const physics_api_t g_physics_api_struct = {
    .physics_function = physics_function_impl,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
physics_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( physics_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )
        return false;

    LOG_INFO( "init (state=%p)", ( void* )g_state );
    return true;
}

static bool
physics_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( physics_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )
        return false;

    LOG_INFO( "reloaded (frames so far = %d)", g_state->frame_count );
    return true;
}

static void
physics_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( core() )
        LOG_INFO( "exit" );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
physics_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( physics_state_t ),
        .func_api_size = sizeof( physics_api_t ),
        .func_api      = &g_physics_api_struct,
        .deps          = { "core" },
        .dep_count     = 1,
        .init          = physics_init,
        .exit          = physics_exit,
        .reload        = physics_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( physics )

/*============================================================================================*/
