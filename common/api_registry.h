#pragma once
/*==============================================================================================

    Version & Config Macros

==============================================================================================*/

// Plugin/engine API version (for ABI checks)
#define ORB_API_VERSION 1

// Platform detection
#if defined( _WIN32 ) || defined( _WIN64 )
#    define ORB_PLATFORM_WINDOWS 1
#    define ORB_PLATFORM_LINUX   0
#elif defined( __linux__ )
#    define ORB_PLATFORM_WINDOWS 0
#    define ORB_PLATFORM_LINUX   1
#else
#    error "Unsupported platform"
#endif

// Debug / release macros
#ifdef _DEBUG
#    define ORB_DEBUG 1
#endif

// DLL export/import
#if ORB_PLATFORM_WINDOWS
#    define ORB_DLL_EXPORT __declspec( dllexport )
#    define ORB_DLL_IMPORT __declspec( dllimport )
#else
#    define ORB_DLL_EXPORT
#    define ORB_DLL_IMPORT
#endif

// plugin entry signature used by all modules
#ifdef _WIN32
#    define ORB_API __declspec( dllexport )
#else
#    define ORB_API
#endif

/*==============================================================================================

     Fixed-Width Basic Types

==============================================================================================*/

#include <stdint.h>
#include <stddef.h>

typedef int8_t   i8;
typedef uint8_t  u8;
typedef int16_t  i16;
typedef uint16_t u16;
typedef int32_t  i32;
typedef uint32_t u32;
typedef int64_t  i64;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

/*==============================================================================================

     API Registry

==============================================================================================*/

// If callers allocate names, they must also free them or register a cleanup.
// Generic API registry used by loader and plugins.

struct registry_api_t
{
    // register an api pointer under a name (string)
    void ( *add )( const char* name, void* api );

    // get an api pointer previously registered, or NULL
    void* ( *get )( const char* name );
};

typedef void ( *load_plugin_func_t )( struct registry_api_t* );

/*==============================================================================================

     API Entry Point

==============================================================================================*/

// struct registry_t;
// typedef void ( *orb_load_plugin_ft )( struct registry_api_t* api, struct registry_t* reg );


/*============================================================================================*/