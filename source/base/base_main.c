#include "orb.h"

#include "base/base.c"
#include "base/base_test.c"


int base_run_tests( void );

int
main( int argc, char* argv[] )
{
    UNUSED( argc );
    UNUSED( argv );

    base_run_tests();
    return 0;
}

