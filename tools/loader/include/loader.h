/*==============================================================================================

    Loader Exports

==============================================================================================*/

#pragma once
struct registry_api_t;

/*============================================================================================*/
// Convenience functions used by engine/editor executables

struct registry_api_t* loader_get_registry();
const char*            loader_get_plugin_dir();
void                   loader_load_runtime_modules( struct registry_api_t* reg, const char* plugin_dir );
void                   loader_load_editor_modules( struct registry_api_t* reg, const char* plugin_dir );

/*============================================================================================*/