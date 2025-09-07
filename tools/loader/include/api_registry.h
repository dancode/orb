#pragma once
/*============================================================================================*/

// If callers allocate names, they must also free them or register a cleanup.
// Generic API registry used by loader and plugins.

struct api_registry
{
    // register an api pointer under a name (string)
    void ( *add )( const char* name, void* api );

    // get an api pointer previously registered, or NULL
    void* ( *get )( const char* name );
};


/*============================================================================================*/