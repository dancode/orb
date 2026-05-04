#ifndef CORE_API_H
#define CORE_API_H
/*==============================================================================================

    core_api.c

==============================================================================================*/

#include "engine/module/module_api.h"

/*============================================================================================*/
/* These data pointers are required for natvis debugging within modules */

typedef struct string_pool_s      string_pool_t;
typedef struct string_arena_s     string_arena_t;
typedef struct user_string_pool_s user_string_pool_t;

typedef struct core_debug_api_s
{
    string_arena_t*     intern_arena;        // sid interned strings (debug preview)
    string_pool_t*      string_pool;         // cvar strings (debug preview)
    user_string_pool_t* user_string_pool;    // cvar user strings (debug preview)

} core_debug_api_t;

/*============================================================================================*/
/* forward declarations for core API */

// Logging
typedef void ( *core_log_fn )( const char* fmt, ... );

// Memory
typedef void* ( *core_alloc_fn )( size_t size );
typedef void ( *core_free_fn )( void* ptr );
typedef void* ( *core_realloc_fn )( void* ptr, size_t size );

/*============================================================================================*/
/* core API struct — this is what modules get when they ask for "core" */

typedef struct core_api_s
{
    /* debug api */
    core_debug_api_t* debug_api;    // for natvis and debugging

    /* simple api */
    core_log_fn   log;
    core_log_fn   warn;
    core_log_fn   error;
    core_alloc_fn alloc;
    core_free_fn  free;

    /* cvar system */
    // cvar_find_fn cvar_find;
    // cvar_register_fn   cvar_register;
    // cvar_get_int_fn    cvar_get_int;
    // cvar_set_int_fn    cvar_set_int;
    // cvar_set_string_fn cvar_set_string;
    // cvar_get_string_fn cvar_get_string;

} core_api_t;

#if defined( ORB_BUILD_STATIC ) || defined( CORE_LINK_STATIC )

/* Exe-linked: the struct is part of this binary.  Direct address, LTO-eligible. */
MODULE_GATEWAY_STRUCT_PATH( core_api_t, core )

#else

/* DLL-linked: the struct is in the exe which we cannot link against.
   g_core_api_ptr is defined by MODULE_DEFINE_API_PTR in the consuming .c file
   and populated once in init() via MODULE_FETCH_API. */
MODULE_GATEWAY_PTR_PATH( core_api_t, core )

#endif

/*============================================================================================*/
#endif    // CORE_API_H

