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
                gui()->render( vp, render()->frame_cmd( ctx_id ) )   // optional composite
                render()->end_frame( ctx_id )

        The host registers contexts with context_register before the first frame and
        unregisters them when the window is closed.

    Pass ownership:
        begin_frame acquires the frame command list; draw_scene opens the scene pass
        (clear), draws, and CLOSES it; end_frame presents.  Between draw_scene and
        end_frame the frame is open but no pass is -- the composite point.  Anything
        that composites there (gui()->render opens its own LOAD pass) draws over the
        finished scene on the same swapchain image.

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
    /* Binds an RHI context to a render slot; call after rhi()->context_open. */
    void ( *context_register   )( i32 ctx_id );
    void ( *context_unregister )( i32 ctx_id );

    /* ---- Per-context frame ---- */
    /* begin_frame returns false if the swapchain is not ready (resize, minimized, etc.);
       draw_scene and end_frame should be skipped when false is returned. */
    bool ( *begin_frame  )( i32 ctx_id );
    void ( *draw_scene   )( i32 ctx_id, f32 dt );
    void ( *end_frame    )( i32 ctx_id );

    /* ---- Scene submission (0.1 minimal) ---- */
    /* Queue a solid rect on a context for this frame, pixel space, centered at (cx,cy),
       origin top-left.  Submit between frames (e.g. host on_update); draw_scene( ctx_id )
       replays that context's list through the draw service and clears it.  Replaced later
       by a real scene / draw-list system. */
    void ( *submit_rect )( i32 ctx_id, f32 cx, f32 cy, f32 w, f32 h, const f32 rgba[ 4 ] );

    /* ---- Per-context settings ---- */
    void ( *set_clear_color )( i32 ctx_id, f32 r, f32 g, f32 b, f32 a );

    /* ---- Offscreen targets ---- */
    /* A target is a double-buffered offscreen color texture the scene can render into
       instead of a swapchain: target ids live in [RENDER_TARGET_ID_BASE..+MAX) and are
       valid anywhere a render_ctx i32 is accepted (submit_rect, run_view_t.render_ctx) --
       the editor's scene viewport hands one to the project in place of the window ctx.
       draw_scene drains every pending target into its texture (own pass, barriers to
       SHADER_READ) before the swapchain pass, so hosts never touch the command list.

       target_texture returns the bindless index of the CURRENT buffer for sampling
       (gui()->image_texture); 0 = none.  target_flip advances the write buffer -- call it
       once per frame that will draw, BEFORE the gui emit bakes the index, so in-flight
       frames keep sampling the previous buffer untouched.  target_resize waits for the
       device to idle and recreates -- callers debounce (settle) live drags themselves. */
    i32  ( *target_create  )( i32 w, i32 h );                /* -> target id, or -1        */
    void ( *target_destroy )( i32 target_id );
    bool ( *target_resize  )( i32 target_id, i32 w, i32 h );
    u32  ( *target_texture )( i32 target_id );               /* bindless idx of cur, 0=none */
    void ( *target_flip    )( i32 target_id );
    void ( *target_size    )( i32 target_id, i32* w, i32* h );

    /* ---- Frame command list access ---- */
    /* The live command list between begin_frame and end_frame; RHI_CMD_INVALID otherwise.
       The host composite point: after draw_scene the scene pass is closed, so overlays
       that open their own pass (gui()->render) record here before end_frame presents. */
    rhi_cmd_t ( *frame_cmd )( i32 ctx_id );

} render_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( RENDER_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    MOD_GATEWAY_STATIC( render_api_t, render )
    #define MOD_USE_RENDER    /* static build */
    #define MOD_FETCH_RENDER  true
#else
    MOD_GATEWAY_DYNAMIC( render_api_t, render )
    #define MOD_USE_RENDER    MOD_DEFINE_API_PTR( render_api_t, render )
    #define MOD_FETCH_RENDER  MOD_FETCH_API( render_api_t, render )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RENDER_API_H
