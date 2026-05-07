/*==============================================================================================

    engine_test.c

    A compilation unit that allows us to test the engine without polluting the namespace
    or proper engine compilation unit. 
    
    This is useful for testing internal functions that aren't exposed

==============================================================================================*/

#include "orb.h"
#include "engine/core/core.h"

/*============================================================================================*/

int  intern_test( void );                        // ... temporary code ...
void reflection_test( void );                    // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/
/* test entry point */

void
test( int argc, char** argv )
{
    /**************************************************************/
    /* test module system */

    if ( 1 )
        return;

    /**************************************************************/
    /* test memory, string intern, and reflection systems */

    if ( 1 )
    {
        mem_test();           // <-- test memory system
        intern_test();        // <-- test string interning system
        reflection_test();    // <-- test reflection system
    }

    /**************************************************************/
    /* test cvar system */

    if ( 1 )
    {
        cvar_system_init();
        test_core_cvar( argc, argv );    // <-- test cvar system
        cvar_system_exit();
    }

    /**************************************************************/
}


/*============================================================================================*/