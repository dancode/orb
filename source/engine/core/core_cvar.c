/*==============================================================================================

    cvar_system.c

==============================================================================================*/

#include "engine/core/cvar/string_pool.h"
#include "engine/core/cvar/string_pool.c"
#include "engine/core/cvar/cvar.h"
#include "engine/core/console/console.h"
#include "engine/core/cmd/cmd.h"
#include "engine/core/cvar/cvar.c"
#include "engine/core/cmd/cmd_buffer.c"
#include "engine/core/cmd/cmd_bind.c"
#include "engine/core/cmd/cmd.c"
#include "engine/core/cvar/cvar_cmd.c"
#include "engine/core/cvar/cvar_config.c"
#include "engine/core/console/console.c"
#include "engine/core/test/test_core_cvar.c"

/*==============================================================================================

    Core-owned cvars -- registered by core_init after cvar_system_init/con_init.

==============================================================================================*/

/* log_level tracks the runtime log filter: setting it from the console retunes the logger. */

static void
core_cvar_log_level_changed( cvar_t* cv )
{
    log_set_min_level( ( log_level_t )cvar_get_int( cv ) );
}

static void
core_register_cvars( void )
{
    cvar_register_r( "version", "Engine version string", "ORB 0.1.0", CVAR_ROM );
    cvar_register_b( "developer", "Enable developer diagnostics", false, 0 );

    cvar_t* cv = cvar_register_i( 
        "log_level", "Minimum log level (0=trace 1=debug 2=info 3=warn 4=error)",
        LOG_LEVEL_INFO, LOG_LEVEL_TRACE, LOG_LEVEL_ERROR, 0 );

    cvar_callback_register( cv, core_cvar_log_level_changed );
}

/*============================================================================================*/
