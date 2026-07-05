#ifndef DRAW_H
#define DRAW_H
/*==============================================================================================

    runtime_service/draw/draw.h -- Draw library, public types.

    Pure types, constants, and enums.  No function declarations, no vtable, no includes
    beyond orb.h.  Callers that need to call draw functions include draw_api.h (DLL
    modules) or draw_host.h (host exes and sandboxes).

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    Vertex layout  (28 bytes; float3 pos @ location 0, float4 color @ location 1)
==============================================================================================*/

typedef struct
{
    f32 x, y, z;     /* world-space position */
    f32 r, g, b, a;  /* linear RGBA */

} draw_vertex_t;

/*==============================================================================================
    Push constants  (64 bytes; fits within the shared 128-byte RHI pipeline layout)
==============================================================================================*/

typedef struct
{
    f32 mvp[ 16 ]; /* column-major view-projection matrix */

} draw_push_t;

/*==============================================================================================
    Material IDs
==============================================================================================*/

typedef enum
{
    DRAW_MAT_SOLID       = 0,   /* 2D/overlay: no depth test, no cull -- draws always on top */
    DRAW_MAT_SOLID_DEPTH = 1,   /* 3D: depth test + write; requires a bound depth attachment */
    DRAW_MAT_COUNT,

} draw_mat_id_t;

/*==============================================================================================
    Depth format used by DRAW_MAT_SOLID_DEPTH.  Callers that use begin_depth() must attach a
    depth image of this exact format to cmd_begin_rendering (pipeline/attachment must match).
==============================================================================================*/

#define DRAW_DEPTH_FORMAT   RHI_FORMAT_D32_FLOAT

/*==============================================================================================
    Batch limits
==============================================================================================*/

#define DRAW_BATCH_MAX_VERTS   ( 8 * 1024 )
#define DRAW_BATCH_MAX_IDX     ( 32 * 1024 )
#define DRAW_MAX_CALLS         1024
#define DRAW_CIRCLE_MAX_SEGS   64

/*============================================================================================*/
#endif    // DRAW_H
