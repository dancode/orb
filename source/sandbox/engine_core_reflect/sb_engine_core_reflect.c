/*==============================================================================================

    sandbox/sb_engine_core_reflect.c - Testbed for the engine reflection system.

    Runs reflection_test() which exercises registration, lookup, field iteration,
    hot-reload simulation, attributes, and diagnostics.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/core/core.h"

void reflection_test( void );
void rs_run_tests( void );

/*============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    // reflection_test();
    rs_run_tests();
    return 0;
}

/*============================================================================================*/
