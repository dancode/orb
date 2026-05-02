/*==============================================================================================

    core_api.c

    Core module — always statically compiled into the exe.

    This file is NEVER built as a DLL.

    ALWAYS-STATIC PROVIDER PATTERN
    ────────────────────────────────────────────────────────────────────────────────────────
    Identical to any other static module (audio.c, base.c) with two differences:

        1.  MODULE_DEFINE_EXPORTS is replaced with MODULE_NEVER_DYNAMIC.
            Core has no undecorated "get_module_api" / "get_api" DLL exports.

        2.  main.c registers it via module_static_load before loading any DLLs, so the
            module system can hand its func_api pointer to DLL modules that request it
            through get_api("core") in their init() callbacks.

    The g_core_api_struct is globally visible (non-static) by design. Exe-linked
    translation units that define CORE_LINK_STATIC resolve core_api() directly to its
    address at link time.  DLL-linked translation units cannot see it — they go through
    the cached pointer path instead (g_core_api_ptr, set by MODULE_GET_API in module init).

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "orb.h"
#include "core.h"
#include "core_api.h"
#include "module/module_api.h"

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
    API struct — non-static, globally visible.

    Naming: g_core_api_struct matches the symbol declared extern by MODULE_GATEWAY_STRUCT_PATH
    in core_api.h.  Exe-linked TUs that define CORE_LINK_STATIC resolve core_api() here.
    DLL-linked TUs cannot link this symbol — they use g_core_api_ptr instead.
==============================================================================================*/

const core_api_t g_core_api_struct = {

    .debug_api = &g_core_debug_api_struct,

    .log       = core_log,
    .warn      = core_warn,
    .error     = core_error,
    .alloc     = core_alloc,
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
core_init( void* raw_state, get_api_fn get_api )
{
    s = ( core_state_t* )raw_state;

    /* State is zeroed on first call; no guard needed here since 0 is valid for all fields. */
    core_log( "core init" );
    return true;    
}

static void
core_exit( void* raw_state )
{
    core_state_t* c = ( core_state_t* )raw_state;
    core_log( "core exit  logs=%u  allocs=%u  bytes=%llu", c->log_count, c->alloc_count,
              ( unsigned long long )c->total_alloc_bytes );
}

static void
core_tick( void* raw_state, float dt )
{
    UNUSED( raw_state );
    UNUSED( dt );
}

static void
core_on_reload( void* raw_state, get_api_fn get_api )
{
    /* Core is always-static — on_reload is never called in practice.
       Provided here so the pattern is complete and consistent with other modules. */

    UNUSED( get_api );
    s = ( core_state_t* )raw_state;
}


/*==============================================================================================
    Module descriptor
==============================================================================================*/

module_api_t*
core_get_module_api( void )
{
    static module_api_t api = {
        .version    = 1,
        .state_size = sizeof( core_state_t ),
        .deps       = { NULL },
        .dep_count  = 0,

        .func_api   = ( void* )&g_core_api_struct,

        .init       = core_init,
        .tick       = core_tick,
        .exit       = core_exit,
        .on_reload  = core_on_reload,        
    };
    return &api;
}

void*
core_get_api( void )
{
    return ( void* )&g_core_api_struct;
}

/*============================================================================================*/