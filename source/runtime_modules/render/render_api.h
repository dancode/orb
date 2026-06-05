#ifndef RENDER_API_H
#define RENDER_API_H
/*==============================================================================================

    runtime_modules/render/render_api.h -- Render module API struct and gateway macro.

    The render module sits above the RHI and provides the high-level frame surface.
    It is hot-reloadable: all state is preserved across DLL swaps.

    Multi-context design:
        All frame functions take an explicit ctx_id so the host can drive multiple
        windows (game viewport + editor windows) independently each frame:

            for each active context:
                render()->begin_frame( ctx_id )
                render()->draw_scene( ctx_id, dt )
                render()->draw_editor( ctx_id, dt )    // no-op when not an editor window
                render()->end_frame( ctx_id )

        The host registers contexts with context_register before the first frame and
        unregisters them when the window is closed.

==============================================================================================*/

#include "runtime_modules/render/render.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct render_api_s
{
    /* ---- Context management ---- */
    /* Binds an RHI context to a render slot; call after rhi()->context_create. */
    void ( *context_register   )( i32 ctx_id );
    void ( *context_unregister )( i32 ctx_id );

    /* ---- Per-context frame ---- */
    /* begin_frame returns false if the swapchain is not ready (resize, minimized, etc.);
       draw_scene and end_frame should be skipped when false is returned. */
    bool ( *begin_frame  )( i32 ctx_id );
    void ( *draw_scene   )( i32 ctx_id, f32 dt );
    void ( *draw_editor  )( i32 ctx_id, f32 dt );    /* editor overlays + ImGui (stub) */
    void ( *end_frame    )( i32 ctx_id );

    /* ---- Per-context settings ---- */
    void ( *set_clear_color )( i32 ctx_id, f32 r, f32 g, f32 b, f32 a );

} render_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
    MOD_GATEWAY_STATIC( render_api_t, render )
#else
    MOD_GATEWAY_DYNAMIC( render_api_t, render )
#endif

#if defined( BUILD_STATIC ) || defined( RENDER_STATIC )
    #define MOD_USE_RENDER    /* static build */
    #define MOD_FETCH_RENDER  true
#else
    #define MOD_USE_RENDER    MOD_DEFINE_API_PTR( render_api_t, render )
    #define MOD_FETCH_RENDER  MOD_FETCH_API( render_api_t, render )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RENDER_API_H
