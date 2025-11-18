#ifndef ORB_HEADER_H
#define ORB_HEADER_H
/*==============================================================================================

    orb.h -- every compilation unit must include this

==============================================================================================*/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*==============================================================================================

    version and config macros

==============================================================================================*/

#if defined( _WIN32 ) || defined( _WIN64 )
#    define PLATFORM_WINDOWS 1
#    define PLATFORM_LINUX   0
#elif defined( __linux__ )
#    define PLATFORM_WINDOWS 0
#    define PLATFORM_LINUX   1
#else
#    error "Unsupported platform"
#endif

// Debug / release macros
#ifdef _DEBUG
#    define DEBUG 1
#endif

#ifndef DEBUG
#    define RELEASE 1
#endif

// DLL export/import
#if PLATFORM_WINDOWS
#    define DLL_EXPORT __declspec( dllexport )
#    define DLL_IMPORT __declspec( dllimport )
#else
#    define DLL_EXPORT
#    define DLL_IMPORT
#endif

// plugin entry signature used by all modules
#if PLATFORM_WINDOWS
#    define API_EXPORT DLL_EXPORT
#else
#    define API_EXPORT
#endif

/*==============================================================================================

    fixed width basic types

==============================================================================================*/

typedef int8_t     i8;
typedef int16_t    i16;
typedef int32_t    i32;
typedef int64_t    i64;
typedef uint8_t    u8;
typedef uint16_t   u16;
typedef uint32_t   u32;
typedef uint64_t   u64;
typedef float      f32;
typedef double     f64;

typedef const int8_t     ci8;
typedef const int16_t    ci16;
typedef const int32_t    ci32;
typedef const int64_t    ci64;
typedef const uint8_t    cu8;
typedef const uint16_t   cu16;
typedef const uint32_t   cu32;
typedef const uint64_t   cu64;
typedef const float      cf32;
typedef const double     cf64;

#ifndef NULL
#    ifdef __cplusplus
#        define NULL 0
#    else
#        define NULL ( ( void* )0 )
#    endif
#endif

/*==============================================================================================

    macros

==============================================================================================*/

#define UNUSED( x ) ( void )x
#define BIT( x )    ( 1ULL << ( x ) )    // returns value of the set bit.

/* portable alignment + attributes */
#if PLATFORM_WINDOWS
#    define ORB_ALIGN( n ) __declspec( align( n ) )
#    define ORB_NOINLINE   __declspec( noinline )
#    define LIB_PREFIX     ""
#    define LIB_EXT        ".dll"
#    define LIB_DIR        ""
#else
#    define ORB_ALIGN( n ) __attribute__( ( aligned( n ) ) )
#    define ORB_NOINLINE   __attribute__( ( noinline ) )
#    define LIB_PREFIX     "lib"
#    define LIB_EXT        ".so"
#    define LIB_DIR        "../lib/"
#endif

/*==============================================================================================

    utilty

==============================================================================================*/

#define noinline ORB_NOINLINE

/*============================================================================================*/
#endif    // ORB_HEADER_H