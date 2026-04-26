#ifndef PLATFORM_SYS_API
#define PLATFORM_SYS_API
/*============================================================================================*/
#include "orb.h"

// typedef struct platform_sys_api_t platform_sys_api_t;
typedef struct module_api_s       module_api_t;

typedef f64 ( *platform_tick_reset_fn )( void );
typedef f64 ( *platform_tick_seconds_fn )( void );
typedef i64 ( *platform_tick_milliseconds_fn )( void );
typedef i64 ( *platform_tick_microseconds_fn )( void );
typedef i64 ( *platform_tick_nanoseconds_fn )( void );
typedef void ( *platform_tick_sleep_fn )( i32 milliseconds );

// The struct passed to every module
typedef struct platform_sys_api_s
{
    platform_tick_reset_fn        tick_reset;
    platform_tick_seconds_fn      tick_seconds;
    platform_tick_milliseconds_fn tick_milliseconds;
    platform_tick_microseconds_fn tick_microseconds;
    platform_tick_nanoseconds_fn  tick_nanoseconds;
    platform_tick_sleep_fn        tick_sleep;

} platform_sys_api_t;

typedef struct module_api_s       module_api_t;

module_api_t*                     platform_sys_get_module_api( void ); /* the lifecycle descriptor */
platform_sys_api_t*               platform_sys_get_api( void );        /* the typed API struct    */

/*============================================================================================*/
#endif    // PLATFORM_SYS_API

