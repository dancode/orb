/*==============================================================================================

    runtime_service/gui/backend/gui_02_build_tess.c -- CPU-side tessellation engine.

    Translates the frame's semantic gui_cmd_t list (s_draw) into packed vertex/index
    geometry in s_tess.  This is the CPU half of the command-list split: everything here
    reads semantic commands and writes gui_draw_vert_t / u16 index data; nothing here
    touches the GPU API.

    s_tess is read only by the two files included immediately after: gui_02_build_cache.c (the
    BUILD phase fills it via tess_dispatch) and gui_03_submit_render.c (gui_render_flush uploads it and
    emits draw calls).  No file above the backend unit touches it.

    Included by gui_backend.c after gui_01_emit_path.c (provides v2, seg_normal,
    stroke_center_offset, STROKE_* constants) and before gui_02_build_cache.c (which drives
    tess_reset / tess_dispatch from cache_build_frame / cache_tess_window).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Tessellation state -- private vertex/index buffers populated from the semantic command list.

    cache_build_frame tessellates the frame's gui_cmd_t list into s_tess (per window, via
    tess_dispatch), then gui_render_flush uploads s_tess.verts/indices to the GPU.  s_tess is
    backend-private; nothing above the backend unit touches it.  s_draw holds only semantic
    commands (gui_cmd_t) -- no vtx/idx buffers.

    cur_clip/cur_vp are written by tess_dispatch before each primitive so tess_ensure_gpu_cmd
    can stamp the correct context onto new GPU commands without extra parameters.  cur_clip is
    resolved from s_draw.clip_table[c->clip_idx]; z is per-segment and is not tracked here.
----------------------------------------------------------------------------------------------*/

static struct
{
    gui_draw_vert_t  verts    [ GUI_MAX_VERTS ];
    u16                indices  [ GUI_MAX_IDX   ];
    gui_gpu_cmd_t    cmds     [ GUI_MAX_CMDS  ];
    u32                cmd_vp   [ GUI_MAX_CMDS  ];
    u32                cmd_vbase[ GUI_MAX_CMDS  ];  /* vertex-buffer slot where this cmd's geometry starts */

    u32 vert_count, idx_count, cmd_count;

    gui_rect_t cur_clip;   /* clip resolved from s_draw.clip_table[c->clip_idx] for each command */
    u32          cur_vp;    /* viewport baked from the current semantic command                    */

    /* Vertex base of the window slot currently being tessellated.  Index values emitted during
       tess are (local_vert - slot_vert_base), making them 0-relative within the slot.  At draw
       time vertex_offset = slot.vert_base shifts them to the correct absolute VB position. */
    u32 slot_vert_base;

    /* Set before each cache_tess_window call so tess_ensure_gpu_cmd always opens a fresh
       command for the first primitive of a new slot, even when the previous slot's last
       command shares the same clip/tex/vp (same-position windows would otherwise merge
       across the slot boundary and corrupt elem_count + first_index tracking). */
    bool force_new_cmd;

    u32  vert_hwm, idx_hwm;
    bool overflow, overflow_ever;

} s_tess;

/*----------------------------------------------------------------------------------------------
    Cached corner geometry -- the rounded-rect optimization.

    A rounded rectangle's four corners are the same quarter circle, only mirrored and translated.
    So we sample the unit quarter arc (cos / sin at GUI_ROUND_SEGS+1 angles across 0..90 deg)
    exactly once, then every corner of every rounded rect this run is that one table scaled by the
    radius and offset to the corner centre with per-corner axis signs -- the cached unit arc is the
    basis of the emit transform.  No sin / cos runs per rect; only once, on first use.

    Segment count adapts to the radius (round_rect_segs): a small widget corner is visually
    indistinguishable at 2 segments, so it needs far fewer triangles than a large window corner.
    The count is kept to a divisor of GUI_ROUND_SEGS so a coarser arc is just the full table
    STRIDED (k * GUI_ROUND_SEGS/segs) -- a full quarter at lower resolution with no extra trig,
    so the single cached table still serves every tier.
----------------------------------------------------------------------------------------------*/

#define GUI_ROUND_SEGS 8                                  /* max segments per corner (finest tier) */
#define GUI_ROUND_PTS  ( 4 * ( GUI_ROUND_SEGS + 1 ) )   /* perimeter point count at the finest tier */

static f32  s_arc_cos[ GUI_ROUND_SEGS + 1 ];
static f32  s_arc_sin[ GUI_ROUND_SEGS + 1 ];
static bool s_arc_ready;

static void
round_arc_init( void )
{
    if ( s_arc_ready ) return;
    for ( u32 k = 0; k <= GUI_ROUND_SEGS; ++k )
    {
        f32 a = 1.5707963f * (f32)k / (f32)GUI_ROUND_SEGS;   /* 0 .. pi/2 */
        s_arc_cos[ k ] = cosf( a );
        s_arc_sin[ k ] = sinf( a );
    }
    s_arc_ready = true;
}

/* Segments per corner for a given radius -- restricted to divisors of GUI_ROUND_SEGS so the
   cached unit arc can be strided (see the note above).  Small corners drop to 2 segments (a
   visually clean bevel at a few pixels), mid radii to 4, large to the full 8. */
static u32
round_rect_segs( f32 r )
{
    if ( r <= 3.0f ) return 2;
    if ( r <= 6.0f ) return 4;
    return GUI_ROUND_SEGS;   /* 8 -- large (window) corners */
}

/* Build the rounded-rect perimeter, clockwise, into out[] (capacity GUI_ROUND_PTS).  Each corner
   reuses the cached unit arc -- strided by GUI_ROUND_SEGS/segs so `segs` (a divisor of
   GUI_ROUND_SEGS) gives a full quarter at that resolution -- scaled by r and translated to its
   centre; the straight edges fall out between adjacent corners' shared endpoints.  Returns the
   point count, 4 * (segs + 1). */
static u32
round_rect_perimeter( f32 x, f32 y, f32 w, f32 h, f32 r, u32 segs, gui_vec2_t* out )
{
    round_arc_init();
    u32 st = GUI_ROUND_SEGS / segs;          /* table stride; segs divides GUI_ROUND_SEGS */
    f32 xl = x + r,       yt = y + r;          /* near-corner arc centres */
    f32 xr = x + w - r,   yb = y + h - r;      /* far-corner arc centres  */
    u32 n = 0;
    for ( u32 k = 0; k <= segs; ++k )   /* top-right: up   -> right */
        out[ n++ ] = v2( xr + r * s_arc_sin[ k * st ], yt - r * s_arc_cos[ k * st ] );
    for ( u32 k = 0; k <= segs; ++k )   /* bottom-right: right -> down */
        out[ n++ ] = v2( xr + r * s_arc_cos[ k * st ], yb + r * s_arc_sin[ k * st ] );
    for ( u32 k = 0; k <= segs; ++k )   /* bottom-left: down  -> left */
        out[ n++ ] = v2( xl - r * s_arc_sin[ k * st ], yb + r * s_arc_cos[ k * st ] );
    for ( u32 k = 0; k <= segs; ++k )   /* top-left: left   -> up   */
        out[ n++ ] = v2( xl - r * s_arc_cos[ k * st ], yt - r * s_arc_sin[ k * st ] );
    return n;
}

/*----------------------------------------------------------------------------------------------
    Tessellation helpers -- mirrors of the draw_push_* functions in gui_01_emit_draw.c, but writing
    into s_tess instead of s_draw.  These are the backend half of the command-list split.
    Called from tess_dispatch; not called from anywhere else.
----------------------------------------------------------------------------------------------*/

static void
tess_reset( void )
{
    s_tess.vert_count      = 0;
    s_tess.idx_count       = 0;
    s_tess.cmd_count       = 0;
    s_tess.slot_vert_base  = 0;
    s_tess.force_new_cmd   = false;
    s_tess.overflow        = false;
}

/* Open a new GPU command when texture, clip, or viewport changes -- same batching logic as
   draw_ensure_cmd but writing into s_tess.  z is per-segment (not per-command) so it is not
   a batch boundary here; the segment system already guarantees all commands in one window's
   tessellation pass share the same z. */
static void
tess_ensure_gpu_cmd( u32 tex_idx )
{
    if ( s_tess.cmd_count > 0 && !s_tess.force_new_cmd )
    {
        const gui_gpu_cmd_t* cur = &s_tess.cmds[ s_tess.cmd_count - 1 ];
        if ( s_tess.cmd_vp[ s_tess.cmd_count - 1 ] == s_tess.cur_vp
          && cur->tex_idx       == tex_idx
          && cur->clip_rect.x   == s_tess.cur_clip.x
          && cur->clip_rect.y   == s_tess.cur_clip.y
          && cur->clip_rect.w   == s_tess.cur_clip.w
          && cur->clip_rect.h   == s_tess.cur_clip.h )
            return;
    }
    s_tess.force_new_cmd = false;
    if ( s_tess.cmd_count >= GUI_MAX_CMDS )
        return;
    s_tess.cmd_vp   [ s_tess.cmd_count ] = s_tess.cur_vp;
    /* Vertex span of this command starts at the current vert_count; the next command's vbase (or
       the final vert_count for the last) bounds it.  Lets a surface upload only its own vertices. */
    s_tess.cmd_vbase[ s_tess.cmd_count ] = s_tess.vert_count;
    s_tess.cmds     [ s_tess.cmd_count++ ] = ( gui_gpu_cmd_t ){
        .elem_count = 0,
        .tex_idx    = tex_idx,
        .clip_rect  = s_tess.cur_clip,
    };
}

/* Raw vertex/index reservation for stroke tessellation -- mirror of draw_prim_begin/commit. */
static bool
tess_prim_begin( u32 nv, u32 ni, f32* wu, f32* wv,
                 gui_draw_vert_t** out_v, u16** out_i, u16* out_base )
{
    if ( s_tess.vert_count + nv > GUI_MAX_VERTS || s_tess.idx_count + ni > GUI_MAX_IDX )
    {
        s_tess.overflow = true;
        return false;
    }
    u32 tex = font_atlas_idx();
    tess_ensure_gpu_cmd( tex );
    if ( s_tess.cmd_count == 0 )
        return false;
    font_white_uv( wu, wv );
    *out_base = (u16)( s_tess.vert_count - s_tess.slot_vert_base );
    *out_v    = &s_tess.verts  [ s_tess.vert_count ];
    *out_i    = &s_tess.indices[ s_tess.idx_count  ];
    return true;
}

static void
tess_prim_commit( u32 nv, u32 ni )
{
    s_tess.vert_count += nv;
    s_tess.idx_count  += ni;
    s_tess.cmds[ s_tess.cmd_count - 1 ].elem_count += ni;
}

/* Tessellate a filled quad into s_tess.  abgr has alpha pre-baked by the emit side. */
static void
tess_rect_filled( f32 x, f32 y, f32 w, f32 h,
                  f32 u0, f32 v0, f32 u1, f32 v1,
                  u32 tex_idx, u32 abgr )
{
    if ( s_tess.vert_count + 4 > GUI_MAX_VERTS || s_tess.idx_count + 6 > GUI_MAX_IDX )
    {
        s_tess.overflow = true;
        return;
    }
    /* tex_idx 0 = solid-color convention: route to the font atlas's white texel. */
    if ( tex_idx == 0 )
    {
        tex_idx = font_atlas_idx();
        font_white_uv( &u0, &v0 );
        u1 = u0; v1 = v0;
    }
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );
    tess_ensure_gpu_cmd( tex_idx );
    if ( s_tess.cmd_count == 0 )
        return;

    u16 base = (u16)( s_tess.vert_count - s_tess.slot_vert_base );
    gui_draw_vert_t* v = &s_tess.verts[ s_tess.vert_count ];
    v[ 0 ] = ( gui_draw_vert_t ){ x,     y,     u0, v0, abgr };
    v[ 1 ] = ( gui_draw_vert_t ){ x + w, y,     u1, v0, abgr };
    v[ 2 ] = ( gui_draw_vert_t ){ x + w, y + h, u1, v1, abgr };
    v[ 3 ] = ( gui_draw_vert_t ){ x,     y + h, u0, v1, abgr };
    s_tess.vert_count += 4;

    u16* idx = &s_tess.indices[ s_tess.idx_count ];
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base + 0; idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
    s_tess.idx_count += 6;

    s_tess.cmds[ s_tess.cmd_count - 1 ].elem_count += 6;
}

/* Tessellate a two-color gradient quad: col_a / col_b on opposite edges, sampled at the white
   texel so the GPU's per-vertex color interpolation IS the gradient (one quad, exact blend --
   replaces the old 32-band approximation).  Origin grid-snapped like tess_rect_filled. */
static void
tess_rect_gradient( f32 x, f32 y, f32 w, f32 h, u32 col_a, u32 col_b, bool horizontal )
{
    if ( s_tess.vert_count + 4 > GUI_MAX_VERTS || s_tess.idx_count + 6 > GUI_MAX_IDX )
    {
        s_tess.overflow = true;
        return;
    }
    f32 wu, wv;
    font_white_uv( &wu, &wv );

    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );
    tess_ensure_gpu_cmd( font_atlas_idx() );
    if ( s_tess.cmd_count == 0 )
        return;

    /* Corner colors walk col_a -> col_b along the chosen axis (TL, TR, BR, BL winding). */
    u32 c0 = col_a;                          /* top-left  */
    u32 c1 = horizontal ? col_b : col_a;     /* top-right */
    u32 c2 = col_b;                          /* bottom-right */
    u32 c3 = horizontal ? col_a : col_b;     /* bottom-left  */

    u16 base = (u16)( s_tess.vert_count - s_tess.slot_vert_base );
    gui_draw_vert_t* v = &s_tess.verts[ s_tess.vert_count ];
    v[ 0 ] = ( gui_draw_vert_t ){ x,     y,     wu, wv, c0 };
    v[ 1 ] = ( gui_draw_vert_t ){ x + w, y,     wu, wv, c1 };
    v[ 2 ] = ( gui_draw_vert_t ){ x + w, y + h, wu, wv, c2 };
    v[ 3 ] = ( gui_draw_vert_t ){ x,     y + h, wu, wv, c3 };
    s_tess.vert_count += 4;

    u16* idx = &s_tess.indices[ s_tess.idx_count ];
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base + 0; idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
    s_tess.idx_count += 6;
    s_tess.cmds[ s_tess.cmd_count - 1 ].elem_count += 6;
}

/* Tessellate a hollow rectangle as four edge quads. */
static void
tess_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    tess_rect_filled( x,         y,         w, t,     0,0,1,1, 0, abgr );
    tess_rect_filled( x,         y + h - t, w, t,     0,0,1,1, 0, abgr );
    tess_rect_filled( x,         y + t,     t, h-2*t, 0,0,1,1, 0, abgr );
    tess_rect_filled( x + w - t, y + t,     t, h-2*t, 0,0,1,1, 0, abgr );
}

/* Tessellate a rounded filled rect as a triangle fan from the centre over the cached perimeter.
   abgr has alpha pre-baked.  The origin is grid-snapped like tess_rect_filled so frames stay crisp. */
static void
tess_round_rect_filled( f32 x, f32 y, f32 w, f32 h, f32 r, u32 abgr )
{
    x = floorf( x + 0.5f );
    y = floorf( y + 0.5f );

    static gui_vec2_t per[ GUI_ROUND_PTS ];   /* single-threaded backend; avoids a stack frame */
    u32 m = round_rect_perimeter( x, y, w, h, r, round_rect_segs( r ), per );

    u32 nv = m + 1, ni = m * 3;                   /* centre vertex + one fan triangle per edge */
    f32 wu, wv;
    gui_draw_vert_t* v;
    u16* idx;
    u16  base;
    if ( !tess_prim_begin( nv, ni, &wu, &wv, &v, &idx, &base ) )
        return;

    v[ 0 ] = ( gui_draw_vert_t ){ x + w * 0.5f, y + h * 0.5f, wu, wv, abgr };   /* fan centre */
    for ( u32 i = 0; i < m; ++i )
        v[ 1 + i ] = ( gui_draw_vert_t ){ per[ i ].x, per[ i ].y, wu, wv, abgr };

    u32 k = 0;
    for ( u32 i = 0; i < m; ++i )
    {
        idx[ k++ ] = base;
        idx[ k++ ] = (u16)( base + 1 + i );
        idx[ k++ ] = (u16)( base + 1 + ( i + 1 ) % m );
    }
    tess_prim_commit( nv, ni );
}

/* Forward decl: the AA polyline stroker is defined further down but the rounded outline below
   reuses it to stroke the perimeter loop. */
static void tess_stroke_poly_aa( const gui_vec2_t* pts, u32 n, f32 thickness, f32 center_off,
                                 bool closed, u32 abgr );

/* Tessellate a rounded hollow rect by stroking the cached perimeter as a closed antialiased loop.
   INSIDE alignment keeps the band within the rect, matching the square tess_rect_outline. */
static void
tess_round_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 r, f32 t, u32 abgr )
{
    static gui_vec2_t per[ GUI_ROUND_PTS ];
    u32 m          = round_rect_perimeter( x, y, w, h, r, round_rect_segs( r ), per );
    f32 center_off = stroke_center_offset( GUI_STROKE_INSIDE, t * 0.5f );
    tess_stroke_poly_aa( per, m, t, center_off, true, abgr );
}

/* Tessellate a solid triangle into s_tess. */
static void
tess_triangle( f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, u32 abgr )
{
    if ( s_tess.vert_count + 3 > GUI_MAX_VERTS || s_tess.idx_count + 3 > GUI_MAX_IDX )
    {
        s_tess.overflow = true;
        return;
    }
    f32 wu, wv;
    font_white_uv( &wu, &wv );
    u32 tex = font_atlas_idx();
    tess_ensure_gpu_cmd( tex );
    if ( s_tess.cmd_count == 0 )
        return;

    u16 base = (u16)( s_tess.vert_count - s_tess.slot_vert_base );
    s_tess.verts[ s_tess.vert_count++ ] = ( gui_draw_vert_t ){ ax, ay, wu, wv, abgr };
    s_tess.verts[ s_tess.vert_count++ ] = ( gui_draw_vert_t ){ bx, by, wu, wv, abgr };
    s_tess.verts[ s_tess.vert_count++ ] = ( gui_draw_vert_t ){ cx, cy, wu, wv, abgr };
    s_tess.indices[ s_tess.idx_count++ ] = base + 0;
    s_tess.indices[ s_tess.idx_count++ ] = base + 1;
    s_tess.indices[ s_tess.idx_count++ ] = base + 2;
    s_tess.cmds[ s_tess.cmd_count - 1 ].elem_count += 3;
}

/* Tessellate a filled disc as a single triangle fan: one centre vertex plus a ring of `segs`
   perimeter vertices, with each fan triangle sharing the ring.  One batch open + one white-uv
   lookup for the whole disc (vs. the old per-triangle path that re-ran tess_ensure_gpu_cmd /
   font_white_uv / font_atlas_idx and emitted 3 unshared vertices for every segment), so a disc
   costs segs+1 vertices instead of 3*segs.  Same index count and winding. */
static void
tess_circle_filled( f32 pcx, f32 pcy, f32 r, u32 segs, u32 abgr )
{
    if ( segs < 3 ) segs = 3;

    u32 nv = segs + 1, ni = segs * 3;             /* centre vertex + one fan triangle per segment */
    f32 wu, wv;
    gui_draw_vert_t* v;
    u16* idx;
    u16  base;
    if ( !tess_prim_begin( nv, ni, &wu, &wv, &v, &idx, &base ) )
        return;

    f32 step = 6.2831853f / (f32)segs;
    v[ 0 ] = ( gui_draw_vert_t ){ pcx, pcy, wu, wv, abgr };   /* fan centre */
    for ( u32 i = 0; i < segs; ++i )
    {
        f32 a = step * (f32)i;
        v[ 1 + i ] = ( gui_draw_vert_t ){ pcx + cosf( a ) * r, pcy + sinf( a ) * r, wu, wv, abgr };
    }

    u32 k = 0;
    for ( u32 i = 0; i < segs; ++i )
    {
        idx[ k++ ] = base;
        idx[ k++ ] = (u16)( base + 1 + i );
        idx[ k++ ] = (u16)( base + 1 + ( i + 1 ) % segs );
    }
    tess_prim_commit( nv, ni );
}

/* Tessellate a glyph run from the font atlas into s_tess, hard-clipped to the horizontal pixel
   window [clip_x0, clip_x1].  Glyphs fully outside the window are skipped; glyphs fully inside emit
   whole; the (at most two) straddling glyphs are cut on a pixel boundary with their U remapped by
   the same fraction -- exact, since the glyph quad is an axis-aligned 1:1 atlas sample.  The window
   is monotonic with the left-to-right cursor, so interior glyphs pay only one compare: no clip math.
   The unclipped sentinel (clip_x1 >= GUI_TEXT_NO_CLIP) takes the original whole-run fast path. */
static void
tess_text_n( f32 x, f32 y, u32 abgr, const char* str, u32 n, f32 clip_x0, f32 clip_x1 )
{
    bool clipped = ( clip_x1 < GUI_TEXT_NO_CLIP );
    f32  cx      = x;
    for ( u32 i = 0; i < n && str[ i ]; ++i )
    {
        u8  ch = (u8)str[ i ];
        f32 u0, v0, u1, v1, ox, oy, gw, gh, advance;
        font_glyph( ch, &u0, &v0, &u1, &v1, &ox, &oy, &gw, &gh, &advance );

        if ( gw > 0.0f && gh > 0.0f )
        {
            f32 gx0 = cx + ox;          /* glyph bitmap left/right in screen px */
            f32 gx1 = gx0 + gw;

            if ( !clipped || ( gx0 >= clip_x0 && gx1 <= clip_x1 ) )
            {
                /* Whole glyph (or no clipping): emit as-is -- the hot interior path. */
                tess_rect_filled( gx0, y + oy, gw, gh, u0, v0, u1, v1, font_atlas_idx(), abgr );
            }
            else if ( gx1 > clip_x0 && gx0 < clip_x1 )
            {
                /* Straddler: cut to the window and walk U by the same fraction on each cut edge. */
                f32 du   = u1 - u0;
                f32 nx0  = gx0, nx1 = gx1, nu0 = u0, nu1 = u1;
                if ( nx0 < clip_x0 )    /* left edge cut  */
                {
                    nu0 = u0 + du * ( ( clip_x0 - gx0 ) / gw );
                    nx0 = clip_x0;
                }
                if ( nx1 > clip_x1 )    /* right edge cut */
                {
                    nu1 = u0 + du * ( ( clip_x1 - gx0 ) / gw );
                    nx1 = clip_x1;
                }
                tess_rect_filled( nx0, y + oy, nx1 - nx0, gh, nu0, v0, nu1, v1,
                                  font_atlas_idx(), abgr );
            }
            /* else: glyph wholly outside the window -- drop it. */
        }

        cx += advance;
        if ( clipped && cx >= clip_x1 )   /* cursor past the window: nothing further is visible */
            break;
    }
}

/* Tessellate a dashed / dotted line as one oriented textured quad sampling the atlas dash row.
   U spans 0..len/period so the row tiles along the line under REPEAT-U addressing; V selects the
   baked row whose on-fraction is closest to `duty`.  O(1) geometry regardless of line length --
   the per-dash quad explosion (which used to exhaust the command list) is gone. */
static void
tess_dashed_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, f32 period, f32 duty, u32 abgr )
{
    if ( thickness <= 0.0f || period <= 0.0f )
        return;
    f32 dx = x1 - x0, dy = y1 - y0;
    f32 len = sqrtf( dx * dx + dy * dy );
    if ( len < 1e-4f )
        return;
    if ( s_tess.vert_count + 4 > GUI_MAX_VERTS || s_tess.idx_count + 6 > GUI_MAX_IDX )
    {
        s_tess.overflow = true;
        return;
    }

    f32 inv  = 1.0f / len;
    f32 ux   = dx * inv, uy = dy * inv;          /* unit vector along the line  */
    f32 nx   = -uy,      ny = ux;                /* unit normal across the line */
    f32 half = thickness * 0.5f;
    f32 umax = len / period;                     /* number of tiled periods -> U span */
    f32 vv   = font_dash_v( duty );

    tess_ensure_gpu_cmd( font_atlas_idx() );
    if ( s_tess.cmd_count == 0 )
        return;

    u16 base = (u16)( s_tess.vert_count - s_tess.slot_vert_base );
    gui_draw_vert_t* v = &s_tess.verts[ s_tess.vert_count ];
    v[ 0 ] = ( gui_draw_vert_t ){ x0 + nx * half, y0 + ny * half, 0.0f, vv, abgr };
    v[ 1 ] = ( gui_draw_vert_t ){ x1 + nx * half, y1 + ny * half, umax, vv, abgr };
    v[ 2 ] = ( gui_draw_vert_t ){ x1 - nx * half, y1 - ny * half, umax, vv, abgr };
    v[ 3 ] = ( gui_draw_vert_t ){ x0 - nx * half, y0 - ny * half, 0.0f, vv, abgr };
    s_tess.vert_count += 4;

    u16* idx = &s_tess.indices[ s_tess.idx_count ];
    idx[ 0 ] = base + 0; idx[ 1 ] = base + 1; idx[ 2 ] = base + 2;
    idx[ 3 ] = base + 0; idx[ 4 ] = base + 2; idx[ 5 ] = base + 3;
    s_tess.idx_count += 6;
    s_tess.cmds[ s_tess.cmd_count - 1 ].elem_count += 6;
}

/*----------------------------------------------------------------------------------------------
    tess_stroke_poly_aa -- antialiased polyline tessellation for the render backend.

    Mirrors the old stroke_poly_aa (removed from gui_01_emit_path.c in step 4) but writes into
    s_tess via tess_prim_begin/commit.  abgr is pre-baked (alpha folded in at emit time).
    v2 / seg_normal / stroke_center_offset are defined in gui_01_emit_path.c (included before
    this file in the unity build) so they are visible here without forward declarations.
----------------------------------------------------------------------------------------------*/

static void
tess_stroke_poly_aa( const gui_vec2_t* pts, u32 n, f32 thickness, f32 center_off,
                     bool closed, u32 abgr )
{
    if ( n < 2 )
        return;
    if ( n > GUI_MAX_PATH_PTS )
         n = GUI_MAX_PATH_PTS;

    /* Sub-pixel coverage: hold a 1px footprint, fade peak alpha by the requested thickness. */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f )
    {
        a_scale   = thickness < 0.0f ? 0.0f : thickness;
        thickness = 1.0f;
    }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );   /* inner / solid color */
    u32 col0 = ( abgr & 0x00FFFFFFu );                     /* outer feather, alpha 0 */

    f32 half     = thickness * 0.5f;
    f32 core_min = ( half < STROKE_CORE_MIN ) ? half : STROKE_CORE_MIN;
    f32 inner    = half - STROKE_AA;
    if ( inner < core_min )
        inner = core_min;

    u32 seg = closed ? n : n - 1;

    /* Per-point miter normal; static avoids an 8K+ stack frame. Single-threaded. */
    static gui_vec2_t nrm[ GUI_MAX_PATH_PTS ];
    for ( u32 i = 0; i < n; ++i )
    {
        gui_vec2_t n0, n1;
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

        if ( !closed && i == 0 )          nrm[ i ] = n1;
        else if ( !closed && i == n - 1 ) nrm[ i ] = n0;
        else
        {
            gui_vec2_t dm = v2( ( n0.x + n1.x ) * 0.5f, ( n0.y + n1.y ) * 0.5f );
            f32 d2 = dm.x * dm.x + dm.y * dm.y;
            if ( d2 > 1e-6f )
            {
                f32 inv = 1.0f / d2;
                if ( inv > 100.0f ) inv = 100.0f;   /* miter limit */
                dm.x *= inv; dm.y *= inv;
                nrm[ i ] = dm;
            }
            else { nrm[ i ] = n1; }
        }
    }

    u32 nv = 4u * n, ni = 18u * seg;
    f32 wu, wv;
    gui_draw_vert_t* v;
    u16* idx;
    u16  base;
    if ( !tess_prim_begin( nv, ni, &wu, &wv, &v, &idx, &base ) )
        return;

    for ( u32 i = 0; i < n; ++i )
    {
        gui_vec2_t m  = nrm[ i ];
        f32          cx = pts[ i ].x + m.x * center_off;
        f32          cy = pts[ i ].y + m.y * center_off;
        v[ 4*i+0 ] = ( gui_draw_vert_t ){ cx + m.x*half,  cy + m.y*half,  wu, wv, col0 };
        v[ 4*i+1 ] = ( gui_draw_vert_t ){ cx + m.x*inner, cy + m.y*inner, wu, wv, col  };
        v[ 4*i+2 ] = ( gui_draw_vert_t ){ cx - m.x*inner, cy - m.y*inner, wu, wv, col  };
        v[ 4*i+3 ] = ( gui_draw_vert_t ){ cx - m.x*half,  cy - m.y*half,  wu, wv, col0 };
    }

    static const int band[ 3 ][ 2 ] = { { 0, 1 }, { 1, 2 }, { 2, 3 } };
    u32 k = 0;
    for ( u32 s = 0; s < seg; ++s )
    {
        u16 i0 = (u16)( base + 4u * s );
        u16 i1 = (u16)( base + 4u * ( ( s + 1 ) % n ) );
        for ( int q = 0; q < 3; ++q )
        {
            u16 a0 = (u16)( i0 + band[q][0] ), a1 = (u16)( i0 + band[q][1] );
            u16 b0 = (u16)( i1 + band[q][0] ), b1 = (u16)( i1 + band[q][1] );
            idx[k++]=a0; idx[k++]=a1; idx[k++]=b1;
            idx[k++]=a0; idx[k++]=b1; idx[k++]=b0;
        }
    }
    tess_prim_commit( nv, ni );
}

/* Fast path for an axis-aligned line: a horizontal (y0==y1) or vertical (x0==x1) line has no
   diagonal edge to feather, so a crisp grid-snapped quad is indistinguishable from the AA stroke
   while costing a fraction of the geometry -- 4 verts / 6 idx vs. 8 verts / 18 idx, and none of the
   per-point miter-normal solve.  This is the common case (separators, frame borders, table grid
   lines, underlines).  Sub-pixel thickness fades the alpha exactly as tess_stroke_poly_aa does so a
   hairline keeps its weight.  Returns false for a diagonal line, which falls back to the stroker. */
static bool
tess_axis_line( f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, u32 abgr )
{
    bool horizontal = ( y0 == y1 );
    bool vertical   = ( x0 == x1 );
    if ( !horizontal && !vertical )
        return false;            /* diagonal -- needs the AA stroker */
    if ( thickness <= 0.0f )
        return true;             /* axis-aligned but nothing to draw -- consumed */

    /* Sub-pixel coverage: hold a 1px footprint and fade peak alpha (mirrors the AA stroker). */
    f32 a_scale = 1.0f;
    if ( thickness < 1.0f ) { a_scale = thickness; thickness = 1.0f; }
    u32 a_in = (u32)( ( ( abgr >> 24 ) & 0xFFu ) * a_scale + 0.5f );
    u32 col  = ( abgr & 0x00FFFFFFu ) | ( a_in << 24 );

    /* Centre the band on the line, exactly as the stroker does (CMD_LINE passes center_off 0).
       tess_rect_filled grid-snaps the origin, so the quad lands crisp on the pixel grid. */
    f32 half = thickness * 0.5f;
    if ( horizontal )
    {
        f32 xa = x0 < x1 ? x0 : x1;
        f32 xb = x0 < x1 ? x1 : x0;
        tess_rect_filled( xa, y0 - half, xb - xa, thickness, 0, 0, 1, 1, 0, col );
    }
    else /* vertical */
    {
        f32 ya = y0 < y1 ? y0 : y1;
        f32 yb = y0 < y1 ? y1 : y0;
        tess_rect_filled( x0 - half, ya, thickness, yb - ya, 0, 0, 1, 1, 0, col );
    }
    return true;
}

/* Tessellate one frame's semantic command list into s_tess geometry.

   `order` is a permutation of [0,count): the indices grouped by clip within each z-run (built by
   cache_tess_window) so equal-clip commands tessellate contiguously and collapse into one GPU batch.
   `fonts` is the parallel font id of each ordered entry (its segment's font).  Before tessellating a
   command we activate its font so the tess-time lookups -- font_glyph (UVs), font_atlas_idx (atlas),
   the white texel and dash rows -- resolve from the right atlas; tess_ensure_gpu_cmd then splits the
   GPU batch on the resulting atlas change.  The active font is saved and restored so the BUILD phase
   leaves the global font state (used by the next frame's layout) untouched. */
static void
tess_dispatch( const gui_cmd_t* cmds, const u32* order, const u32* fonts, u32 count )
{
    u32 saved_font = font_active_id();
    u32 cur_font   = saved_font;

    for ( u32 oi = 0; oi < count; ++oi )
    {
        const gui_cmd_t* c = &cmds[ order[ oi ] ];

        /* Switch the atlas batch context to this command's segment font when it changes. */
        if ( fonts[ oi ] != cur_font )
        {
            cur_font = fonts[ oi ];
            font_use( cur_font );
        }

        s_tess.cur_clip = s_draw.clip_table[ c->clip_idx ];
        s_tess.cur_vp   = c->vp;

        switch ( c->type )
        {
            case GUI_CMD_RECT_FILLED:
                if ( c->rect.rounding > 0.0f )
                    tess_round_rect_filled( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                            c->rect.rounding, c->rect.abgr );
                else
                    tess_rect_filled( c->rect.x, c->rect.y, c->rect.w, c->rect.h,
                                      c->rect.u0, c->rect.v0, c->rect.u1, c->rect.v1,
                                      c->rect.tex_idx, c->rect.abgr );
                break;

            case GUI_CMD_RECT_OUTLINE:
                if ( c->rect_outline.rounding > 0.0f )
                    tess_round_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                             c->rect_outline.w, c->rect_outline.h,
                                             c->rect_outline.rounding, c->rect_outline.t,
                                             c->rect_outline.abgr );
                else
                    tess_rect_outline( c->rect_outline.x, c->rect_outline.y,
                                       c->rect_outline.w, c->rect_outline.h,
                                       c->rect_outline.t, c->rect_outline.abgr );
                break;

            case GUI_CMD_TRIANGLE:
                tess_triangle( c->tri.ax, c->tri.ay, c->tri.bx, c->tri.by,
                               c->tri.cx, c->tri.cy, c->tri.abgr );
                break;

            case GUI_CMD_TEXT:
                tess_text_n( c->text.x, c->text.y, c->text.abgr, s_draw.text_pool + c->text.off,
                             c->text.len, c->text.clip_x0, c->text.clip_x1 );
                break;

            case GUI_CMD_CIRCLE_FILLED:
                tess_circle_filled( c->circle.cx, c->circle.cy, c->circle.r,
                                    c->circle.segs, c->circle.abgr );
                break;

            case GUI_CMD_LINE:
            {
                /* Axis-aligned lines take the crisp-quad fast path; only diagonals reach the
                   AA stroker (tess_axis_line returns false for those). */
                if ( tess_axis_line( c->line.x0, c->line.y0, c->line.x1, c->line.y1,
                                     c->line.thickness, c->line.abgr ) )
                    break;
                gui_vec2_t pts[ 2 ] = { { c->line.x0, c->line.y0 }, { c->line.x1, c->line.y1 } };
                tess_stroke_poly_aa( pts, 2, c->line.thickness, 0.0f, false, c->line.abgr );
                break;
            }

            case GUI_CMD_POLYLINE:
            {
                const gui_vec2_t* pts = &s_draw.points[ c->polyline.pt_offset ];
                f32 center_off = stroke_center_offset( c->polyline.align, c->polyline.thickness * 0.5f );
                tess_stroke_poly_aa( pts, c->polyline.pt_count, c->polyline.thickness,
                                     center_off, c->polyline.closed, c->polyline.abgr );
                break;
            }

            case GUI_CMD_DASHED_LINE:
                tess_dashed_line( c->dash.x0, c->dash.y0, c->dash.x1, c->dash.y1,
                                  c->dash.thickness, c->dash.period, c->dash.duty, c->dash.abgr );
                break;

            case GUI_CMD_RECT_GRADIENT:
                tess_rect_gradient( c->gradient.x, c->gradient.y, c->gradient.w, c->gradient.h,
                                    c->gradient.col_a, c->gradient.col_b, c->gradient.horizontal );
                break;
        }
    }

    /* Leave the global font state as we found it -- the next frame's emit/layout depends on it. */
    if ( cur_font != saved_font )
        font_use( saved_font );
}

// clang-format on
/*============================================================================================*/
