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
    Vertex layout  (36 bytes)

    Attribute LOCATIONS are fixed by contract so the solid and textured pipelines share one
    vertex buffer and stride:
        location 0  pos    float3  offset 0    (all primitives)
        location 1  color  float4  offset 12   (solid: color; textured: tint)
        location 2  uv     float2  offset 28   (textured; 0,0 on solid primitives)
    uv was appended at location 2 so the pre-existing solid shaders -- which read only
    location 0 and location 1 -- are unaffected by the vertex growing.
==============================================================================================*/

typedef struct
{
    f32 x, y, z;     /* world-space position       -> location 0 */
    f32 r, g, b, a;  /* linear RGBA (tint if textured) -> location 1 */
    f32 u, v;        /* texcoord (0,0 if untextured)   -> location 2 */

} draw_vertex_t;

/*==============================================================================================
    Push constants  (both fit within the shared 128-byte RHI pipeline layout)
==============================================================================================*/

typedef struct
{
    f32 mvp[ 16 ]; /* column-major view-projection matrix */

} draw_push_t;                 /* 64 bytes; DRAW_MAT_SOLID / DRAW_MAT_SOLID_DEPTH */

typedef struct
{
    f32 mvp[ 16 ]; /* column-major view-projection matrix */
    u32 tex_idx;   /* bindless texture slot (rhi()->register_texture) */
    u32 samp_idx;  /* bindless sampler slot (draw()->sampler_linear / _point) */

} draw_push_tex_t;             /* 72 bytes; DRAW_MAT_TEXTURED */

/*==============================================================================================
    Material IDs
==============================================================================================*/

typedef enum
{
    DRAW_MAT_SOLID       = 0,   /* 2D/overlay: no depth test, no cull -- draws always on top */
    DRAW_MAT_SOLID_DEPTH = 1,   /* 3D: depth test + write; requires a bound depth attachment */
    DRAW_MAT_TEXTURED    = 2,   /* 2D: alpha-blended; samples a bindless texture, vertex color tints */
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

/*==============================================================================================
    Built-in 5x7 bitmap debug font (draw_font.c)

    Cell metrics in source pixels (before the per-call scale multiplier).  A glyph is 5 wide x 7
    tall; ADVANCE / LINE add one pixel of spacing between columns / rows.  At scale s a glyph
    occupies COLS*s x ROWS*s px and the pen steps ADVANCE*s / LINE*s.
==============================================================================================*/

#define DRAW_FONT_COLS      5    /* glyph width  in source pixels */
#define DRAW_FONT_ROWS      7    /* glyph height in source pixels */
#define DRAW_FONT_ADVANCE   6    /* x pen step per glyph (COLS + 1 spacing) */
#define DRAW_FONT_LINE      8    /* y pen step per line  (ROWS + 1 spacing) */
#define DRAW_FONT_GLYPHS    95   /* printable ASCII 0x20..0x7E */

/*============================================================================================*/
#endif    // DRAW_H
