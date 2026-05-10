#ifndef CORE_API_H
#define CORE_API_H
/*==============================================================================================

    core_api.c

==============================================================================================*/

#include "engine/mod/mod_api.h"

/*============================================================================================*/
/* These data pointers are required for natvis debugging within DLL modules */

typedef struct string_pool_s      string_pool_t;
typedef struct string_arena_s     string_arena_t;
typedef struct user_string_pool_s user_string_pool_t;

typedef struct core_debug_api_s
{
    string_arena_t*     intern_arena;        // sid interned strings (debug preview)
    string_pool_t*      string_pool;         // cvar strings (debug preview)
    user_string_pool_t* user_string_pool;    // cvar user strings (debug preview)

} core_debug_api_t;

/*==============================================================================================
    Public API struct
==============================================================================================*/

typedef struct core_api_s
{
    /* debug api */
    core_debug_api_t* debug_api;    // for natvis and debugging

    /* logging */
    void ( *log )( const char* fmt, ... );
    void ( *warn )( const char* fmt, ... );
    void ( *error )( const char* fmt, ... );

    /* allocator */
    void* ( *alloc )( size_t size );
    void* ( *realloc )( void* ptr, size_t size );
    void ( *free )( void* ptr );

    /* cvar system */
    // cvar_find_fn cvar_find;
    // cvar_register_fn   cvar_register;
    // cvar_get_int_fn    cvar_get_int;
    // cvar_set_int_fn    cvar_set_int;
    // cvar_set_string_fn cvar_set_string;
    // cvar_get_string_fn cvar_get_string;

} core_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( CORE_STATIC )
MOD_GATEWAY_STATIC( core_api_t, core )
#else
MOD_GATEWAY_DYNAMIC( core_api_t, core )
#endif

/*============================================================================================*/
#endif    // CORE_API_H
