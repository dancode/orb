/*==============================================================================================

    example_api.c -- example module wiring.
    Implements the example_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers

    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),
    then fetch in init() and reload() with MOD_FETCH_<NAME>:

        MOD_USE_CORE;                                    // file scope
        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()
==============================================================================================*/

/* none */

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
==============================================================================================*/

typedef struct example_state_s
{
    bool example_init;
    int  reload_count;
    int  counter;
    bool fail_next_reload;

} example_state_t;

static example_state_t* g_state = NULL;

/*==============================================================================================
    Implementation
==============================================================================================*/

static void
example_function_1( void )
{
    printf( "[example] function 1\n" );
}

static void
example_function_2( int value )
{
    printf( "[example] function 2 :%d\n", value );
}

static void
example_fail_next_reload( void )
{
    if ( g_state )
    {
        g_state->fail_next_reload = true;
        printf( "[example] fail_next_reload armed - next on_reload will return false\n" );
    }
}

static void
example_update( float dt )
{
    UNUSED( dt );
    g_state->counter++;
    if ( g_state->counter % 60 == 0 )
        printf( "[example] update: counter=%d\n", g_state->counter );
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const example_api_t g_example_api_struct = {
    .example_function_1 = example_function_1,
    .example_function_2 = example_function_2,
    .fail_next_reload   = example_fail_next_reload,
    .update             = example_update,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
example_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( example_state_t* )raw_state;

    example()->example_function_1();

    g_state->example_init = true;
    printf( "[example] init: example_state=%d\n", g_state->example_init );
    return true;
}

static bool
example_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( example_state_t* )raw_state;

    if ( g_state->fail_next_reload )
    {
        g_state->fail_next_reload = false;    /* one-shot -- snapshot_rollback path will succeed */
        printf( "[example] on_reload: simulating failure (returning false)\n" );
        return false;
    }

    g_state->reload_count++;
    printf( "[example] on_reload: reload_count=%d\n", g_state->reload_count );

    if ( g_state->counter > 0 )
        printf( "[example] on_reload: counter=%d\n", g_state->counter );

    printf( "\n\n VISUAL STUDIO CODE!!!! \n\n" );
    return true;
}

static void
example_exit( void* raw_state )
{
    UNUSED( raw_state );
    printf( "[example] exit: example_state=%d\n", g_state->example_init );
    printf( "[example] exit: counter=%d\n", g_state->counter );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
example_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( example_state_t ),
        .func_api_size = sizeof( example_api_t ),
        .func_api      = &g_example_api_struct,
        .deps          = { NULL },
        .dep_count     = 0,
        .init          = example_init,
        .exit          = example_exit,
        .reload        = example_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( example )

/*============================================================================================*/
