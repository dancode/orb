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
#include "platform_sys/platform_sys.h"


/* compile runtime */
#include "runtime/runtime.c"

/*==============================================================================================

    engine_module.c

    Registers engine_api as a static module so any DLL can retrieve it by name:

        engine_api_t* engine = sys->get_api("engine");

    engine depends on core being initialized first — we declare that here so
    the topo-sort always places engine after core.

==============================================================================================*/

static int engine_value = 0;



/*============================================================================================*/
