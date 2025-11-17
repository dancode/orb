/*==============================================================================================

    core.h

==============================================================================================*/
#pragma once
#include "orb.h"
#include "cvar/string_pool.h"
#include "cvar/cvar.h"
#include "reflect/str_intern.h"

// clang-format off

/*==============================================================================================

    core

==============================================================================================*/

void core_init( void );

/*==============================================================================================

    bits

==============================================================================================*/

u32 u32_clz             ( u32 v );  // count leadeing zeros
u32 u32_highest_bit     ( u32 v );  // bit offset (log2) of highest set bit
u32 u32_round_up_pow2   ( u32 v );  // 

/*==============================================================================================

    module system

==============================================================================================*/

struct module_t;
struct core_api_t;

// Engine expects modules to provide these functions
typedef void ( *module_init_fn )( struct core_api_t* api );
typedef void ( *module_tick_fn )( void );
typedef void ( *module_exit_fn )( void );

#define MAX_MODULE_NAME 16

struct module_t* module_load( const char* name, const char* path );
void             module_unload( struct module_t* mod );
void             module_reload( struct module_t* mod );
void             module_call_tick( struct module_t* m );

/*==============================================================================================

    Debug API - For module and natvis visualization

==============================================================================================*/

typedef struct core_debug_api_t
{
    string_pool_t*  string_pool;
    string_pool_t*  user_string_pool;
    string_arena_t* intern_arena;

} core_debug_api_t;

extern core_debug_api_t* core_debug_get_api( void );

/*==============================================================================================

    core api

==============================================================================================*/

// Logging
typedef void ( *core_log_fn )( const char* fmt, ... );

// Memory
typedef void* ( *core_alloc_fn )( size_t size );
typedef void ( *core_free_fn )( void* ptr );

// Cvars (just int for now)

typedef void ( *cvar_register_fn )( const char* name, cvar_type_t type, void* storage );

typedef int32_t ( *cvar_get_fn )( const char* name );
typedef void ( *cvar_set_fn )( const char* name, int32_t value );

typedef int ( *cvar_get_int_fn )( const char* name );
typedef void ( *cvar_set_int_fn )( const char* name, int32_t value );

typedef void ( *cvar_set_string_fn )( const char* name, const char* value );
typedef const char* ( *cvar_get_string_fn )( const char* name );

// new
typedef cvar_t* ( *cvar_find_fn )( const char* name );

// The struct passed to every module
typedef struct core_api_t
{
    core_log_fn   log;
    core_alloc_fn alloc;
    core_free_fn  free;

    cvar_find_fn cvar_find;

    // cvar_register_fn   cvar_register;
    // cvar_get_int_fn    cvar_get_int;
    // cvar_set_int_fn    cvar_set_int;
    // cvar_set_string_fn cvar_set_string;
    // cvar_get_string_fn cvar_get_string;

    core_debug_api_t* debug_api;

} core_api_t;

void            core_api_init       ( void );
void            core_api_exit       ( void );
core_api_t*     core_get_api        ( void );

/*============================================================================================*/

                                    /* Test core cvar system */
void            test_core_cvar      ( int argc, char** argv );




/*============================================================================================*/
