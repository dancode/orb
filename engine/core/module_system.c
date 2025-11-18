/*==============================================================================================

    Module System

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "../base/base.h"
#include "../core/core.h"
#include "../core/module_system.h"

/*==============================================================================================
    modules
==============================================================================================*/

#define MAX_MODULES 16

typedef struct module_t
{
    char         name[ MAX_MODULE_NAME ];
    lib_handle_t handle;

    // START NEW
    uint32_t           module_version;      // e.g. MODULE_VERSION_1
    uint32_t           flags;               // e.g. MODULE_FLAGS
    const char* const* required_modules;    // null terminated string name list.
    uint32_t           api_version;         // e.g. SAMPLE_GAME_API_VERSION
    void*              api_struct;          // pointer to module api struct
    // END NEW

    module_init_fn init;    // called when dll loads
    module_tick_fn tick;    // called during frame
    module_exit_fn exit;    // called on dll unload

} module_t;

static module_t g_modules[ MAX_MODULES ];
static int      g_module_count            = 0;
static char     g_module_base_path[ 256 ] = { 0 };

/*==============================================================================================
    set platform specific global base path for module loading
==============================================================================================*/

void
module_set_base_path( const char* path )
{
    strncpy( g_module_base_path, path, sizeof( g_module_base_path ) );
    g_module_base_path[ sizeof( g_module_base_path ) - 1 ] = '\0';
}

const char*
module_get_base_path( void )
{
    return g_module_base_path;
}

/*==============================================================================================
    internal helper for load and reload
==============================================================================================*/

static bool
module_get_interface( module_t* m )
{
    const char* init_name  = "module_init";
    const char* tick_name  = "module_tick";
    const char* exit_name  = "module_exit";

    m->init                = ( module_init_fn )library_get_symbol( m->handle, init_name );
    m->tick                = ( module_tick_fn )library_get_symbol( m->handle, tick_name );
    m->exit                = ( module_exit_fn )library_get_symbol( m->handle, exit_name );

    bool all_symbols_found = ( m->init && m->tick && m->exit );
    if ( all_symbols_found )
    {
        m->init( core_get_api() );
        return true;
    }

    printf( "[core] Module symbol not found: %s %s %s ", m->init ? "" : init_name, m->tick ? "" : tick_name,
            m->exit ? "" : exit_name );

    return false;
}

/*==============================================================================================
    ...
==============================================================================================*/

module_t*
module_load( const char* name, const char* path )
{
    if ( g_module_count >= MAX_MODULES )
    {
        printf( "[core] Module count exceeded maximum: %s\n", path );
        return NULL;
    }

    size_t name_len = strlen( name );
    if ( name_len > MAX_MODULE_NAME )
    {
        printf( "[core] Module name exceeds maximum length: %s\n", path );
        return NULL;
    }

    lib_handle_t h = library_load( path );
    if ( !h )
    {
        printf( "[core] Failed to load module: %s\n", path );
        return NULL;
    }

    module_t* m = &g_modules[ g_module_count++ ];
    {
        strncpy( m->name, name, MAX_MODULE_NAME );
        m->name[ MAX_MODULE_NAME - 1 ] = '\0';
        m->handle                      = h;

        if ( module_get_interface( m ) == false )
        {
            return NULL;
        }
    }

    printf( "[core] Module %s loaded\n", name );
    return m;
}

/*==============================================================================================
    ...
==============================================================================================*/

void
module_unload( module_t* m )
{
    if ( !m || !m->handle )
        return;

    if ( m->exit )
    {
        m->exit();
    }

    library_unload( m->handle );
    m->handle = NULL;
    m->init   = NULL;
    m->tick   = NULL;
    m->exit   = NULL;

    printf( "[core] Module %s unloaded\n", m->name );
}

/*==============================================================================================
    ...
==============================================================================================*/

void
module_reload( module_t* m )
{
    if ( !m )
        return;

    /* Reconstruct path using macros for portability */
    char path[ 256 ];
    snprintf( path, sizeof( path ), "%s%s%s%s", module_get_base_path(), LIB_PREFIX, m->name, LIB_EXT );

    module_unload( m );

    m->handle = library_load( path );

    module_get_interface( m );

    printf( "[core] Module %s reloaded\n", m->name );
}

/*==============================================================================================
    ...
==============================================================================================*/

void
module_call_tick( struct module_t* m )
{
    if ( m && m->tick )
    {
        m->tick( 0.0f );
    }
}

/*============================================================================================*/