#ifndef SYS_API
#define SYS_API
/*============================================================================================*/
#include "base/orb.h"
#include "engine/module/module_api.h"

// typedef struct sys_api_t sys_api_t;
typedef struct module_api_s       module_api_t;

typedef f64 ( *platform_tick_reset_fn )( void );
typedef f64 ( *platform_tick_seconds_fn )( void );
typedef i64 ( *platform_tick_milliseconds_fn )( void );
typedef i64 ( *platform_tick_microseconds_fn )( void );
typedef i64 ( *platform_tick_nanoseconds_fn )( void );
typedef void ( *platform_tick_sleep_fn )( i32 milliseconds );

// The struct passed to every module
typedef struct sys_api_s
{
    platform_tick_reset_fn        tick_reset;
    platform_tick_seconds_fn      tick_seconds;
    platform_tick_milliseconds_fn tick_milliseconds;
    platform_tick_microseconds_fn tick_microseconds;
    platform_tick_nanoseconds_fn  tick_nanoseconds;
    platform_tick_sleep_fn        tick_sleep;

} sys_api_t;

// The MODULE_GATEWAY_* macros resolve to either a struct or pointer symbol depending on build mode.
 
#if defined( BUILD_STATIC ) || defined( SYS_LINK_STATIC )
MODULE_GATEWAY_STRUCT_PATH( sys_api_t, sys )
#else
MODULE_GATEWAY_PTR_PATH( sys_api_t, sys )
#endif

/*============================================================================================*/
#endif    // SYS_API

