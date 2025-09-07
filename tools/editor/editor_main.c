#include "api_registry.h"
#include "loader.h"
#include "base.h"
#include <stdio.h>

extern struct tm_api_registry global_registry;

int
main( int argc, char** argv )
{
    (void)argc;
    (void)argv;

    struct api_registry* registry = loader_get_registry();

    // register foundation into the global registry
    base_register_api( registry );

    const char* plugin_dir = loader_get_plugin_dir();

    if ( plugin_dir == NULL )
        return 1;

    // Editor loads runtime modules + editor modules, injecting tooling
    loader_load_editor_modules( registry, plugin_dir );

    // Example: editor might find an editor api and open UI etc.
    void*              editor_api = registry->get( "editor_api" );
    struct base_api_t* f          = (struct base_api_t*)registry->get( "base_api" );

    if ( f && f->log )
    {
        f->log( "Editor: startup complete" );
        if ( editor_api )
            f->log( "Editor: found editor api" );
        else
            f->log( "Editor: editor api not present" );
    }

    // The editor wraps runtime modules: e.g. it might call into core to ask for scene data and open UI.
    return 0;
}