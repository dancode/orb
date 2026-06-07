#ifndef DRAW_API_H
#define DRAW_API_H
/*==============================================================================================

    runtime_service/draw/draw_api.h -- Draw module API struct and gateway macro.

    Include this header in DLL .c files that call draw through the vtable.
    Host executables and sandboxes include draw_host.h instead.

    Function groups (all called through the draw() vtable):
        Frame     : begin / end
        Primitives: rect / box / circle

==============================================================================================*/

#include "runtime_service/draw/draw.h"
#include "runtime_service/rhi/rhi.h"   /* rhi_cmd_list_t for begin() */
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct draw_api_s
{
    /* GPU resource setup / teardown.
       init()     -- call after rhi()->init(); creates vertex/index buffers and pipelines.
       shutdown() -- call before rhi()->shutdown(); destroys all GPU-side resources. */
    bool ( *init     )( void );
    void ( *shutdown )( void );

    /* Set the command list and view-projection matrix for the current frame.
       Call inside an open render pass (between cmd_begin_rendering / cmd_end_rendering). */
    void ( *begin  )( rhi_cmd_list_t cmd, const f32 view_proj[ 16 ] );

    /* Flush all accumulated draw calls to the command list and reset internal state. */
    void ( *end    )( void );

    /* Solid primitives -- centered at world-space position; rgba is linear [0..1]. */
    void ( *rect   )( f32 cx, f32 cy,          f32 w, f32 h,        const f32 rgba[ 4 ] );
    void ( *box    )( f32 cx, f32 cy, f32 cz,  f32 w, f32 h, f32 d, const f32 rgba[ 4 ] );
    void ( *circle )( f32 cx, f32 cy,          f32 r, u32 segs,      const f32 rgba[ 4 ] );

} draw_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( DRAW_STATIC )
MOD_GATEWAY_STATIC( draw_api_t, draw )
#else
MOD_GATEWAY_DYNAMIC( draw_api_t, draw )
#endif

#if defined( BUILD_STATIC ) || defined( DRAW_STATIC )
    #define MOD_USE_DRAW    /* static: gateway returns pointer to global struct directly */
    #define MOD_FETCH_DRAW  true
#else
    #define MOD_USE_DRAW    MOD_DEFINE_API_PTR( draw_api_t, draw )
    #define MOD_FETCH_DRAW  MOD_FETCH_API( draw_api_t, draw )
#endif

// clang-format on
/*============================================================================================*/
#endif    // DRAW_API_H
