/*==============================================================================================

    runtime_service/draw/draw_geo.c -- Procedural geometry generation.

    Pure CPU, stateless; no RHI dependency.  Each function writes into caller-supplied
    arrays and returns the counts via nv (vertex count) and ni (index count).

    Indices are always 0-relative to the first vertex of the current call; the batch
    layer adds the base-vertex offset at draw time via vertex_offset in the draw call.

==============================================================================================*/

#include <math.h>

#define DRAW_PI 3.14159265358979323846f

/*==============================================================================================
    geo_rect  --  axis-aligned quad in the XY plane (z = 0)

        (-hw,-hh)-----(hw,-hh)
            |               |
        (-hw, hh)-----(hw, hh)

    4 vertices, 6 indices (2 triangles, CCW).
==============================================================================================*/

static void
geo_rect( draw_vertex_t* verts, u16* indices, u32* nv, u32* ni,
          f32 cx, f32 cy, f32 hw, f32 hh, const f32 rgba[ 4 ] )
{
    verts[ 0 ] = ( draw_vertex_t ){ cx - hw, cy - hh, 0.0f, rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ] };
    verts[ 1 ] = ( draw_vertex_t ){ cx + hw, cy - hh, 0.0f, rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ] };
    verts[ 2 ] = ( draw_vertex_t ){ cx + hw, cy + hh, 0.0f, rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ] };
    verts[ 3 ] = ( draw_vertex_t ){ cx - hw, cy + hh, 0.0f, rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ] };

    indices[ 0 ] = 0; indices[ 1 ] = 1; indices[ 2 ] = 2;
    indices[ 3 ] = 0; indices[ 4 ] = 2; indices[ 5 ] = 3;

    *nv = 4;
    *ni = 6;
}

/*==============================================================================================
    geo_image  --  textured quad in the XY plane (z = 0)

    Same corners and winding as geo_rect, plus a source uv sub-rect: (u0,v0) maps to the
    top-left corner, (u1,v1) to the bottom-right.  Pass 0,0,1,1 to sample the whole texture,
    or a sub-rect to pull one sprite out of an atlas.  tint modulates the sampled texel.

    4 vertices, 6 indices (2 triangles, CCW) -- identical topology to geo_rect.
==============================================================================================*/

static void
geo_image( draw_vertex_t* verts, u16* indices, u32* nv, u32* ni,
           f32 cx, f32 cy, f32 hw, f32 hh,
           f32 u0, f32 v0, f32 u1, f32 v1, const f32 tint[ 4 ] )
{
    verts[ 0 ] = ( draw_vertex_t ){ cx - hw, cy - hh, 0.0f, tint[0], tint[1], tint[2], tint[3], u0, v0 };
    verts[ 1 ] = ( draw_vertex_t ){ cx + hw, cy - hh, 0.0f, tint[0], tint[1], tint[2], tint[3], u1, v0 };
    verts[ 2 ] = ( draw_vertex_t ){ cx + hw, cy + hh, 0.0f, tint[0], tint[1], tint[2], tint[3], u1, v1 };
    verts[ 3 ] = ( draw_vertex_t ){ cx - hw, cy + hh, 0.0f, tint[0], tint[1], tint[2], tint[3], u0, v1 };

    indices[ 0 ] = 0; indices[ 1 ] = 1; indices[ 2 ] = 2;
    indices[ 3 ] = 0; indices[ 4 ] = 2; indices[ 5 ] = 3;

    *nv = 4;
    *ni = 6;
}

/*==============================================================================================
    geo_box  --  axis-aligned box centered at (cx, cy, cz)

    8 unique corner vertices shared across all 6 faces (no per-face normals; colour only).
    36 indices (6 faces x 2 triangles x 3 indices), CCW from outside.
==============================================================================================*/

static void
geo_box( draw_vertex_t* verts, u16* indices, u32* nv, u32* ni,
         f32 cx, f32 cy, f32 cz, f32 hw, f32 hh, f32 hd, const f32 rgba[ 4 ] )
{
    /* 8 corners:  index bits = (z<<2 | y<<1 | x), so x = bit0, y = bit1, z = bit2:
       0: (-x,-y,-z)  1: (+x,-y,-z)  2: (-x,+y,-z)  3: (+x,+y,-z)
       4: (-x,-y,+z)  5: (+x,-y,+z)  6: (-x,+y,+z)  7: (+x,+y,+z) */
    f32 x0 = cx - hw, x1 = cx + hw;
    f32 y0 = cy - hh, y1 = cy + hh;
    f32 z0 = cz - hd, z1 = cz + hd;

    for ( u32 i = 0; i < 8; ++i )
    {
        verts[ i ] = ( draw_vertex_t ){
            ( i & 1 ) ? x1 : x0,
            ( i & 2 ) ? y1 : y0,
            ( i & 4 ) ? z1 : z0,
            rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ],
        };
    }

    /* Each face = 4 of the corners above, split into 2 triangles that share ONE diagonal
       (so they tile the quad -- mismatched diagonals produce a criss-cross bowtie).  Wound
       CCW as seen from outside, so backface culling would also be correct if enabled. */
    /* clang-format off */
    static const u16 s_faces[ 36 ] = {
        4, 5, 7,  4, 7, 6,   /* +Z front  (corners 4,5,6,7) */
        1, 0, 2,  1, 2, 3,   /* -Z back   (corners 0,1,2,3) */
        5, 1, 3,  5, 3, 7,   /* +X right  (corners 1,3,5,7) */
        0, 4, 6,  0, 6, 2,   /* -X left   (corners 0,2,4,6) */
        6, 7, 3,  6, 3, 2,   /* +Y top    (corners 2,3,6,7) */
        0, 1, 5,  0, 5, 4,   /* -Y bottom (corners 0,1,4,5) */
    };
    /* clang-format on */

    for ( u32 i = 0; i < 36; ++i )
        indices[ i ] = s_faces[ i ];

    *nv = 8;
    *ni = 36;
}

/*==============================================================================================
    geo_circle  --  filled disc in the XY plane (z = 0), triangle fan from centre

    segs is clamped to [3, DRAW_CIRCLE_MAX_SEGS].
    Vertices: 1 centre + segs perimeter = segs + 1 total.
    Indices: segs * 3.
==============================================================================================*/

static void
geo_circle( draw_vertex_t* verts, u16* indices, u32* nv, u32* ni,
            f32 cx, f32 cy, f32 r, u32 segs, const f32 rgba[ 4 ] )
{
    if ( segs < 3 )               segs = 3;
    if ( segs > DRAW_CIRCLE_MAX_SEGS ) segs = DRAW_CIRCLE_MAX_SEGS;

    /* Centre vertex at index 0. */
    verts[ 0 ] = ( draw_vertex_t ){ cx, cy, 0.0f, rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ] };

    /* Perimeter vertices at indices 1..segs. */
    f32 step = 2.0f * DRAW_PI / (f32)segs;
    for ( u32 i = 0; i < segs; ++i )
    {
        f32 a = (f32)i * step;
        verts[ 1 + i ] = ( draw_vertex_t ){
            cx + r * cosf( a ), cy + r * sinf( a ), 0.0f,
            rgba[ 0 ], rgba[ 1 ], rgba[ 2 ], rgba[ 3 ],
        };
    }

    /* Triangle fan: (0, i+1, i+2), wrapping last segment back to vertex 1. */
    for ( u32 i = 0; i < segs; ++i )
    {
        indices[ i * 3 + 0 ] = 0;
        indices[ i * 3 + 1 ] = (u16)( 1 + i );
        indices[ i * 3 + 2 ] = (u16)( 1 + ( i + 1 ) % segs );
    }

    *nv = segs + 1;
    *ni = segs * 3;
}

/*============================================================================================*/