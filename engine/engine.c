/*==============================================================================================

    engine.c : (loader, api registry, simple memory/log implementation, main)

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "orb.h"
#include "core.h"

static void*
engine_alloc( size_t s, const char* tag )
{
    ( void )tag;
    return malloc( s );
}

