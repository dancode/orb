/*==============================================================================================

    sandbox/sb_engine_core_reflect.c - Testbed for the engine reflection system.

    Runs reflection_test() which exercises registration, lookup, field iteration,
    hot-reload simulation, attributes, and diagnostics.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/ref/ref_test.c"

/*============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    ref_run_tests();
    
    return 0;
}

/*============================================================================================*/
