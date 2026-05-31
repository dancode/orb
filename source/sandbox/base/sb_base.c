/*==============================================================================================

    sandbox/base/sb_base.c -- Sandbox for the base library.

    Links against the base static library and runs the full test suite.
    Exit code: 0 = all tests passed, non-zero = failures.

==============================================================================================*/
#include "orb.h"
#include "base/base_test.c"

int
main( int argc, char* argv[] )
{
    UNUSED( argc );
    UNUSED( argv );

    return base_run_tests();
}

/*============================================================================================*/
