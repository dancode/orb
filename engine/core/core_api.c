/*==============================================================================================

    Core API

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "core.h"

/*==============================================================================================

    API Functions

==============================================================================================*/

static void
core_log( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vprintf( fmt, args );
    printf( "\n" );
    va_end( args );
}

static void*
core_alloc( size_t size )
{
    return malloc( size );
}

static void
core_free( void* ptr )
{
    free( ptr );
}

/*============================================================================================*/

extern core_debug_api_t g_core_debug_api;
static core_api_t       g_core_api = {
          .log       = core_log,
          .alloc     = core_alloc,
          .free      = core_free,

          .cvar_find = cvar_find,

    // .cvar_register = cvar_register,
    // .cvar_get_int  = cvar_get_int,
    // .cvar_set_int  = cvar_set_int,
    // .cvar_set_string = cvar_set_string,
    // .cvar_get_string = cvar_get_string,

          .debug_api = &g_core_debug_api,
};

core_api_t*
core_get_api( void )
{
    return &g_core_api;
}

/*============================================================================================*/