#ifndef MODULE_SYSTEM_H
#define MODULE_SYSTEM_H
/*============================================================================================*/

#include "orb.h"
#include "../base/base.h"
struct core_api_s;

/*==============================================================================================

    module_system.c

    TODO: we need a registry system to manage module dependencies, versions, and APIs.
    TODO: text file that describes the module metadata that outlines its depdendencies.
    TODO: we can possibly create on programatically? is it possible through reflection.
    TODO:

==============================================================================================*/

// clang-format off

#define MAX_MODULE_NAME 16

typedef void ( *module_init_fn )( struct core_api_s* api );
typedef void ( *module_tick_fn )( float dt );
typedef void ( *module_exit_fn )( void );

typedef struct module_t
{
    char            name[ MAX_MODULE_NAME ];    // 
    lib_handle_t    handle;                     // 

 // uint32_t        module_version;             // e.g. MODULE_VERSION_1
 // uint32_t        flags;                      // e.g. MODULE_FLAGS (optional)

    // const char* const* required;                // null terminated string name list.
    
    // uint32_t        api_version;                // counter for hot reload
    // void*           api_struct;                 // pointer to module api struct    

    /* required function pointers */

    module_init_fn  init;                       // called when dll loads
    module_tick_fn  tick;                       // called each frame with a time delta.
    module_exit_fn  exit;                       // called on dll unload

} module_t;

/*============================================================================================*/

void             module_set_base_path( const char* path );
const char*      module_get_base_path( void );

struct module_t* module_load( const char* name, const char* path );
void             module_unload( struct module_t* mod );
void             module_reload( struct module_t* mod );
void             module_call_tick( struct module_t* m );

// void*            module_get_api( const char* name, uint32_t min_version );

/*============================================================================================*/
#endif    // MODULE_SYSTEM_H