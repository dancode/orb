/*==============================================================================================

    engine_module.c

    Registers engine_api as a static module so any DLL can retrieve it by name:

        engine_api_t* engine = sys->get_api("engine");

    engine depends on core being initialized first — we declare that here so
    the topo-sort always places engine after core.

==============================================================================================*/

// #include "engine_api.h"
// #include "module_api.h"
// #include "module_sys.h"
// 
// static bool
// engine_mod_init( void* state, module_sys_api_t* sys )
// {
//     /* Engine has no module-level init work — the engine itself is already
//        running before the module system starts. */
//     ( void )state;
//     ( void )sys;
//     return true;
// }
// 
// static module_api_t g_engine_module_api = {
//     .version    = 1,
//     .state_size = 0,
//     .deps       = { "core" },
//     .dep_count  = 1,
// 
//     .init       = engine_mod_init,
//     .tick       = NULL,
//     .exit       = NULL,
//     .on_reload  = NULL,
// };
// 
// void
// engine_module_register( void )
// {
//     module_register_static( "engine", &g_engine_module_api, engine_get_api() );
// }
