/*==============================================================================================

    engine.c 

    This is the main compilation unit for the engine. 
    It compiles the standard library and runtime

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* include dependency headers */
#include "base/orb.h"
#include "engine/core/core.h"

/* compile standard library */
#include "base/standard.c"    // TODO: static library build should compile this separately and link it in, not compile in every module
#include "engine/sys/sys.h"

/* compile runtime */
// #include "runtime/runtime.c"
/*============================================================================================*/

static int engine_value = 0;



/*============================================================================================*/
