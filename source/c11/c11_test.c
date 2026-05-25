/*==============================================================================================

    c11_test.c -- Unity entry for C11 language feature tests.

    Zero engine dependencies. Compile with /std:c11 /TC only.
    Run this executable to confirm MSVC accepts C11 features as compiled code;
    compare against VS IntelliSense squiggle behavior in c11_di.c.

==============================================================================================*/

#include "c11_di.c"

int
main( void )
{
    c11_di_run();
    return 0;
}
