/*============================================================================================*/

#include "plugin.h"
#include "../../../tools/loader/include/api_registry.h"
#include "base.h"

#include <stdio.h>
#include <string.h>

/*============================================================================================*/

ORB_API void
load_plugin( struct api_registry* registry )
{
    // Core expects foundation to be present
    struct base_api_t* f = registry ? (struct base_api_t*)registry->get( "base_api" ) : NULL;
    if ( f && f->log )
    {
        f->log( "core: loaded" );
    }

    // Core could register its own APIs for others to use, e.g. reflection
    // For demo, core registers a tiny "core_marker"

    const char* marker = "core_v1_marker";
    if ( registry && registry->add )
    {
        registry->add( "core_marker", (void*)marker );
    }

    if ( f && f->log )
    {
        char buf[ 128 ];
        snprintf( buf, sizeof( buf ), "core: registered marker '%s'", marker );
        f->log( buf );
    }
}

/*============================================================================================*/