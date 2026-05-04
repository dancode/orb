#ifndef ORB_HEADER_H
#define ORB_HEADER_H
/*==============================================================================================

    orb.h -- every compilation unit must include this

==============================================================================================*/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*==============================================================================================
    fixed-width aliases
==============================================================================================*/

typedef int8_t         i8;
typedef int16_t        i16;
typedef int32_t        i32;
typedef int64_t        i64;
typedef uint8_t        u8;
typedef uint16_t       u16;
typedef uint32_t       u32;
typedef uint64_t       u64;
typedef float          f32;
typedef double         f64;
typedef uintptr_t      uptr;
typedef ptrdiff_t      iptr;

typedef const int8_t   ci8;
typedef const int16_t  ci16;
typedef const int32_t  ci32;
typedef const int64_t  ci64;
typedef const uint8_t  cu8;
typedef const uint16_t cu16;
typedef const uint32_t cu32;
typedef const uint64_t cu64;
typedef const float    cf32;
typedef const double   cf64;

/*==============================================================================================
    debug / release macros
==============================================================================================*/

#ifdef _DEBUG
#    define DEBUG 1
#endif

#ifndef DEBUG
#    define RELEASE 1
#endif

/*==============================================================================================
    platform detection
==============================================================================================*/

#if defined( _WIN32 ) || defined( _WIN64 )
#    define PLATFORM_WINDOWS 1
#    define ORB_OS_WINDOWS   1
#elif defined( __APPLE__ )
#    error "Apple platforms are not supported yet"
#elif defined( __linux__ )
#    define PLATFORM_LINUX 1
#    define ORB_OS_LINUX   1
#    error "Linux platforms are not supported yet"
#else
#    error "Unsupported platform"
#endif

/*==============================================================================================
    compiler detection
==============================================================================================*/

#if defined( _MSC_VER )
#    define ORB_MSVC 1
#elif
#    error "Unsupported compiler"
#endif

/*==============================================================================================

    static vs dynamic linking

    The build system defines MODULE_LINK_STATIC to enable static linked monolithic builds.
    In this mode, all modules are compiled into the exe and linked together, and the module
    API structs are linked in directly (no pointers, no dynamic lookup).

==============================================================================================*/

#ifdef MODULE_LINK_STATIC
#    define ORB_BUILD_STATIC
#endif

/*==============================================================================================
    DLL import / export
==============================================================================================*/

#if PLATFORM_WINDOWS
#    define ORB_EXPORT __declspec( dllexport )
#    define ORB_IMPORT __declspec( dllimport )
#else
#    define ORB_EXPORT
#    define ORB_IMPORT
#endif

/* for monolithic builds we don't need import/export at all */

#if defined( ORB_BUILD_STATIC )
#    define ORB_EXPORT
#    define ORB_IMPORT
#else
#    if defined( ORB_OS_WINDOWS )
#        define ORB_EXPORT __declspec( dllexport )
#        define ORB_IMPORT __declspec( dllimport )
#    else
#        define ORB_EXPORT __attribute__( ( visibility( "default" ) ) )
#        define ORB_IMPORT
#    endif
#endif

/* portable alignment + attributes */

#if PLATFORM_WINDOWS
#    define LIB_PREFIX ""
#    define LIB_EXT    ".dll"
#    define LIB_DIR    ""
#else
#    define LIB_PREFIX "lib"
#    define LIB_EXT    ".so"
#    define LIB_DIR    "../lib/"
#endif

/*==============================================================================================
    compiler helpers
==============================================================================================*/

#if defined( _MSC_VER )
#    define ORB_INLINE        __forceinline
#    define ORB_NOINLINE      __declspec( noinline )
#    define ORB_ALIGNAS( n )  __declspec( align( n ) )
#    define ORB_THREAD_LOCAL  __declspec( thread )
#    define ORB_LIKELY( x )   ( x )
#    define ORB_UNLIKELY( x ) ( x )
#else
#    define ORB_INLINE        __attribute__( ( always_inline ) ) inline
#    define ORB_NOINLINE      __attribute__( ( noinline ) )
#    define ORB_ALIGNAS( n )  __attribute__( ( aligned( n ) ) )
#    define ORB_THREAD_LOCAL  __thread
#    define ORB_LIKELY( x )   __builtin_expect( !!( x ), 1 )
#    define ORB_UNLIKELY( x ) __builtin_expect( !!( x ), 0 )
#endif

#define UNUSED( x ) ( void )x
#define noinline    ORB_NOINLINE

/*==============================================================================================
    utilty macros
==============================================================================================*/

#define ORB_STRINGIFY( x )  #x
#define ORB_CONCAT_( a, b ) a##b
#define ORB_CONCAT( a, b )  ORB_CONCAT_( a, b )

/*==============================================================================================
    math macros
==============================================================================================*/

#define BIT( x )               ( 1ULL << ( x ) )                         // returns value of the set bit.
#define ORB_ARRAY_COUNT( a )   ( sizeof( a ) / sizeof( ( a )[ 0 ] ) )    // elements in a static array
#define ORB_UNUSED( x )        ( ( void )( x ) )                         // used to silence warnings
#define ORB_KB( n )            ( ( u64 )( n ) * 1024ULL )
#define ORB_MB( n )            ( ( u64 )( n ) * 1024ULL * 1024ULL )
#define ORB_GB( n )            ( ( u64 )( n ) * 1024ULL * 1024ULL * 1024ULL )
#define ORB_ALIGN_UP( v, a )   ( ( ( v ) + ( a ) - 1 ) & ~( ( a ) - 1 ) )
#define ORB_ALIGN_DOWN( v, a ) ( ( v ) & ~( ( a ) - 1 ) )
#define ORB_MIN( a, b )        ( ( a ) < ( b ) ? ( a ) : ( b ) )
#define ORB_MAX( a, b )        ( ( a ) > ( b ) ? ( a ) : ( b ) )
#define ORB_CLAMP( v, lo, hi ) ORB_MIN( ORB_MAX( v, lo ), hi )

/*==============================================================================================
    assert
==============================================================================================*/
// Replaced by the debug module with a richer implementation; this is the
// bootstrap assert used before debug is initialised.

// #if defined( ORB_DEBUG )
// #    include <assert.h>
// #    define ORB_ASSERT( cond )       assert( cond )
// #    define ORB_ASSERT_MSG( c, msg ) assert( ( c ) && ( msg ) )
// #else
// #    define ORB_ASSERT( cond )       ( ( void )0 )
// #    define ORB_ASSERT_MSG( c, msg ) ( ( void )0 )
// #endif

/*==============================================================================================
    zero / init helpers
==============================================================================================*/

// #include <string.h>    // memset, memcpy
// #define ORB_ZERO( ptr )      memset( ( ptr ), 0, sizeof( *( ptr ) ) )
// #define ORB_ZERO_N( p, n )   memset( ( p ), 0, ( n ) )
// #define ORB_COPY( dst, src ) memcpy( ( dst ), ( src ), sizeof( *( dst ) ) )

/*==============================================================================================
    version
==============================================================================================*/

#define ORB_VERSION_MAJOR 0
#define ORB_VERSION_MINOR 1
#define ORB_VERSION_PATCH 0

/*==============================================================================================
    warnings
==============================================================================================*/

/*============================================================================================*/
#endif    // ORB_HEADER_H