/*==============================================================================================

    core_api.c : the "core" module's API implementation.

    The core module provides basic services like logging and memory allocation.  
    It is the foundational Tier 1 module that all other modules depend on.

    The API struct is defined in core_api.h and implemented here.  The struct contains function
    pointers to the core services, and is exposed to other modules via the module system.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "orb.h"
#include "engine/mod/mod_api.h"
#include "engine/mod/mod_export.h"

#include "engine/core/core.h"
#include "engine/core/core_api.h"

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    uint64_t total_alloc_bytes;
    uint32_t alloc_count;
    uint32_t log_count;

} core_state_t;

static core_state_t* s = NULL;

/*==============================================================================================
    API Start / Shutdown
==============================================================================================*/

void
core_init( void )
{
    sid_init();
    cvar_system_init();
}

void
core_exit( void )
{
    cvar_system_exit();
    sid_exit();
}

/*==============================================================================================
    API implementation
==============================================================================================*/

static void
core_log( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    printf( "[log] " );
    vprintf( fmt, args );
    printf( "\n" );
    va_end( args );
    if ( s )
        ++s->log_count;
}

static void
core_warn( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    printf( "[WRN] " );
    vprintf( fmt, ap );
    printf( "\n" );
    va_end( ap );
}

static void
core_error( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    printf( "[ERR] " );
    vprintf( fmt, ap );
    printf( "\n" );
    va_end( ap );
}

static void*
core_alloc( size_t size )
{
    if ( s )
    {
        s->total_alloc_bytes += size;
        ++s->alloc_count;
    }
    return malloc( size );
}

static void*
core_realloc( void* ptr, size_t size )
{
    if ( s )
    {
        s->total_alloc_bytes += size;
        ++s->alloc_count;
    }
    return realloc( ptr, size );
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

static core_debug_api_t   g_core_debug_api_struct = {
      .string_pool      = &g_cvar_string_pool,
      .user_string_pool = &g_user_string_pool,
      .intern_arena     = &g_intern.arena,
};

/*==============================================================================================
    API struct
==============================================================================================*/

const core_api_t g_core_api_struct = {

    .debug_api = &g_core_debug_api_struct,

    .log       = core_log,
    .warn      = core_warn,
    .error     = core_error,
    .alloc     = core_alloc,
    .realloc   = core_realloc,
    .free      = core_free,

    // .cvar_find = cvar_find,
    // .cvar_register = cvar_register,
    // .cvar_get_int  = cvar_get_int,
    // .cvar_set_int  = cvar_set_int,
    // .cvar_set_string = cvar_set_string,
    // .cvar_get_string = cvar_get_string,

};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
core_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    core_init();
    core_log( "core initialized" );
    return true;
}

static void
core_mod_exit( void* raw_state )
{
    core_state_t* c = ( core_state_t* )raw_state;
    core_exit();
    core_log( "core exit  logs=%u  allocs=%u  bytes=%llu", c->log_count, c->alloc_count,
              ( unsigned long long )c->total_alloc_bytes );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_api_t*
core_get_mod_api( void )
{
    static mod_api_t api = {
        .version    = 1,
        .state_size = sizeof( core_state_t ),
        .deps       = { NULL },
        .dep_count  = 0,
        .func_api   = ( void* )&g_core_api_struct,
        .init       = core_mod_init,
        .tick       = NULL,
        .exit       = core_mod_exit,
        .reload     = NULL,
    };
    return &api;
}

/*============================================================================================*/