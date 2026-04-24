#ifndef CORE_API_H
#define CORE_API_H
/*==============================================================================================

    core_api.c

==============================================================================================*/
/* data pointers required for natvis debugging via global data between modules */

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
// The core API is provided by the engine to all modules.

// Logging
typedef void ( *core_log_fn )( const char* fmt, ... );

// Memory
typedef void* ( *core_alloc_fn )( size_t size );
typedef void ( *core_free_fn )( void* ptr );
typedef void* ( *core_realloc_fn )( void* ptr, size_t size );

// The struct passed to every module
typedef struct core_api_s
{
    /* debug api */
    core_debug_api_t* debug_api;    // for natvis and debugging

    /* simple api */
    core_log_fn   log;
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

/*============================================================================================*/
/* public core api functions */

void        core_api_init( void );
void        core_api_exit( void );
core_api_t* core_get_api( void );

/*============================================================================================*/
#endif    // CORE_API_H
