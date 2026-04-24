/*==============================================================================================

    platform.h

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

typedef f64 ( *platform_tick_reset_fn )( void );
typedef f64 ( *platform_tick_seconds_fn )( void );
typedef i64 ( *platform_tick_milliseconds_fn )( void );
typedef i64 ( *platform_tick_microseconds_fn )( void );
typedef i64 ( *platform_tick_nanoseconds_fn )( void );
typedef void ( *platform_tick_sleep_fn )( i32 milliseconds );

// The struct passed to every module
typedef struct platform_api_s
{
    platform_tick_reset_fn        tick_reset;
    platform_tick_seconds_fn      tick_seconds;
    platform_tick_milliseconds_fn tick_milliseconds;
    platform_tick_microseconds_fn tick_microseconds;
    platform_tick_nanoseconds_fn  tick_nanoseconds;
    platform_tick_sleep_fn        tick_sleep;

} platform_api_t;

platform_api_t* platform_get_api( void );

/*==============================================================================================

System : Tick

==============================================================================================*/

void sys_tick_init( void );
void sys_tick_exit( void );

f64  sys_tick_reset( void );
f64  sys_tick_seconds( void );
i64  sys_tick_milliseconds( void );
i64  sys_tick_microseconds( void );
i64  sys_tick_nanoseconds( void );
void sys_tick_sleep( i32 milliseconds );

/*==============================================================================================

    System : Debug Print

==============================================================================================*/

// void sys_print( const char* msg );
// void sys_printf( const char* fmt, ... );

/*==============================================================================================

    System : Console

==============================================================================================*/

#ifdef CONFIG_CONSOLE

// void sys_console_open();
// void sys_console_close();
// i32  sys_console_read_line( i8* buffer, i32 buffer_size );

#endif


/*============================================================================================*/