/*==============================================================================================

    engine/core/core_api_log.h — Per-call-site logging macros.

    Included at the bottom of core_api.h (after the gateway macro is defined).
    Do not include directly.

    Per-file setup (one line, before including core_api.h):
        #define LOG_CH "render"

    Call sites:
        LOG_TRACE( "dt=%.4f", dt );
        LOG_INFO( "loaded %u textures", count );
        LOG_WARN( "texture '%s' not found", name );
        LOG_ERROR( "out of memory" );

    Compile-time stripping:
        LOG_COMPILE_MIN defaults to ORB_LOG_TRACE in debug, ORB_LOG_INFO in release.
        Override per-target in CMake: target_compile_definitions(foo PRIVATE LOG_COMPILE_MIN=2)
        Calls below the minimum compile to ((void)0) — no string literal, no branch, no call.

==============================================================================================*/
#pragma once

#ifndef CORE_API_H
    #error "log.h must not be included directly; include core_api.h"
#endif

// clang-format off

/*==============================================================================================
    Compile-time minimum level
==============================================================================================*/

#ifndef LOG_COMPILE_MIN
    #if RELEASE
        #define LOG_COMPILE_MIN ORB_LOG_INFO
    #else
        #define LOG_COMPILE_MIN ORB_LOG_TRACE
    #endif
#endif

/*==============================================================================================
    Per-file channel fallback
==============================================================================================*/

#ifndef LOG_CH
    #define LOG_CH "?"
#endif

/*==============================================================================================
    Internal dispatch  (do not call directly)
==============================================================================================*/

#define _LOG( lvl, fmt, ... )  core()->log_write( lvl, LOG_CH, fmt, ##__VA_ARGS__ )

/*==============================================================================================
    Public macros
==============================================================================================*/

#if ORB_LOG_TRACE >= LOG_COMPILE_MIN
    #define LOG_TRACE( fmt, ... )  _LOG( LOG_LEVEL_TRACE, fmt, ##__VA_ARGS__ )
#else
    #define LOG_TRACE( fmt, ... )  ( ( void )0 )
#endif

#if ORB_LOG_DEBUG >= LOG_COMPILE_MIN
    #define LOG_DEBUG( fmt, ... )  _LOG( LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__ )
#else
    #define LOG_DEBUG( fmt, ... )  ( ( void )0 )
#endif

#if ORB_LOG_INFO >= LOG_COMPILE_MIN
    #define LOG_INFO(  fmt, ... )  _LOG( LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__ )
#else
    #define LOG_INFO(  fmt, ... )  ( ( void )0 )
#endif

#if ORB_LOG_WARN >= LOG_COMPILE_MIN
    #define LOG_WARN(  fmt, ... )  _LOG( LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__ )
#else
    #define LOG_WARN(  fmt, ... )  ( ( void )0 )
#endif

#if ORB_LOG_ERROR >= LOG_COMPILE_MIN
    #define LOG_ERROR( fmt, ... )  _LOG( LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__ )
#else
    #define LOG_ERROR( fmt, ... )  ( ( void )0 )
#endif

// clang-format on
/*============================================================================================*/
