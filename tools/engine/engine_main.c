/*============================================================================================*/

#include "api_registry.h"
#include "loader.h"
#include "base.h"

#include <stdio.h>

/*============================================================================================*/

// extern struct registry_api_t global_registry;

/*============================================================================================*/

int
main( int argc, char** argv )
{
    (void)argc;
    (void)argv;

    struct registry_api_t* registry = loader_get_registry();

    // register foundation into the global registry
    base_register_api( registry );

    const char* plugin_dir = loader_get_plugin_dir();

    if ( plugin_dir == NULL )
        return 1;

    // engine is lean: load only runtime modules (core + game modules)
    loader_load_runtime_modules( registry, plugin_dir );    // "./lib" );

    // At this point runtime modules have registered APIs and the game would run.
    // For demo, probe core marker:
    const char* marker = (const char*)registry->get( "core_marker" );
    if ( marker )
    {
        struct base_api_t* f = (struct base_api_t*)registry->get( "base_api" );
        if ( f && f->log )
        {
            char buf[ 128 ];
            snprintf( buf, sizeof( buf ), "engine: found core marker: %s", marker );
            f->log( buf );
        }
    }

    return 0;
}

/*============================================================================================*/