/*==============================================================================================

    runtime_service/imgui/imgui_draw_path.c -- Line and path stroking.

    Builds antialiased / pixel-snapped stroke geometry on top of the raw draw-list reservation
    (draw_prim_begin in imgui_draw.c).  Two layers:

        draw_line / draw_polyline    -- immediate: stroke a single segment or a point array now.
        path_line_to / path_stroke   -- retained: accumulate points, then stroke the whole run.

    Pixel model (see draw_push_rect_filled): integer coordinates fall on the lines *between*
    pixels, so a crisp axis-aligned stroke is one whose two edges both land on integers.  A
    horizontal / vertical single line therefore takes a snapped-rectangle fast path (no
    antialiasing, perfectly crisp like a separator); any other angle -- and every multi-segment
    polyline -- is stroked as an expanded quad strip with a 1px alpha-fading edge, so diagonals and
    the corners between segments stay smooth.

    Antialiasing is free in the fragment shader here: output alpha is vertex-alpha * texel, and the
    solid path samples the white texel (alpha 1), so a vertex authored with alpha 0 contributes
    nothing -- the feather is pure geometry, no shader or vertex-format change.

    Included by imgui.c immediately after imgui_draw.c (uses s_draw, draw_prim_begin/commit,
    draw_push_rect_filled, draw_apply_alpha).

==============================================================================================*/
// clang-format off

#define STROKE_AA 1.0f          /* width of the antialiased edge, in pixels */
#define STROKE_CORE_MIN 0.5f    /* min solid-core half-width kept on thin strokes so an angled 1px
                                   line stays a coherent solid run instead of a dotted alpha tent */

/*----------------------------------------------------------------------------------------------
    Small vec2 helpers (local -- base/ stays stateless and we only need a couple)
----------------------------------------------------------------------------------------------*/

static imgui_vec2_t
v2( f32 x, f32 y )
{
    return ( imgui_vec2_t ){ x, y };
}

/* Left-hand normal (90 deg CCW from a->b travel), normalized; {0,0} for a zero-length segment. */
static imgui_vec2_t
seg_normal( imgui_vec2_t a, imgui_vec2_t b )
{
    f32 dx  = b.x - a.x;
    f32 dy  = b.y - a.y;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-6f )
        return v2( 0.0f, 0.0f );
    f32 inv = 1.0f / len;
    return v2( -dy * inv, dx * inv );
}

/*----------------------------------------------------------------------------------------------
    Retained path scratch -- path_line_to appends, path_stroke consumes (ImGui-style).
----------------------------------------------------------------------------------------------*/

static struct
{
    imgui_vec2_t pts[ IMGUI_PATH_MAX ];
    u32          count;

} s_path;

/*----------------------------------------------------------------------------------------------
    Alignment + snapping helpers
----------------------------------------------------------------------------------------------*/

/* Offset (along the left-hand normal) to slide the path centerline by so the stroke sits per the
   alignment.  The left-hand normal (90 deg CCW from a->b travel) points into the interior of a
   clockwise-on-screen ring, so INSIDE moves the band that way (outer edge stays on the path, the
   shape does not grow) and OUTSIDE moves it out: 0 centered, +half fully inside, -half fully
   outside.  (Inside / outside are defined by the winding -- a counter-clockwise ring flips them.) */
static f32
stroke_center_offset( imgui_stroke_align_t align, f32 half )
{
    switch ( align )
    {
        case IMGUI_STROKE_INSIDE:  return  half;
        case IMGUI_STROKE_OUTSIDE: return -half;
        default:                   return 0.0f;   /* CENTER / CENTER_BIASED */
    }
}

/* Snap a stroke's center coordinate on one axis so both its edges land on integer pixel
   boundaries: an odd integer thickness centers on a pixel center (k + 0.5), an even one on a grid
   line (k).  Used only by the axis-aligned crisp fast path. */
static f32
stroke_snap_center( f32 c, f32 thickness )
{
    i32 ti = (i32)floorf( thickness + 0.5f );
    if ( ti & 1 )
        return floorf( c ) + 0.5f;
    return floorf( c + 0.5f );
}

/*----------------------------------------------------------------------------------------------
    stroke_axis_aligned_rect -- crisp fast path for a horizontal / vertical single segment.

    A pure H/V line under a centered alignment is just a 1-to-N px thick rectangle; routing it
    through draw_push_rect_filled (which itself snaps the origin to the grid) makes it pixel-exact,
    matching the look of separators and borders.  Returns false (let the antialiased path take it)
    for diagonal lines or the INSIDE / OUTSIDE alignments, whose edges are not symmetric here.
----------------------------------------------------------------------------------------------*/

static bool
stroke_axis_aligned_rect( f32 x0, f32 y0, f32 x1, f32 y1, f32 t,
                          imgui_stroke_align_t align, u32 abgr )
{
    bool horiz = ( y0 == y1 );
    bool vert  = ( x0 == x1 );
    if ( !horiz && !vert )
        return false;
    if ( align != IMGUI_STROKE_CENTER_BIASED && align != IMGUI_STROKE_CENTER )
        return false;

    bool biased = ( align == IMGUI_STROKE_CENTER_BIASED );

    if ( horiz )
    {
        f32 xa = floorf( ( x0 < x1 ? x0 : x1 ) + 0.5f );   /* snap endpoints to whole pixels */
        f32 xb = floorf( ( x0 < x1 ? x1 : x0 ) + 0.5f );
        f32 cy = biased ? stroke_snap_center( y0, t ) : y0;
        draw_push_rect_filled( xa, cy - t * 0.5f, xb - xa, t, 0, 0, 1, 1, 0, abgr );
    }
    else /* vertical */
    {
        f32 ya = floorf( ( y0 < y1 ? y0 : y1 ) + 0.5f );
        f32 yb = floorf( ( y0 < y1 ? y1 : y0 ) + 0.5f );
        f32 cx = biased ? stroke_snap_center( x0, t ) : x0;
        draw_push_rect_filled( cx - t * 0.5f, ya, t, yb - ya, 0, 0, 1, 1, 0, abgr );
    }
    return true;
}

/*----------------------------------------------------------------------------------------------
    stroke_poly_aa -- antialiased thick polyline (the general path).

    Expands each point to four offset vertices along its miter normal: outer+ / inner+ / inner- /
    outer-.  The inner pair carries the solid color; the outer pair fades alpha to 0 for the 1px
    edge.  Consecutive points are bridged by three quads (the + feather band, the solid core, the
    - feather band), built as one strip so there is no overlap to double-blend under alpha < 1.

    Joints use a miter normal -- the averaged adjacent segment normals, scaled to length
    1/cos(theta/2) so a moved edge still lands `half` from each segment -- clamped to a limit so a
    sharp corner falls back to a short bevel-like cut instead of shooting a long spike.

    A sub-pixel thickness keeps its 1px footprint but fades peak alpha by the coverage, so thin
    diagonal lines render as a soft hairline rather than dropping out.  The solid core is floored so
    it never fully collapses into an alpha tent -- that floor is what keeps a 1px angled line a
    coherent run instead of a dotted one.  center_off slides the whole width to one side for the
    INSIDE / OUTSIDE alignments.
----------------------------------------------------------------------------------------------*/

static void
stroke_poly_aa( const imgui_vec2_t* pts, u32 n, f32 thickness, f32 center_off, bool closed, u32 abgr )
{
    if ( n < 2 )
        return;
    if ( n > IMGUI_PATH_MAX )
        n = IMGUI_PATH_MAX;

    abgr = draw_apply_alpha( abgr );    /* fold in the per-item opacity (disabled dim) */

    /* Sub-pixel coverage: hold a 1px footprint, fade peak alpha by the requested thickness. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )
    {
        a_scale   = thickness < 0.0f ? 0.0f : thickness;
        thickness = 1.0f;
    }
    u32 a_in  = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col   = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );    /* inner / solid color */
    u32 col0  = ( abgr & 0x00FFFFFFu );                      /* outer feather, alpha 0 */

    f32 half  = thickness * 0.5f;

    /* Carve the feather out of the inside, but never let the solid core collapse: at inner == 0 the
       whole stroke is a pure alpha tent whose full-alpha peak only grazes the centerline, so along a
       diagonal the per-pixel coverage oscillates and a 1px line breaks into dashes.  Floor the core
       half-width at STROKE_CORE_MIN (capped at `half` for the thinnest strokes) so every
       cross-section keeps a solid, fully-covered span -- a 1px diagonal becomes a coherent hard run,
       and thick lines (half - STROKE_AA already >= the floor) are unchanged. */
    f32 core_min = ( half < STROKE_CORE_MIN ) ? half : STROKE_CORE_MIN;
    f32 inner    = half - STROKE_AA;
    if ( inner < core_min )
        inner = core_min;

    u32 seg = closed ? n : n - 1;       /* number of bridged segments */

    /* Per-point miter normal: the bisector of the two adjacent segment normals (segment normal at
       the open ends), scaled so an edge moved by it still sits `half` off each segment. */
    imgui_vec2_t nrm[ IMGUI_PATH_MAX ];
    for ( u32 i = 0; i < n; ++i )
    {
        imgui_vec2_t n0, n1;
        if ( closed )
        {
            n0 = seg_normal( pts[ ( i + n - 1 ) % n ], pts[ i ] );
            n1 = seg_normal( pts[ i ], pts[ ( i + 1 ) % n ] );
        }
        else
        {
            n0 = ( i > 0 )     ? seg_normal( pts[ i - 1 ], pts[ i ] ) : v2( 0.0f, 0.0f );
            n1 = ( i < n - 1 ) ? seg_normal( pts[ i ], pts[ i + 1 ] ) : v2( 0.0f, 0.0f );
        }

        if ( !closed && i == 0 )
            nrm[ i ] = n1;                  /* open start: use the leaving segment's normal */
        else if ( !closed && i == n - 1 )
            nrm[ i ] = n0;                  /* open end: use the entering segment's normal */
        else
        {
            imgui_vec2_t dm = v2( ( n0.x + n1.x ) * 0.5f, ( n0.y + n1.y ) * 0.5f );
            f32 d2 = dm.x * dm.x + dm.y * dm.y;
            if ( d2 > 1e-6f )
            {
                f32 inv = 1.0f / d2;
                if ( inv > 100.0f )         /* miter limit: clamp the spike on sharp angles */
                    inv = 100.0f;
                dm.x *= inv;
                dm.y *= inv;
                nrm[ i ] = dm;
            }
            else
            {
                nrm[ i ] = n1;              /* ~180 deg fold-back; pick a side */
            }
        }
    }

    u32 nv = 4u * n;
    u32 ni = 18u * seg;

    f32                 wu, wv;
    imgui_draw_vert_t*  v;
    u16*                idx;
    u16                 base;
    if ( !draw_prim_begin( nv, ni, &wu, &wv, &v, &idx, &base ) )
        return;

    /* Four offset vertices per point: outer+ (a=0), inner+ , inner- , outer- (a=0). */
    for ( u32 i = 0; i < n; ++i )
    {
        imgui_vec2_t m  = nrm[ i ];
        f32          cx = pts[ i ].x + m.x * center_off;
        f32          cy = pts[ i ].y + m.y * center_off;

        v[ 4 * i + 0 ] = ( imgui_draw_vert_t ){ cx + m.x * half,  cy + m.y * half,  wu, wv, col0 };
        v[ 4 * i + 1 ] = ( imgui_draw_vert_t ){ cx + m.x * inner, cy + m.y * inner, wu, wv, col  };
        v[ 4 * i + 2 ] = ( imgui_draw_vert_t ){ cx - m.x * inner, cy - m.y * inner, wu, wv, col  };
        v[ 4 * i + 3 ] = ( imgui_draw_vert_t ){ cx - m.x * half,  cy - m.y * half,  wu, wv, col0 };
    }

    /* Bridge each segment with three quads: (+feather)(0,1) (core)(1,2) (-feather)(2,3). */
    static const int band[ 3 ][ 2 ] = { { 0, 1 }, { 1, 2 }, { 2, 3 } };
    u32 k = 0;
    for ( u32 s = 0; s < seg; ++s )
    {
        u16 i0 = (u16)( base + 4u * s );
        u16 i1 = (u16)( base + 4u * ( ( s + 1 ) % n ) );

        for ( int q = 0; q < 3; ++q )
        {
            u16 a0 = (u16)( i0 + band[ q ][ 0 ] );
            u16 a1 = (u16)( i0 + band[ q ][ 1 ] );
            u16 b0 = (u16)( i1 + band[ q ][ 0 ] );
            u16 b1 = (u16)( i1 + band[ q ][ 1 ] );

            idx[ k++ ] = a0; idx[ k++ ] = a1; idx[ k++ ] = b1;
            idx[ k++ ] = a0; idx[ k++ ] = b1; idx[ k++ ] = b0;
        }
    }

    draw_prim_commit( nv, ni );
}

/*==============================================================================================
    Public draw-list entry points
==============================================================================================*/

/* Stroke an array of points as one connected polyline (closed joins the last point back to the
   first).  All angles are antialiased with miter-limited corners. */
void
imgui_draw_polyline( const imgui_vec2_t* pts, u32 count, f32 thickness,
                     imgui_stroke_align_t align, bool closed, u32 abgr )
{
    if ( !pts || count < 2 || thickness <= 0.0f )
        return;
    stroke_poly_aa( pts, count, thickness, stroke_center_offset( align, thickness * 0.5f ),
                    closed, abgr );
}

/* Stroke a single segment.  Horizontal / vertical lines render pixel-crisp (snapped); any other
   angle is antialiased.  Uses the CENTER_BIASED alignment -- centered and grid-snapped. */
void
imgui_draw_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr )
{
    if ( thickness <= 0.0f )
        return;
    if ( stroke_axis_aligned_rect( x0, y0, x1, y1, thickness, IMGUI_STROKE_CENTER_BIASED, abgr ) )
        return;

    imgui_vec2_t p[ 2 ] = { { x0, y0 }, { x1, y1 } };
    stroke_poly_aa( p, 2, thickness, 0.0f, false, abgr );
}

/* Retained path: clear, append points, then stroke (which consumes the buffer). */
void
imgui_path_clear( void )
{
    s_path.count = 0;
}

void
imgui_path_line_to( f32 x, f32 y )
{
    if ( s_path.count < IMGUI_PATH_MAX )
        s_path.pts[ s_path.count++ ] = v2( x, y );
}

void
imgui_path_stroke( f32 thickness, imgui_stroke_align_t align, bool closed, u32 abgr )
{
    imgui_draw_polyline( s_path.pts, s_path.count, thickness, align, closed, abgr );
    s_path.count = 0;     /* consume the path, so the next build starts fresh */
}

// clang-format on
/*============================================================================================*/
