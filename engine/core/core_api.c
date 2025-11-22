/*==============================================================================================

    Core API

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "orb.h"
#include "core.h"

/*==============================================================================================

    API Functions (simplified version for now)

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
/* required to debug natvis data from dll's (we must import reference to global data ) */

extern string_pool_t      g_cvar_string_pool;    // cvar data for strings
extern user_string_pool_t g_user_string_pool;    // cvar data for user strings
extern intern_state_t     g_intern;              // sid string data

static core_debug_api_t   g_core_debug_api_internal = {
      .string_pool      = &g_cvar_string_pool,
      .user_string_pool = &g_user_string_pool,
      .intern_arena     = &g_intern.arena,
};

/*============================================================================================*/

static core_api_t g_core_api_internal = {

    .debug_api = &g_core_debug_api_internal,

    .log       = core_log,
    .alloc     = core_alloc,
    .free      = core_free,

    // .cvar_find = cvar_find,
    // .cvar_register = cvar_register,
    // .cvar_get_int  = cvar_get_int,
    // .cvar_set_int  = cvar_set_int,
    // .cvar_set_string = cvar_set_string,
    // .cvar_get_string = cvar_get_string,

};

/*============================================================================================*/
/* exported api */

core_api_t*
core_get_api( void )
{
    return &g_core_api_internal;
}

core_debug_api_t*
core_debug_get_api( void )
{
    return &g_core_debug_api_internal;
}

/*============================================================================================*/

core_api_t*       g_core_api;
core_debug_api_t* g_debug_api;

void
core_api_init( void )
{
    g_core_api  = core_get_api();
    g_debug_api = core_debug_get_api();
}

void
core_api_exit( void )
{
    g_core_api  = NULL;
    g_debug_api = NULL;
}

/*============================================================================================*/