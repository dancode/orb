#ifndef CORE_API_H
#define CORE_API_H
/*==============================================================================================

    engine/core/core_api.h - core module API struct and gateway macro.

    Consumers use LOG_INFO/LOG_WARN/LOG_ERROR macros from log.h (included below).
    core is always statically linked, but the conditional below preserves the
    pattern in case a future build mode pulls core out into a DLL.

==============================================================================================*/

#include "engine/core/core.h"
#include "engine/core/cvar/cvar.h"
#include "engine/core/cmd/cmd.h"
#include "engine/core/console/console.h"
#include "engine/core/fs/fs.h"
#include "engine/mod/mod_import.h"

// clang-format off
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

    bool        ( *assert_report )      ( const char* cond, const char* msg, const char* func, const char* file, int line );

    /* logging */

    void        ( *log_write )           ( log_level_t level, const char* channel, const char* fmt, ... );
    void        ( *log_set_min_level )   ( log_level_t level );
    void        ( *log_add_sink )        ( log_sink_fn fn, void* userdata );
    void        ( *log_remove_sink )     ( log_sink_fn fn );

    /* ring buffer access for editor/tools */

    const log_entry_t*  ( *log_ring_entries )   ( void );
    u32                 ( *log_ring_capacity )  ( void );
    u32                 ( *log_ring_seq )       ( void );

    /* allocator */

    void*       ( *alloc )              ( size_t size );
    void*       ( *realloc )            ( void* ptr, size_t size );
    void        ( *free )               ( void* ptr );

    /* sid */

    sid_t       ( *sid_intern )         ( const char* str, int32_t len );
    sid_t       ( *sid_intern_cstr )    ( const char* str );
    sid_t       ( *sid_find_cstr )      ( const char* str );
    const char* ( *sid_cstr )           ( sid_t sid );
    uint8_t     ( *sid_length )         ( sid_t sid );
    bool        ( *sid_is_canonical )   ( sid_t sid, const char* str, size_t len );
    uint32_t    ( *sid_get_hash )       ( sid_t sid );
    void        ( *sid_print_stats )    ( void* fp );
    void        ( *sid_reset_stats )    ( void );

    /* cvar system */

    cvar_t*     ( *cvar_register_b )    ( const char* name, const char* desc, bool value, u32 type );
    cvar_t*     ( *cvar_register_i )    ( const char* name, const char* desc, i32 val, i32 min, i32 max, u32 type );
    cvar_t*     ( *cvar_register_f )    ( const char* name, const char* desc, f32 val, f32 min, f32 max, u32 type );
    cvar_t*     ( *cvar_register_s )    ( const char* name, const char* desc, const char** values, u32 count, u32 def_index, u32 type );
    cvar_t*     ( *cvar_register_w )    ( const char* name, const char* desc, const char* reset, u32 size, u32 type );
    cvar_t*     ( *cvar_register_r )    ( const char* name, const char* desc, const char* value, u32 type );

    cvar_t*     ( *cvar_find )          ( const char* name );
    cvar_t*     ( *cvar_get_by_index )  ( u32 index );
    u32         ( *cvar_get_count )     ( void );

    const char* ( *cvar_get_name )      ( const cvar_t* cv );
    const char* ( *cvar_get_desc )      ( const cvar_t* cv );
    bool        ( *cvar_get_bool )      ( const cvar_t* cv );
    i32         ( *cvar_get_int )       ( const cvar_t* cv );
    f32         ( *cvar_get_float )     ( const cvar_t* cv );
    const char* ( *cvar_get_string )    ( const cvar_t* cv );

    bool        ( *cvar_set_value )     ( const char* name, const char* value );
    const char* ( *cvar_get_value )     ( const char* name );
    void        ( *cvar_reset )         ( cvar_t* cv );

    /* owner module id is stamped automatically; re-register in reload() */
    uint8_t     ( *cvar_callback_register )   ( cvar_t* cv, cvar_callback_fn fn );
    void        ( *cvar_callback_unregister ) ( cvar_t* cv );

    /* config writer hook: writeconfig appends service sections after cvars + binds */
    bool        ( *config_writer_add )    ( cvar_config_writer_fn fn );
    void        ( *config_writer_remove ) ( cvar_config_writer_fn fn );

    /* command backend (registry + immediate execute + deferred buffer) */

    bool        ( *cmd_register )       ( const char* name, cmd_fn fn, const char* desc );
    void        ( *cmd_unregister )     ( const char* name );
    u32         ( *cmd_count )          ( void );
    const char* ( *cmd_name )           ( u32 index );
    const char* ( *cmd_desc )           ( u32 index );

    bool        ( *cmd_execute_string ) ( const char* text );
    void        ( *cmd_queue )          ( const char* text );
    void        ( *cmd_queue_front )    ( const char* text );
    void        ( *cmd_queue_args )     ( int argc, char** argv );
    void        ( *cmd_pump )           ( void );

    /* developer console (view over the command backend; state lives in core) */

    void        ( *con_print )          ( const char* text );
    void        ( *con_printf )         ( const char* fmt, ... );
    void        ( *con_clear )          ( void );
    bool        ( *con_exec )           ( const char* line );
    void        ( *con_submit )         ( const char* line );

    u32         ( *con_line_count )     ( void );
    const char* ( *con_line_get )       ( u32 index );

    u32         ( *con_history_count )  ( void );
    const char* ( *con_history_get )    ( u32 index );
    u32         ( *con_complete )       ( const char* prefix, const char** out_names, u32 max );

    /* virtual filesystem (core/fs) -- mount real dirs, read bytes by virtual path */

    bool        ( *fs_mount )           ( const char* vprefix, const char* real_path, int priority );
    void        ( *fs_unmount )         ( const char* vprefix );
    fs_blob_t   ( *fs_read )            ( const char* vpath );
    void        ( *fs_free )            ( fs_blob_t* blob );
    bool        ( *fs_exists )          ( const char* vpath );
    bool        ( *fs_stat )            ( const char* vpath, fs_stat_t* out );
    int         ( *fs_glob )            ( const char* vpat, fs_glob_fn cb, void* userdata );
    u32         ( *fs_file_count )      ( void );

} core_api_t;

/*============================================================================================*/

/*==============================================================================================
    MOD_USE_CORE   — File-scope: defines the core API pointer and the natvis g_debug_api anchor.
    MOD_FETCH_CORE — In init()/reload(): populates both in one call. Requires get_api in scope.

    Static builds: g_debug_api is defined in engine_core; no DLL pointer needed.
    Dynamic builds: both are NULL until MOD_FETCH_CORE runs in init()/reload().

    Usage:
        MOD_USE_CORE;                              // file scope
        if ( !MOD_FETCH_CORE ) return false;       // in init() / reload()
==============================================================================================*/

#if defined( BUILD_STATIC ) || defined( CORE_STATIC )
    MOD_GATEWAY_STATIC( core_api_t, core )
    #define MOD_USE_CORE    /* g_debug_api defined in engine_core; static gateway needs no ptr */
    #define MOD_FETCH_CORE  true
#else
    MOD_GATEWAY_DYNAMIC( core_api_t, core )
    #define MOD_USE_CORE \
        const core_api_t* g_core_api_ptr = NULL; \
        core_debug_api_t* g_debug_api    = NULL
    #define MOD_FETCH_CORE \
        ( ( g_core_api_ptr = ( const core_api_t* )get_api( "core" ) ) != NULL && \
          ( g_debug_api = g_core_api_ptr->debug_api, true ) )
#endif

/*============================================================================================*/
/* ASSERT handler macros - ORB_ASSERT calls cores assert_report function pointer */

#include "engine/core/debug/assert.h"

/*============================================================================================*/
/* LOG convenience macros - LOG_ERROR( fmt, ... ), etc.. */

#include "engine/core/core_api_log.h"      

/*============================================================================================*/
/* SID convenience macros — require core() to be live at call time */

#define SID( str )       core()->sid_intern( ( str ), ( int32_t )strlen( str ) )
#define SID_CSTR( str )  core()->sid_intern_cstr( str )

// clang-format on
/*============================================================================================*/
#endif    // CORE_API_H
