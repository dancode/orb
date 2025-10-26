/*==============================================================================================

    engine_main.c

==============================================================================================*/

// #include <stdlib.h>
// #include <string.h>
// #include <stdio.h>

#include "orb.h"
#include "core/core.h"
#include "core/module_system.h"

/*============================================================================================*/

void 
test( int argc, char** argv )
{
    core_api_init();
    cvar_system_init();

    /**************************************************************/

    test_core_cvar( argc, argv );    // <-- test cvar system
    
    /**************************************************************/

    cvar_system_exit();
    core_api_exit();
}

/*============================================================================================*/

int
main( int argc, char** argv )
{
    test( argc, argv );

    ( void )argc;
    ( void )argv;

    core_init();

    struct module_t* game = module_load( "sample_game", "sample_game.dll" );
    if ( game == NULL )
    {
        return 1;
    }

    for ( int i = 0; i < 3; i++ )
    {
        module_call_tick( game );
    }

    module_reload( game );

    for ( int i = 0; i < 2; i++ )
    {
        module_call_tick( game );
    }

    module_unload( game );

    cvar_system_exit();
    return 0;
}

/*============================================================================================*/