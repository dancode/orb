/*==============================================================================================

    engine.c 

    This is the main compilation unit for the engine. 
    It compiles the standard library and runtime

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* include dependency headers */
#include "orb.h"
#include "core.h"

/* compile standard library */
#include "base/standard.c"
#include "sys/sys.h"

/* compile runtime */
#include "runtime/runtime.c"
/*============================================================================================*/

static int engine_value = 0;



/*============================================================================================*/
