#ifndef RHI_API_H
#define RHI_API_H
/*==============================================================================================

    runtime_service/rhi/rhi_api.h — RHI module API struct and gateway macro.

==============================================================================================*/

#include "runtime_service/rhi/rhi.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct rhi_api_s
{
    /* ---- Global lifecycle (once per process) ---- */
    bool ( *init     )( void );   /* instance + device; no window needed */
    void ( *shutdown )( void );   /* must be called after all contexts are destroyed */

    /* ---- Per-context lifecycle (one per window) ---- */
    i32          ( *context_create  )( i32 win_id, void* native_window, i32 w, i32 h );
    void         ( *context_destroy )( i32 ctx_id );
    bool         ( *context_resize  )( i32 ctx_id, i32 w, i32 h );

    /* ---- Frame ---- */
    rhi_command_list_t ( *frame_begin )( i32 ctx_id );
    void               ( *frame_end   )( i32 ctx_id );

    /* ---- Commands (v0) ---- */
    void ( *cmd_clear_color )( rhi_command_list_t cmd, f32 r, f32 g, f32 b, f32 a );

} rhi_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( RHI_STATIC )
MOD_GATEWAY_STATIC( rhi_api_t, rhi )
#else
MOD_GATEWAY_DYNAMIC( rhi_api_t, rhi )
#endif

#if defined( BUILD_STATIC ) || defined( RHI_STATIC )
    #define MOD_USE_RHI    /* static build */
    #define MOD_FETCH_RHI  true
#else
    #define MOD_USE_RHI    MOD_DEFINE_API_PTR( rhi_api_t, rhi )
    #define MOD_FETCH_RHI  MOD_FETCH_API( rhi_api_t, rhi )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RHI_API_H
