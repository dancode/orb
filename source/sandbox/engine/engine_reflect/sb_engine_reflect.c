/*==============================================================================================

    sandbox/sb_engine_core_reflect.c - Testbed for the engine reflection system.

    Runs reflection_test() which exercises registration, lookup, field iteration,
    hot-reload simulation, attributes, and diagnostics.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/rs/rs_test.c"

/*============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    rs_run_tests();
    
    return 0;
}

/*============================================================================================*/
