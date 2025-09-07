/*============================================================================================*/

#include "plugin.h"
#include "../../loader/include/api_registry.h"
#include "base.h"

/*============================================================================================*/

#include <stdio.h>
#include <string.h>

// small editor api struct
struct editor_api_t
{
    void ( *open_scene )( const char* path );
};

// a trivial open_scene implementation that uses foundation log

static void
editor_open_scene( const char* path )
{
    struct base_api* f = NULL;    // we'll obtain it from registry via closure
    (void)f;
    // In a richer plugin you'd store f in a static on load
    (void)path;
    printf( "[EditorPlugin] open_scene called for %s\n", path ? path : "<null>" );
}


ORB_API void
load_plugin( struct api_registry* registry )
{
    struct base_api_t* f = registry ? (struct base_api_t*)registry->get( "base_api" ) : NULL;
    if ( f && f->log )
    {
        f->log( "editor_plugin: loaded" );
    }

    // register editor api
    static struct editor_api_t editor_api = { .open_scene = editor_open_scene };

    if ( registry )
    {
        registry->add( "editor_api", &editor_api );
    }

    if ( f && f->log )
    {
        f->log( "editor_plugin: registered tm_editor_api" );
    }
}