#ifndef RS_HOST_H
#define RS_HOST_H
/*==============================================================================================

    engine/rs/rs_host.h - rs_ module host interface.  Include in host executables only.
    Analogous to engine/mod/mod_host.h.

    Host boot sequence with rs_ as a module:

        mod_system_init();
        mod_static_load( "rs",   rs_get_mod_api() );
        mod_static_load( "sys",  sys_get_mod_api() );
        mod_static_load( "core", core_get_mod_api() );
        rs_wire_mod_callbacks();          // one call — no on_dll_load boilerplate
        mod_init_all();                   // inits rs, then sys, then core in topo order
        mod_load( my_reflected_module );  // DLL load fires rs_ registration automatically
        mod_init_all();                   // inits newly loaded modules

==============================================================================================*/

#include "engine/rs/rs.h"
#include "engine/mod/mod_host.h"

/*==============================================================================================
    rs_wire_mod_callbacks

    Wires rs_ into the mod DLL lifecycle so reflected modules are automatically registered
    and unregistered on DLL load/unload.  Call once, before the first mod_load() of a DLL.
==============================================================================================*/

static inline void
rs_host_on_dll_load( const char* name, lib_handle_t dll, void* user )
{
    UNUSED( user );
    rs_load_module_dll( name, dll );
}

static inline void
rs_host_on_dll_unload( const char* name, lib_handle_t dll, void* user )
{
    UNUSED( dll );
    UNUSED( user );
    rs_unload_module_dll( name );
}

static inline void
rs_wire_mod_callbacks( void )
{
    mod_set_dll_load_cb  ( rs_host_on_dll_load,   NULL );
    mod_set_dll_unload_cb( rs_host_on_dll_unload, NULL );
}

/*============================================================================================*/
#endif    // RS_HOST_H
