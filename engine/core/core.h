/*==============================================================================================

    core.h

==============================================================================================*/
#pragma once
#include "orb.h"
// #include "cvar/string_pool.h"
#include "cvar/cvar.h"
#include "sid/sid.h"

// clang-format off

/*==============================================================================================

    core.c

==============================================================================================*/

void core_init( void );
void core_exit( void );

/*==============================================================================================

    module_system.c

==============================================================================================*/

struct module_t;
struct core_api_t;

typedef void ( *module_init_fn )( struct core_api_t* api );
typedef void ( *module_tick_fn )( float dt );
typedef void ( *module_exit_fn )( void );

#define MAX_MODULE_NAME 16

void module_set_base_path(const char* path);
const char* module_get_base_path(void);

struct module_t* module_load( const char* name, const char* path );
void             module_unload( struct module_t* mod );
void             module_reload( struct module_t* mod );
void             module_call_tick( struct module_t* m );

/*==============================================================================================

    core_api.c

==============================================================================================*/

typedef struct string_pool_s string_pool_t;
typedef struct string_arena_s string_arena_t;
typedef struct user_string_pool_s user_string_pool_t;

typedef struct core_debug_api_t
{
    string_pool_t*  string_pool;
    user_string_pool_t*  user_string_pool;
    string_arena_t* intern_arena;

} core_debug_api_t;

extern core_debug_api_t* core_debug_get_api( void );

/*============================================================================================*/

// Logging
typedef void ( *core_log_fn )( const char* fmt, ... );

// Memory
typedef void* ( *core_alloc_fn )( size_t size );
typedef void ( *core_free_fn )( void* ptr );

// Cvars (just int for now)

// typedef void ( *cvar_register_fn )( const char* name, cvar_type_t type, void* storage );
// 
// typedef int32_t ( *cvar_get_fn )( const char* name );
// typedef void ( *cvar_set_fn )( const char* name, int32_t value );
// 
// typedef int ( *cvar_get_int_fn )( const char* name );
// typedef void ( *cvar_set_int_fn )( const char* name, int32_t value );
// 
// typedef void ( *cvar_set_string_fn )( const char* name, const char* value );
// typedef const char* ( *cvar_get_string_fn )( const char* name );
// 
// typedef cvar_t* ( *cvar_find_fn )( const char* name );

/*============================================================================================*/

// The struct passed to every module
typedef struct core_api_t
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

void            core_api_init       ( void );
void            core_api_exit       ( void );
core_api_t*     core_get_api        ( void );

/*============================================================================================*/
