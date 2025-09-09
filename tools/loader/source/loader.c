/*==============================================================================================

    (Loader) Main

==============================================================================================*/

#include "api_registry.h"
#include "loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "registry.c"

/*==============================================================================================

    OS Module Support

==============================================================================================*/

#ifdef _WIN32

typedef void*         HMODULE;
typedef const char*   LPCSTR;
typedef void*         FARPROC;
typedef unsigned long DWORD;

__declspec( dllimport ) HMODULE __stdcall LoadLibraryA( LPCSTR lpLibFileName );
__declspec( dllimport ) FARPROC __stdcall GetProcAddress( HMODULE hModule, LPCSTR lpProcName );
__declspec( dllimport ) DWORD __stdcall GetModuleFileNameA( HMODULE hModule, char* lpFilename, DWORD nSize );

#else

#    include <dlfcn.h>
#    include <unistd.h>
typedef void* HMODULE;
typedef void* FARPROC;

#endif

/*============================================================================================*/

typedef HMODULE lib_handle_t;

#ifdef _WIN32

static lib_handle_t
load_lib( const char* path )
{
    return LoadLibraryA( path );
}
static void*
get_sym( lib_handle_t h, const char* s )
{
    return (void*)GetProcAddress( h, s );
}

#else

static lib_handle_t
load_lib( const char* path )
{
    return dlopen( path, RTLD_NOW );
}
static void*
get_sym( lib_handle_t h, const char* s )
{
    return dlsym( h, s );
}

#endif

/*==============================================================================================

    (Loader) API Registry

==============================================================================================*/

#define MAX_APIS 32

static const char* api_names[ MAX_APIS ];
static void*       api_table[ MAX_APIS ];
static int         api_count = 0;

/*============================================================================================*/

static void
registry_add( const char* name, void* api )
{
    if ( api_count >= MAX_APIS )
    {
        fprintf( stderr, "API registry overflow\n" );
        return;
    }
    api_names[ api_count ] = _strdup( name );
    api_table[ api_count ] = api;
    api_count++;
}

static void*
registry_get( const char* name )
{
    for ( int i = 0; i < api_count; ++i )
    {
        if ( strcmp( api_names[ i ], name ) == 0 )
        {
            return api_table[ i ];
        }
    }
    return NULL;
}

/*============================================================================================*/

// Public registry instance to be shared when calling plugin load functions.
struct registry_api_t global_registry_api = {
    .add = registry_add,
    .get = registry_get,
};

struct registry_api_t*
loader_get_registry()
{
    return &global_registry_api;
}

/*==============================================================================================

    (Loader) Load Module

==============================================================================================*/

static int
loader_load_plugin( const char* path, struct registry_api_t* reg )
{
    lib_handle_t handle = load_lib( path );
    if ( !handle )
    {
        fprintf( stderr, "failed to load: %s\n", path );
        return -1;
    }
    load_plugin_func_t load_func = (load_plugin_func_t)get_sym( handle, "load_plugin" );
    if ( !load_func )
    {
        fprintf( stderr, "plugin %s missing load_plugin function\n", path );
        return -2;
    }

    load_func( reg );

    return 0;
}

/*==============================================================================================

    (Helper) Load Engine + Editor Modules

==============================================================================================*/

// We'll keep this minimal: plugin_dir is a path where .so/.dll files reside.
// For demo, load core only: plugin filename conventions: "core.dll"/"libcore.so", "editor_plugin.dll" etc.

#ifdef _WIN32
#    define CORE_NAME          "core.dll"
#    define EDITOR_PLUGIN_NAME "editor_plugin.dll"
#else
#    define CORE_NAME          "libcore.so"
#    define EDITOR_PLUGIN_NAME "libeditor_plugin.so"
#endif

/*============================================================================================*/

static char root_path[ 256 ];

const char*
get_executable_path()
{
    if ( root_path[ 0 ] != '\0' )
        return root_path;

    DWORD len = GetModuleFileNameA( NULL, root_path, sizeof( root_path ) );
    if ( len == 0 || len == sizeof( root_path ) )
    {
        fprintf( stderr, "GetModuleFileNameA failed\n" );
        return NULL;
    }

    // strip off after directories util root.
    int escape_count = 0;
    for ( int i = len - 1; i >= 0; --i )
    {
        if ( root_path[ i ] == '\\' || root_path[ i ] == '/' )
        {
            root_path[ i ] = '\0';
            escape_count++;
            if ( escape_count == 2 )
            {
                len = len - ( len - i );
                break;
            }
        }
    }

    return root_path;
}

/*============================================================================================*/

const char*
loader_get_plugin_dir()
{
    return get_executable_path();
}

/*============================================================================================*/

void
loader_load_runtime_modules( struct registry_api_t* reg, const char* plugin_dir )
{
    // ensure registry is the global one (copy pointer) - we use global_registry internally
    // We expect reg to point to &global_registry in the executable; but allow passing same pointer.

    // Call core
    char path[ 512 ];
    snprintf( path, sizeof( path ), "%s\\%s\\%s", plugin_dir ? plugin_dir : ".", "bin", CORE_NAME );
    loader_load_plugin( path, reg );
}

/*============================================================================================*/

void
loader_load_editor_modules( struct registry_api_t* reg, const char* plugin_dir )
{
    loader_load_runtime_modules( reg, plugin_dir );

    // Load editor-specific plugin(s)

    char path[ 512 ];
    snprintf( path, sizeof( path ), "%s\\%s\\%s", plugin_dir ? plugin_dir : ".", "bin", EDITOR_PLUGIN_NAME );
    int r = loader_load_plugin( path, reg );
    if ( r != 0 )
    {
        fprintf( stderr, "failed loading editor plugin: %d\n", r );
    }
    else
    {
        // Editor plugin can register its own APIs and augment runtime behavior.
        // struct base_api* f = (struct base_api*)reg->get( "base_api" );
        // if  ( f && f->log )
        //     f->log( "Editor modules loaded" );
    }
}

/*==============================================================================================

     API Registry

==============================================================================================*/

int
main( int argc, char** argv )
{
    (void)argc;
    (void)argv;

    // struct api_registry global_registry = { 0 };

    // struct registry_api_t* registry = loader_get_registry();
}

/*============================================================================================*/