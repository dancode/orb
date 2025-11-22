#ifndef MODULE_H
#define MODULE_H
// clang-format off
/*==============================================================================================

    module.h : shared header (engine + module authors include this)

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/
/* engine provides these to allocate from engine memory. */

typedef void*   ( *memory_alloc_fn )    ( size_t size );
typedef void    ( *memory_free_fn )     ( void* ptr );
typedef void*   ( *memory_realloc_fn )  ( void* ptr, size_t size );

typedef struct memory_api_s
{
    memory_alloc_fn       alloc;
    memory_free_fn        free;
    memory_realloc_fn     realloc;

} memory_api_t;

/*============================================================================================*/
/* engine provides these to manage modules */

typedef void*   ( *api_get_fn )      ( const char *name, uint32_t min_version );
typedef void    ( *api_reg_fn )      ( const char *name, uint32_t version, void *ptr );

typedef struct module_api_s
{
    api_get_fn          get;
    api_reg_fn          reg;

} module_api_t;


/*============================================================================================*/
/* engine provides these to acquire time */

typedef double  ( *time_get_fn )        ();

typedef struct time_api_s
{
    time_get_fn         get;

} time_api_t;

/*============================================================================================*/
/* engine provides these to output text abd kig */

typedef void    ( *log_fn )             ( const char* fmt, ... );

typedef struct log_api_s
{
    log_fn              fmt;

} log_api_t;

/*============================================================================================*/
/* engine api is exported to every module */

typedef struct engine_api_s
{
    module_api_t        module;
    memory_api_t        mem;    
    time_api_t          time;
    log_api_t           log;

} engine_api_t;

/*============================================================================================*/

/** a module lifecycle implementation*/
typedef bool    ( *mod_init_fn ) ( const engine_api_t* engine_api, const char* module_path );
typedef void    ( *mod_tick_fn ) ( float dt );
typedef void    ( *mod_exit_fn ) ( void );

/* a modules persistent-state implementation */
typedef size_t  ( *mod_get_persist_size_fn ) ( void );
typedef void    ( *mod_save_state_fn ) ( void *dst, size_t dst_size );
typedef void    ( *mod_load_state_fn ) ( const void *src, size_t src_size );

/*============================================================================================*/
/* the single symbol engine will lookup to acquire interface to a module */

typedef struct module_new_s
{
    uint32_t                    magic;              // 'MODL' for sanity
    uint32_t                    api_version;        // interface version (1)
    uint32_t                    module_version;     // 
    const char*                 name;               // human readable
    
    /* module interface api */

    mod_init_fn                 init;               // modules init function
    mod_tick_fn                 tick;               // modules tick function
    mod_exit_fn                 exit;               // modules exit function

    mod_get_persist_size_fn     get_persist_size;   // get size of persistent state
    mod_save_state_fn           save_state;         // save persistent state
    mod_load_state_fn           load_state;         // load persistent state

    // optional: module can choose to export an API pointer
    // that the engine will register for other modules to use.

    void*                       exported_api;           // 
    const char*                 exported_api_name;      // 
    uint32_t                    exported_api_version;

} module_new_t;

/*============================================================================================*/
#endif    // MODULE_H
