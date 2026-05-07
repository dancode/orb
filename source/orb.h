/*==============================================================================================

    orb.h -- The Glowing Orb Engine

    This is the main header file for the Glowing Orb Engine. It includes all the necessary
    declarations and definitions for using the engine's functionalities. This file should
    be included in any source file that needs to interact with the engine.

==============================================================================================*/
#ifndef ORB_H
#define ORB_H
/*==============================================================================================
    Debug / Release Macros
==============================================================================================*/

#if defined( _DEBUG ) || !defined( NDEBUG )
    #define DEBUG   1
    #define RELEASE 0
#else
    #define DEBUG   0
    #define RELEASE 1
#endif

/*==============================================================================================
    C11 Requirement
==============================================================================================*/

#if !defined( __STDC_VERSION__ ) || __STDC_VERSION__ < 201112L
    #error "base requires C11 or later"
#endif

/*==============================================================================================
    Compiler Detection
==============================================================================================*/

#if defined( _MSC_VER )
    #define COMPILER_MSVC  1
    #define COMPILER_CLANG 0
    #define COMPILER_GCC   0
#elif defined( __clang__ )
    #define COMPILER_MSVC  0
    #define COMPILER_CLANG 1
    #define COMPILER_GCC   0
#elif defined( __GNUC__ ) && !defined( __clang__ )
    #define COMPILER_MSVC  0
    #define COMPILER_CLANG 0
    #define COMPILER_GCC   1
#else
    #error "Unsupported compiler"
#endif

/*==============================================================================================
    Platform Detection
==============================================================================================*/

#if defined( _WIN32 )
    #define OS_WINDOWS 1
    #define OS_LINUX   0
    #define OS_MAC     0
#elif defined( __linux__ )
    #define OS_WINDOWS 0
    #define OS_LINUX   1
    #define OS_MAC     0
#elif defined( __APPLE__ )
    #define OS_WINDOWS 0
    #define OS_LINUX   0
    #define OS_MAC     1
#else
    #error "Unsupported platform"
#endif

/*==============================================================================================
    Architecture Detection
==============================================================================================*/

#if defined( _M_X64 ) || defined( _M_AMD64 ) || defined( __x86_64__ )
    #define ARCH_X64   1
    #define ARCH_ARM64 0
#elif defined( _M_ARM64 ) || defined( __aarch64__ )
    #define ARCH_X64   0
    #define ARCH_ARM64 1
#else
    #error "Unsupported architecture"
#endif

/*==============================================================================================
    Fixed-Width Aliases
==============================================================================================*/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef u32       b32;    // boolean: 0 = false, nonzero = true
typedef uintptr_t uptr;
typedef ptrdiff_t iptr;

typedef size_t    usize;
typedef ptrdiff_t isize;

/*==============================================================================================

    static vs dynamic linking

    The build system defines BUILD_STATIC to enable static linked monolithic builds.
    In this mode, all modules are compiled into the exe and linked together, and the module
    API structs are linked in directly (no pointers, no dynamic lookup).

==============================================================================================*/

#ifdef BUILD_STATIC
#endif

/*==============================================================================================
    DLL import / export
==============================================================================================*/

/* for monolithic builds we don't need import/export at all */

#if defined( BUILD_STATIC )
    #define ORB_EXPORT
    #define ORB_IMPORT
#else
    #if defined( OS_WINDOWS )
        #define ORB_EXPORT __declspec( dllexport )
        #define ORB_IMPORT __declspec( dllimport )
    #else
        #define ORB_EXPORT __attribute__( ( visibility( "default" ) ) )
        #define ORB_IMPORT
    #endif
#endif

/*==============================================================================================
    Compiler Hints + Utility Macros
==============================================================================================*/

#if defined( _MSC_VER )
    #define INLINE        __forceinline
    #define NOINLINE      __declspec( noinline )
    #define ALIGNAS( n )  __declspec( align( n ) )
    #define THREAD_LOCAL  __declspec( thread )
    #define LIKELY( x )   ( x )
    #define UNLIKELY( x ) ( x )
#else
    #define INLINE        inline __attribute__( ( always_inline ) )
    #define NOINLINE      __attribute__( ( noinline ) )
    #define ALIGNAS( n )  __attribute__( ( aligned( n ) ) )
    #define THREAD_LOCAL  __thread    // C11: _Thread_local
    #define LIKELY( x )   __builtin_expect( !!( x ), 1 )
    #define UNLIKELY( x ) __builtin_expect( !!( x ), 0 )
#endif

#define UNUSED( x ) ( void )x

/*==============================================================================================
    Assertions (no CRT assert.h dependency)
==============================================================================================*/

#if COMPILER_MSVC
    #define ORB_TRAP() __debugbreak()
#else
    #define ORB_TRAP() __builtin_trap()
#endif

/* Crash unconditionally (even in release) */

#define ORB_PANIC() ORB_TRAP()

/* Override this macro to provide a custom assert handler.

    #define ORB_REPORT_ASSERT(cond, msg, file, line) \
    my_assert_handler(cond, msg, file, line)
    #include "orb.h"
*/

#ifndef ORB_REPORT_ASSERT
    #define ORB_REPORT_ASSERT( cond, msg, file, line ) ( ( void )0 )
#endif

#ifdef RELEASE
    #define ORB_ASSERT( cond )        ( ( void )0 )
    #define ORB_ASSERT_MSG( cond, m ) ( ( void )0 )
#else
    #define ORB_ASSERT( cond )                                      \
        do {                                                        \
            if ( ORB_UNLIKELY( !( cond ) ) )                        \
            {                                                       \
                ORB_REPORT_ASSERT( #cond, "", __FILE__, __LINE__ ); \
                ORB_TRAP();                                         \
            }                                                       \
        }                                                           \
        while ( 0 )

    #define ORB_ASSERT_MSG( cond, m )                              \
        do {                                                       \
            if ( ORB_UNLIKELY( !( cond ) ) )                       \
            {                                                      \
                ORB_REPORT_ASSERT( #cond, m, __FILE__, __LINE__ ); \
                ORB_TRAP();                                        \
            }                                                      \
        }                                                          \
        while ( 0 )
#endif

#define ORB_STATIC_ASSERT( cond, msg ) _Static_assert( cond, msg )

/*==============================================================================================
    Utility Macros
==============================================================================================*/

#define ORB_STRINGIFY( x )  #x
#define ORB_CONCAT_( a, b ) a##b
#define ORB_CONCAT( a, b )  ORB_CONCAT_( a, b )

/*==============================================================================================
    Math Macros
==============================================================================================*/

#define BIT( x )           ( 1ULL << ( x ) )                             // returns value of the set bit.
#define ARRAY_COUNT( a )   ( sizeof( a ) / sizeof( ( a )[ 0 ] ) )        // elements in a static array
#define ALIGN_UP( v, a )   ( ( ( v ) + ( a ) - 1 ) & ~( ( a ) - 1 ) )    // a must be a pow2
#define ALIGN_DOWN( v, a ) ( ( v ) & ~( ( a ) - 1 ) )                    // a must be a pow2
#define MIN( a, b )        ( ( a ) < ( b ) ? ( a ) : ( b ) )
#define MAX( a, b )        ( ( a ) > ( b ) ? ( a ) : ( b ) )
#define CLAMP( v, lo, hi ) MIN( MAX( v, lo ), hi )

#define ORB_KB( n )        ( ( u64 )( n ) * 1024ULL )
#define ORB_MB( n )        ( ( u64 )( n ) * 1024ULL * 1024ULL )
#define ORB_GB( n )        ( ( u64 )( n ) * 1024ULL * 1024ULL * 1024ULL )

/*==============================================================================================
    Version
==============================================================================================*/

#define ORB_VERSION_MAJOR 0
#define ORB_VERSION_MINOR 1
#define ORB_VERSION_PATCH 0

/*============================================================================================*/
#endif
