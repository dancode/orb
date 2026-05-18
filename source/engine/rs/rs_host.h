#ifndef RS_HOST_H
#define RS_HOST_H
/* engine/rs/rs_host.h - Host-only. Call rs_wire_mod_callbacks() once after mod_system_init().
   See rs.md for the full boot sequence and how reflection integrates with the mod lifecycle. */

#include "engine/rs/rs.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

static inline void
rs_host_on_pre_init( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( user );
    rs_register_module( name, desc ); /* no-op when desc->rs_register is NULL */
}

static inline void
rs_host_on_post_exit( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( desc );
    UNUSED( user );
    rs_unregister_module( name ); /* silent when no frame exists for `name` */
}

/* Install pre_init / post_exit hooks. Safe to call before rs.mod_init has run. */
static inline void
rs_wire_mod_callbacks( void )
{
    mod_set_pre_init_cb ( rs_host_on_pre_init,  NULL );
    mod_set_post_exit_cb( rs_host_on_post_exit, NULL );
}

/*============================================================================================*/
#endif    // RS_HOST_H
