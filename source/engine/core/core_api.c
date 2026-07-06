/*==============================================================================================

    engine/core/core_api.c : Platform-agnostic core module wiring.

    Included LAST by core.c. By the time this file is processed in the unity
    build, every subsystem's static functions are visible in the translation
    unit and can be assigned to the function-pointer slots of g_core_api_struct.

==============================================================================================*/
#include "engine/mod/mod_export.h"

#include "core.generated.h"
#include "engine/core/core_api.h"

/*==============================================================================================
    API Start / Shutdown
==============================================================================================*/

void
core_init( void )
{
    log_init();
    sid_init();
    cvar_system_init();
    cmd_system_init();           /* command backend before registrants (echo/help/wait built-ins) */
    con_init();                  /* console view: registers clear/history into the backend */
    cvar_register_commands();    /* set/seta/toggle/cvarlist/... into the backend registry */
    core_register_cvars();       /* core-owned cvars: version, developer, log_level */
}

void
core_exit( void )
{
    con_exit();
    cmd_system_exit();
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

/* Natvis anchor: the exe's g_debug_api. Each DLL defines its own via CORE_DEBUG_API_DECL. */
core_debug_api_t* g_debug_api = &g_core_debug_api_struct;

/*==============================================================================================
    API struct
==============================================================================================*/

const core_api_t g_core_api_struct = {

    .debug_api          = &g_core_debug_api_struct,

    .assert_report      = assert_report,

    .log_write          = log_write,
    .log_set_min_level  = log_set_min_level,
    .log_add_sink       = log_add_sink,
    .log_remove_sink    = log_remove_sink,
    .log_ring_entries   = log_ring_entries,
    .log_ring_capacity  = log_ring_capacity,
    .log_ring_seq       = log_ring_seq,

    .alloc              = core_alloc,
    .realloc            = core_realloc,
    .free               = core_free,

    .sid_intern         = sid_intern,
    .sid_intern_cstr    = sid_intern_cstr,
    .sid_find_cstr      = sid_find_cstr,
    .sid_cstr           = sid_cstr,
    .sid_length         = sid_length,
    .sid_is_canonical   = sid_is_canonical,
    .sid_get_hash       = sid_get_hash,
    .sid_print_stats    = sid_print_stats,
    .sid_reset_stats    = sid_reset_stats,

    .cvar_register_b    = cvar_register_b,
    .cvar_register_i    = cvar_register_i,
    .cvar_register_f    = cvar_register_f,
    .cvar_register_s    = cvar_register_s,
    .cvar_register_w    = cvar_register_w,
    .cvar_register_r    = cvar_register_r,

    .cvar_find          = cvar_find,
    .cvar_get_by_index  = cvar_get_by_index,
    .cvar_get_count     = cvar_get_count,

    .cvar_get_name      = cvar_get_name,
    .cvar_get_desc      = cvar_get_desc,
    .cvar_get_bool      = cvar_get_bool,
    .cvar_get_int       = cvar_get_int,
    .cvar_get_float     = cvar_get_float,
    .cvar_get_string    = cvar_get_string,

    .cvar_set_value     = cvar_set_value,
    .cvar_get_value     = cvar_get_value,
    .cvar_reset         = cvar_reset,

    .cvar_callback_register   = cvar_callback_register,
    .cvar_callback_unregister = cvar_callback_unregister,

    .cmd_register       = cmd_register,
    .cmd_unregister     = cmd_unregister,
    .cmd_count          = cmd_count,
    .cmd_name           = cmd_name,
    .cmd_desc           = cmd_desc,

    .cmd_execute_string = cmd_execute_string,
    .cmd_queue          = cmd_queue,
    .cmd_queue_front    = cmd_queue_front,
    .cmd_pump           = cmd_pump,

    .con_print          = con_print,
    .con_printf         = con_printf,
    .con_clear          = con_clear,
    .con_exec           = con_exec,

    .con_line_count     = con_line_count,
    .con_line_get       = con_line_get,

    .con_history_count  = con_history_count,
    .con_history_get    = con_history_get,
    .con_complete       = con_complete,

};

/*==============================================================================================
    Module lifecycle  (called by the module system)
==============================================================================================*/

bool
core_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    core_init();
    return true;
}

void
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
        .deps          = { "sys", "ref" },
        .dep_count     = 2,
        .init          = core_mod_init,
        .exit          = core_mod_exit,
        .reload        = NULL,
        .ref_register   = MOD_REFLECT_FUNC( core ),
    };
    return &api;
}

/*============================================================================================*/
