/*==============================================================================================

    Loader Exports

==============================================================================================*/

#pragma once
struct api_registry;

/*============================================================================================*/
// Convenience functions used by engine/editor executables

struct api_registry* loader_get_registry();
const char*          loader_get_plugin_dir();
void                 loader_load_runtime_modules( struct api_registry* reg, const char* plugin_dir );
void                 loader_load_editor_modules( struct api_registry* reg, const char* plugin_dir );

/*============================================================================================*/