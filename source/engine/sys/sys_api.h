#ifndef SYS_API_H
#define SYS_API_H
/*==============================================================================================

    sys_api.h — sys module API struct and gateway macro.

    Consumers call sys()->tick_seconds() etc.
    sys is always statically linked, so MOD_GATEWAY_STATIC is used unconditionally.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct sys_api_s
{
    f64  ( *tick_seconds )( void );      /* seconds since engine init — monotonic, never resets  */
    i64  ( *tick_milliseconds )( void );
    i64  ( *tick_microseconds )( void );
    i64  ( *tick_nanoseconds )( void );
    void ( *sleep_milliseconds )( i32 milliseconds );

} sys_api_t;

#if defined( BUILD_STATIC ) || defined( SYS_STATIC )
MOD_GATEWAY_STATIC( sys_api_t, sys )
#else
MOD_GATEWAY_DYNAMIC( sys_api_t, sys )
#endif

/*============================================================================================*/
#endif    // SYS_API_H
