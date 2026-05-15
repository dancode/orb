/*==============================================================================================

    runtime/services/rhi/rhi.h — Render Hardware Interface, public types and gateway.

    Single public header following the core.h / app.h pattern. Exposes opaque
    handle types, the API struct, and the gateway accessor. Every implementation
    function is static inside rhi.c's unity build; the only way to reach the RHI
    is through rhi_api()->...

    Backend
    -------
    Vulkan, contained entirely within source/runtime/services/rhi/vulkan/. The
    public types here are GPU-API agnostic — adding a hypothetical D3D12 or
    Metal backend in the future would not change this header.

    Scope (v0)
    ----------
    Just enough to clear the swap chain to a color and present:

        rhi_init( void* native_window_handle )
        rhi_shutdown()
        rhi_resize( width, height )
        rhi_command_list_t cmd = rhi_frame_begin()
        rhi_cmd_clear_color( cmd, r, g, b, a )
        rhi_frame_end()

    Buffers, textures, pipelines, draw calls, etc. arrive in subsequent phases
    on top of this skeleton.

==============================================================================================*/
#ifndef RHI_H
#define RHI_H

#include "orb.h"
#include "engine/mod/mod.h"

/*==============================================================================================
    Opaque handles  (defined privately inside the implementation)
==============================================================================================*/

typedef struct rhi_command_list_s* rhi_command_list_t;

/* Future handles, declared here as TODO for the file's reader:
       typedef struct rhi_buffer_s*       rhi_buffer_t;
       typedef struct rhi_texture_s*      rhi_texture_t;
       typedef struct rhi_pipeline_s*     rhi_pipeline_t;
       typedef struct rhi_sampler_s*      rhi_sampler_t;
       typedef struct rhi_shader_s*       rhi_shader_t;
*/

/*==============================================================================================
    Value types
==============================================================================================*/

typedef struct rhi_color_s
{
    f32 r, g, b, a;

} rhi_color_t;

/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct rhi_api_s
{
    /* ---- Lifecycle ----
       init() needs the native window handle (HWND on Windows) to create a
       presentation surface. Call AFTER app_api()->window_create() succeeds. */

    bool ( *init     )( void* native_window_handle );
    void ( *shutdown )( void );
    bool ( *resize   )( i32 width, i32 height );

    /* ---- Frame ----
       frame_begin acquires the next swap chain image and returns a command
       list to record into. Returns NULL if the swap chain is in a state that
       cannot present this frame (resize pending, device lost). The host
       should skip drawing this frame and try again next tick.

       frame_end submits the recorded commands and presents. Must be called
       exactly once per successful frame_begin. */

    rhi_command_list_t ( *frame_begin )( void );
    void               ( *frame_end   )( void );

    /* ---- Commands (v0) ---- */

    void ( *cmd_clear_color )( rhi_command_list_t cmd, f32 r, f32 g, f32 b, f32 a );

} rhi_api_t;

#if defined( BUILD_STATIC ) || defined( RHI_STATIC )
MOD_GATEWAY_STATIC( rhi_api_t, rhi )
#else
MOD_GATEWAY_DYNAMIC( rhi_api_t, rhi )
#endif

/*============================================================================================*/
#endif    // RHI_H
