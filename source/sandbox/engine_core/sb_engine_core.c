/*==============================================================================================

    sandbox/sb_engine_core.c - test system module and host.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/sys/sys.h"
#include "engine/core/core.h"

int  intern_test( void );                        // ... temporary code ...
void reflection_test( void );                    // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/

void
core_test( void )
{
    
    if ( 1 )
    {
        mem_test();           // <-- test memory system
        intern_test();        // <-- test string interning system
        reflection_test();    // <-- test reflection system
    }

    if ( 0 )
    {
        int argc = 0; char** argv = NULL;

        cvar_system_init();
        test_core_cvar( argc, argv );    // <-- test cvar system
        cvar_system_exit();
    }
    
}

/*============================================================================================*/
/* main entry point */

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    core_test();
    return 0;
}

/*============================================================================================*/