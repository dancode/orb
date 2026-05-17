/*==============================================================================================

    engine/core/core_api.c : Platform-agnostic core module wiring.

    Included LAST by core.c. By the time this file is processed in the unity
    build, every subsystem's static functions are visible in the translation
    unit and can be assigned to the function-pointer slots of g_core_api_struct.

==============================================================================================*/
/*==============================================================================================
    API Start / Shutdown
==============================================================================================*/

void
core_init( void )
{
    log_init();
    sid_init();
    cvar_system_init();
}

void
core_exit( void )
{
    cvar_system_exit();
    sid_exit();
    log_exit();
}

/*==============================================================================================
    Persistent state (allocated by module init)
==============================================================================================*/

typedef struct
{
    uint32_t log_count;

} core_state_t;

static core_state_t* s = NULL;

/*============================================================================================*/
/* required to debug natvis data from dll's (we must import reference to global data ) */

extern string_pool_t      g_cvar_string_pool;    // cvar data for strings
extern user_string_pool_t g_user_string_pool;    // cvar data for user strings
extern intern_state_t     g_intern;              // sid string data

static core_debug_api_t   g_core_debug_api_struct = {
      .string_pool      = &g_cvar_string_pool,
      .user_string_pool = &g_user_string_pool,
      .intern_arena     = &g_intern.arena,
};

/*==============================================================================================
    API struct
==============================================================================================*/

const core_api_t g_core_api_struct = {

    .debug_api         = &g_core_debug_api_struct,

    .log               = log_default,
    .log_info          = log_info,
    .log_warn          = log_warn,
    .log_error         = log_error,
    .log_set_min_level = log_set_min_level,

    .alloc             = core_alloc,
    .realloc           = core_realloc,
    .free              = core_free,

    // .cvar_find = cvar_find,
    // .cvar_register = cvar_register,
    // .cvar_get_int  = cvar_get_int,
    // .cvar_set_int  = cvar_set_int,
    // .cvar_set_string = cvar_set_string,
    // .cvar_get_string = cvar_get_string,

};

/*==============================================================================================
    Module lifecycle  (called by the module system)
==============================================================================================*/

static bool
core_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    core_init();
    return true;
}

static void
core_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    core_exit();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
core_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( core_state_t ),
        .func_api_size = sizeof( core_api_t ),
        .func_api      = ( void* )&g_core_api_struct,
        .deps          = { "sys", "rs" },
        .dep_count     = 2,
        .init          = core_mod_init,
        .exit          = core_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
