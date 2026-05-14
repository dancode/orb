/*==============================================================================================

    sandbox_main.c : Sandbox launcher
    
    TODO: 

    Instead of making a new .exe for each test project the sandbox launcher is a 
    single .exe that can drive any module we point it at.  
    
    It has no built-in assumptions about what modules it will run, but it does have
    hot-reload and console input handling built in.
    
    
==============================================================================================*/

#include <stdio.h>    // printf, fprintf
#include <string.h>

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/mod/mod_api.h"

/*============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    return 0;
}

/*============================================================================================*/

