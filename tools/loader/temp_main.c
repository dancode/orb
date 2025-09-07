/*==============================================================================================

    (Loader) Main

==============================================================================================*/

#define UNUSED( x ) (void)( x )

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple structure to track loaded modules */
struct module_handle
{
    void*                 dll_handle; /* Platform-specific handle (HMODULE on Windows, void* on Unix) */
    char                  name[ 64 ]; /* Module name for lookup */
    struct module_handle* next;       /* Linked list of modules */
};

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* 1. Parse command line arguments */
    const char* manifest_path = "engine.mf";
    const char* mode          = "runtime";

    if ( argc > 1 && strcmp( argv[ 1 ], "--editor" ) == 0 )
    {
        mode = "editor";
    }

    /* 2. Load and parse the manifest */
    struct manifest* manifest = NULL;    // parse_manifest( manifest_path );
    if ( !manifest )
    {
        fprintf( stderr, "Failed to load manifest: %s\n", manifest_path );
        return 1;
    }

    /* 3. Load modules in dependency order */
    struct module_handle* modules = NULL;    // load_modules_from_manifest( manifest, mode );
    if ( !modules )
    {
        fprintf( stderr, "Failed to load required modules\n" );
        return 1;
    }

    /* 4. Start the main loop (provided by a module) */
    int ( *engine_main )( int, char** ) = NULL;    // get_module_function( modules, "core", "engine_main" );
    if ( engine_main )
    {
        return engine_main( argc, argv );
    }

    fprintf( stderr, "No engine_main function found\n" );
    return 1;
}

/*============================================================================================*/