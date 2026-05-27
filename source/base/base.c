/*==============================================================================================

    base.c -- the engine's (stateless) standard library implementation.

    rules: cannot allocate memory. cannot depend on the OS. included by all modules.

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    includes for platform-specific implementations
==============================================================================================*/

#include <stdio.h>     // vsnprintf
#include <string.h>    // memcpy, memmove, memset, memcmp
#include <stdarg.h>    // va_list, va_start, va_end

#include "base/base.h"

/*==============================================================================================
    unity build
==============================================================================================*/

#include "base/mem.c"
#include "base/char.c"
#include "base/bit.c"
#include "base/math.c"

#include "base/str.c"
#include "base/str_buf.c"
#include "base/str_arena.c"

/*============================================================================================*/
