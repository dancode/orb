/*==============================================================================================

    host/game/game_main.c

    The retail "game.exe" host.  Optimized for one thing: running the simulation.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"

#include "host/common/host_common.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_import.h"
#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"
#include "engine/app/app_host.h"

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    launch_params_t params;
    host_args_parse( argc, argv, &params );

    /* Boot the module system and register the static engine layers in dependency order. */
    mod_system_init();
    mod_static( sys );
    mod_static( core );
    mod_static( app );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[host] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    /* Route mod and app output through core's logger now that core is live. */
    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    /* TODO: game loop */

    mod_system_exit();
    return 0;
}

/*============================================================================================*/
