/*==============================================================================================

    orb.h -- The Glowing Orb Engine

    Root header. Provides compiler/platform/arch detection, fixed-width type aliases,
    DLL import/export macros, assertions, and common utility macros.

    Include this at the top of every translation unit in the engine.

==============================================================================================*/
#ifndef ORB_H
#define ORB_H

/*==============================================================================================
    C11 Requirement
==============================================================================================*/

#if !defined( __STDC_VERSION__ ) || __STDC_VERSION__ < 201112L
    #error "orb requires C11 or later"
#endif

/*==============================================================================================
    Debug / Release
==============================================================================================*/

#if defined( _DEBUG ) || !defined( NDEBUG )
    #define DEBUG   1
    #define RELEASE 0
#else
    #define DEBUG   0
    #define RELEASE 1
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
    DLL Import / Export

    BUILD_STATIC (monolithic): import/export decorators are not needed.
    Dynamic builds: ORB_EXPORT marks symbols leaving a DLL; ORB_IMPORT marks symbols consumed
    from one. Modules use their own per-module export macros (MOD_EXPORT in mod_export.h).
==============================================================================================*/

#if defined( BUILD_STATIC )
    #define ORB_EXPORT
    #define ORB_IMPORT
#elif defined( OS_WINDOWS )
    #define ORB_EXPORT __declspec( dllexport )
    #define ORB_IMPORT __declspec( dllimport )
#else
    #define ORB_EXPORT __attribute__( ( visibility( "default" ) ) )
    #define ORB_IMPORT
#endif

/*==============================================================================================
    Compiler Hints + Utility Macros
==============================================================================================*/

#if defined( COMPILER_MSVC )
    #define ORB_INLINE        __forceinline
    #define ORB_NOINLINE      __declspec( noinline )
    #define ORB_ALIGNAS( n )  __declspec( align( n ) )
    #define ORB_THREAD_LOCAL  __declspec( thread )
    #define ORB_LIKELY( x )   ( x )
    #define ORB_UNLIKELY( x ) ( x )
#else
    #define ORB_INLINE        inline __attribute__( ( always_inline ) )
    #define ORB_NOINLINE      __attribute__( ( noinline ) )
    #define ORB_ALIGNAS( n )  __attribute__( ( aligned( n ) ) )
    #define ORB_THREAD_LOCAL  __thread    // C11: _Thread_local
    #define ORB_LIKELY( x )   __builtin_expect( !!( x ), 1 )
    #define ORB_UNLIKELY( x ) __builtin_expect( !!( x ), 0 )
#endif

#define UNUSED( x ) ( void )x

/*==============================================================================================
    Assertions
==============================================================================================*/

#if defined( COMPILER_MSVC )
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
            if ( UNLIKELY( !( cond ) ) )                            \
            {                                                       \
                ORB_REPORT_ASSERT( #cond, "", __FILE__, __LINE__ ); \
                ORB_TRAP();                                         \
            }                                                       \
        }                                                           \
        while ( 0 )

    #define ORB_ASSERT_MSG( cond, m )                              \
        do {                                                       \
            if ( UNLIKELY( !( cond ) ) )                           \
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
    Reflection annotation macros

    Parsed by rs_gen at build time; compile out to nothing at runtime.
    Defined here so any annotated header needs only orb.h — no rs.h dependency.

        RS_STRUCT()           typedef struct foo_s { ... } foo_t;
        RS_ENUM()             typedef enum   bar_e { ... } bar_t;
        RS_BITSET()           typedef enum   flags_e { ... } flags_t;   // OR-able bitmask
        RS_PROP()             field_type field_name;                     // inside RS_STRUCT body
        RS_VAR()              type g_name;                               // global variable
==============================================================================================*/

// clang-format off
#define RS_STRUCT(...)
#define RS_ENUM(...)
#define RS_BITSET(...)
#define RS_PROP(...)
#define RS_VAR(...)
// clang-format on

/*==============================================================================================
    Version
==============================================================================================*/

#define ORB_VERSION_MAJOR 0
#define ORB_VERSION_MINOR 1
#define ORB_VERSION_PATCH 0

/*============================================================================================*/
#endif