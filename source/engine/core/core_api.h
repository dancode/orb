#ifndef CORE_API_H
#define CORE_API_H
/*==============================================================================================

    engine/core/core_api.h - core module API struct and gateway macro.

    Consumers call core()->log_info(...) etc.
    core is always statically linked, but the conditional below preserves the
    pattern in case a future build mode pulls core out into a DLL.

==============================================================================================*/

#ifndef CORE_DECLARED
    #error "core_api.h should not be included directly; include core.h instead."
#endif

#include "engine/mod/mod.h"

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
    API Struct
==============================================================================================*/

typedef struct core_api_s
{
    /* debug api */
    core_debug_api_t* debug_api;    // for natvis and debugging

    /* assertions */
    bool ( *assert_report )( const char* cond, const char* msg, const char* file, int line );

    /* logging */
    void ( *log )( const char* fmt, ... );
    void ( *log_info )( const char* fmt, ... );
    void ( *log_warn )( const char* fmt, ... );
    void ( *log_error )( const char* fmt, ... );
    void ( *log_set_min_level )( log_level_t level );

    /* allocator */
    void* ( *alloc )( size_t size );
    void* ( *realloc )( void* ptr, size_t size );
    void ( *free )( void* ptr );

    /* sid */
    sid_t       ( *sid_intern )       ( const char* str, int32_t len );
    sid_t       ( *sid_intern_cstr )  ( const char* str );
    sid_t       ( *sid_find_cstr )    ( const char* str );
    const char* ( *sid_cstr )         ( sid_t sid );
    uint8_t     ( *sid_length )       ( sid_t sid );
    bool        ( *sid_is_canonical ) ( sid_t sid, const char* str, size_t len );
    uint32_t    ( *sid_get_hash )     ( sid_t sid );
    void        ( *sid_print_stats )  ( void* fp );
    void        ( *sid_reset_stats )  ( void );

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
