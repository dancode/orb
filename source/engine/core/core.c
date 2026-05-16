/*==============================================================================================

    engine/core/core.c — Unity build entry point for the core module.
    
==============================================================================================*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/*==============================================================================================
    Engine headers
==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod_export.h" /* mod_api_t, get_api_fn */
#include "engine/core/core.h"      /* Public types (no function declarations) */

/*==============================================================================================
    TEMPORARY API FNUCTIONS (until we implement a real memory system )
==============================================================================================*/

/*==============================================================================================
    API Start / Shutdown
==============================================================================================*/

static void*
core_alloc( size_t size )
{
    return malloc( size );
}

static void*
core_realloc( void* ptr, size_t size )
{
    return realloc( ptr, size );
}

static void
core_free( void* ptr )
{
    free( ptr );
}

/*==============================================================================================
    Subsystem implementations  (all functions are static within this TU)
==============================================================================================*/

#include "engine/core/core_log.c"
#include "engine/core/core_cvar.c"
#include "engine/core/core_sid.c"
// #include "engine/core/core_debug.c"
#include "engine/core/core_rs.c"
#include "engine/core/core_reflect.c"
// #include "engine/core/core_memory.c"

/*==============================================================================================
    API wiring  (must be last — assigns every static function to g_core_api_struct)
==============================================================================================*/
 
#include "engine/core/core_api.c"
