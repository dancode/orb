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

    Included by imgui_backend.c immediately after imgui_draw.c (uses s_draw, draw_prim_begin/commit,
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

/*==============================================================================================
    Public draw-list entry points
==============================================================================================*/

/* Copy points into the per-frame point pool and push a CMD_POLYLINE semantic command.
   The pool (s_draw.points[]) is stable until imgui_render_flush, where tess_dispatch reads it. */
static void
draw_push_polyline_cmd( const imgui_vec2_t* pts, u32 count, f32 thickness,
                        imgui_stroke_align_t align, bool closed, u32 abgr )
{
    if ( count < 2 || s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    if ( s_draw.pt_count + count > IMGUI_MAX_PATH_PTS )
        return;   /* point pool exhausted this frame */

    /* Cull against the bounding box of the points, padded by half the stroke width so a thick line
       grazing the clip edge is never wrongly dropped. */
    f32 minx = pts[ 0 ].x, maxx = pts[ 0 ].x, miny = pts[ 0 ].y, maxy = pts[ 0 ].y;
    for ( u32 i = 1; i < count; ++i )
    {
        if ( pts[ i ].x < minx ) minx = pts[ i ].x;
        if ( pts[ i ].x > maxx ) maxx = pts[ i ].x;
        if ( pts[ i ].y < miny ) miny = pts[ i ].y;
        if ( pts[ i ].y > maxy ) maxy = pts[ i ].y;
    }
    f32 pad = thickness * 0.5f + 1.0f;
    if ( draw_cull_box( minx - pad, miny - pad, ( maxx - minx ) + 2.0f * pad, ( maxy - miny ) + 2.0f * pad ) )
        return;

    u32 pt_offset = s_draw.pt_count;
    for ( u32 i = 0; i < count; ++i )
        s_draw.points[ s_draw.pt_count++ ] = pts[ i ];

    imgui_cmd_t* c        = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type               = IMGUI_CMD_POLYLINE;
    c->clip               = draw_current_clip();
    c->z                  = s_draw.cur_z;
    c->vp                 = s_draw.cur_vp;
    c->polyline.pt_offset = pt_offset;
    c->polyline.pt_count  = count;
    c->polyline.thickness = thickness;
    c->polyline.align     = align;
    c->polyline.closed    = closed;
    c->polyline.abgr      = draw_apply_alpha( abgr );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );   /* points are L1-hot here */
}

/* Stroke an array of points as one connected polyline (closed joins the last point back to the
   first).  All angles are antialiased with miter-limited corners. */
void
imgui_draw_polyline( const imgui_vec2_t* pts, u32 count, f32 thickness,
                     imgui_stroke_align_t align, bool closed, u32 abgr )
{
    if ( !pts || count < 2 || thickness <= 0.0f )
        return;
    draw_push_polyline_cmd( pts, count, thickness, align, closed, abgr );
}

/* Stroke a single segment.  Horizontal / vertical lines render pixel-crisp (snapped rect);
   diagonal lines are pushed as CMD_LINE (tessellated as an antialiased 2-point polyline). */
void
imgui_draw_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr )
{
    if ( thickness <= 0.0f )
        return;
    /* H/V fast path: axis-aligned lines become a snapped solid rect -- crisp like a separator. */
    if ( stroke_axis_aligned_rect( x0, y0, x1, y1, thickness, IMGUI_STROKE_CENTER_BIASED, abgr ) )
        return;
    /* Diagonal: push a CMD_LINE; tessellated at flush as a 2-point antialiased stroke. */
    if ( s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    {
        f32 lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
        f32 hx = x0 > x1 ? x0 : x1, hy = y0 > y1 ? y0 : y1;
        f32 pad = thickness * 0.5f + 1.0f;
        if ( draw_cull_box( lx - pad, ly - pad, ( hx - lx ) + 2.0f * pad, ( hy - ly ) + 2.0f * pad ) )
            return;
    }
    imgui_cmd_t* c    = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type           = IMGUI_CMD_LINE;
    c->clip           = draw_current_clip();
    c->z              = s_draw.cur_z;
    c->vp             = s_draw.cur_vp;
    c->line.x0        = x0; c->line.y0 = y0;
    c->line.x1        = x1; c->line.y1 = y1;
    c->line.thickness = thickness;
    c->line.abgr      = draw_apply_alpha( abgr );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
}

/* Dashed / dotted line: one CMD_DASHED_LINE, tessellated at flush into a single textured quad
   that tiles an atlas stipple row along the line.  `dash` / `gap` set the on-run / gap lengths in
   pixels; period = dash + gap drives the tile count and duty = dash/period picks the nearest baked
   pattern.  This is the efficient replacement for emitting one stroke per dash. */
void
imgui_draw_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 dash, f32 gap, f32 thickness, u32 abgr )
{
    if ( thickness <= 0.0f || dash <= 0.0f )
        return;
    f32 period = dash + ( gap > 0.0f ? gap : dash );
    if ( period <= 0.0f || s_draw.cmd_count >= IMGUI_MAX_CMDS )
        return;
    {
        f32 lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
        f32 hx = x0 > x1 ? x0 : x1, hy = y0 > y1 ? y0 : y1;
        f32 pad = thickness * 0.5f + 1.0f;
        if ( draw_cull_box( lx - pad, ly - pad, ( hx - lx ) + 2.0f * pad, ( hy - ly ) + 2.0f * pad ) )
            return;
    }
    imgui_cmd_t* c    = &s_draw.cmds[ s_draw.cmd_count++ ];
    c->type           = IMGUI_CMD_DASHED_LINE;
    c->clip           = draw_current_clip();
    c->z              = s_draw.cur_z;
    c->vp             = s_draw.cur_vp;
    c->dash.x0        = x0; c->dash.y0 = y0;
    c->dash.x1        = x1; c->dash.y1 = y1;
    c->dash.thickness = thickness;
    c->dash.period    = period;
    c->dash.duty      = dash / period;
    c->dash.abgr      = draw_apply_alpha( abgr );
    s_draw.cmd_hashes[ s_draw.cmd_count - 1 ] = draw_hash_cmd( c );
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
    if ( s_path.count >= 2 )
        draw_push_polyline_cmd( s_path.pts, s_path.count, thickness, align, closed, abgr );
    s_path.count = 0;   /* consume the path so the next build starts fresh */
}

// clang-format on
/*============================================================================================*/
