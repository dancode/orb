/*==============================================================================================

    c11_test.c -- Unity entry for C11 language feature tests.

    Zero engine dependencies. Compile with /std:c11 /TC only.
    Run this executable to confirm MSVC accepts C11 features as compiled code;
    compare against VS IntelliSense squiggle behavior in c11_di.c.

==============================================================================================*/

#include "c11_di.c"

typedef struct
{
    int   x;
    int   y;
    float z;
} vec3X_t;

static void
test_basic_X( void )
{
    /* Basic designated initializer -- C99/C11 standard, should always compile. */
    vec3X_t v = { .x = 1, .y = 2, .z = 3.0f };
    printf( "test_basic_X: x=%d y=%d z=%.1f\n", v.x, v.y, v.z );
}


int
main( void )
{
    c11_di_run();
    return 0;
}
