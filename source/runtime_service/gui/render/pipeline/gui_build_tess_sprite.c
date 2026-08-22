/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_build_tess_sprite.c -- sprites and hollow rects.

    Part 3 of 7 of the CPU-side quad-record builder (see gui_build_tess_state.c for the family
    overview).  Holds the nine-slice sprite expansion (tess_sprite, tess_sprite_piece) built on
    tess_rect_filled, and tess_rect_outline -- the four-edge-quad hollow rect.

    Included right after gui_build_tess_quad.c (needs tess_rect_filled, tess_snap_px).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Sprites and the nine-slice expansion

    ONE semantic command becomes 1, 3 or 9 quads here.  The expansion lives at tessellation time
    rather than in the emit layer for two reasons, and they are the same reason twice: only the
    registry knows the source's pixel size and slice insets, and only tessellation runs late enough
    that a sprite-atlas repack (which moves UVs) is already accounted for.  Emitting quads early
    would bake both facts into a command that outlives them.

    The pieces come out of ONE atlas with ONE bindless slot, so a whole nine-slice frame is a single
    GPU batch -- which is what makes an authored border affordable on every panel rather than a
    special occasion.
==============================================================================================*/

/* Repeat cap per axis for a TILEd piece.  Past this the piece stretches instead: a pathological
   pitch (art authored 1px wide, or a scale near zero) would otherwise turn one command into tens
   of thousands of quads, and a stretched fallback is wrong in a way you can see and fix, where an
   exhausted vertex budget is wrong in a way that takes the rest of the frame down with it. */
#define TESS_SPRITE_TILE_MAX  64u

/* One piece of the grid.  pitch <= 0 on an axis means "stretch across that axis" (the default);
   a positive pitch repeats the piece at authored size, trimming the trailing repeat to fit by
   cutting its UV in the same proportion.  UVs may arrive reversed (u0 > u1) -- that is how a flip
   is expressed -- and every interpolation here is a lerp from u0 toward u1, so reversal carries
   through the trim untouched. */
static void
tess_sprite_piece( f32 x, f32 y, f32 w, f32 h,
                   f32 u0, f32 v0, f32 u1, f32 v1,
                   f32 pitch_x, f32 pitch_y, u32 tex_idx, u32 abgr )
{
    if ( w <= 0.0f || h <= 0.0f )
        return;

    u32 nx = 1, ny = 1;
    if ( pitch_x > 0.5f )
    {
        f32 n = ceilf( w / pitch_x );
        if ( n < 1.0f || n > (f32)TESS_SPRITE_TILE_MAX ) pitch_x = 0.0f;   /* stretch instead */
        else                                             nx = (u32)n;
    }
    if ( pitch_y > 0.5f )
    {
        f32 n = ceilf( h / pitch_y );
        if ( n < 1.0f || n > (f32)TESS_SPRITE_TILE_MAX ) pitch_y = 0.0f;
        else                                             ny = (u32)n;
    }

    for ( u32 j = 0; j < ny; ++j )
    {
        f32 py = ( pitch_y > 0.0f ) ? y + (f32)j * pitch_y : y;
        f32 ph = ( pitch_y > 0.0f ) ? pitch_y : h;
        if ( py + ph > y + h ) ph = y + h - py;          /* trailing repeat: cut to the piece */
        f32 fv = ( pitch_y > 0.0f ) ? ph / pitch_y : 1.0f;
        f32 pv1 = v0 + ( v1 - v0 ) * fv;

        for ( u32 i = 0; i < nx; ++i )
        {
            f32 px = ( pitch_x > 0.0f ) ? x + (f32)i * pitch_x : x;
            f32 pw = ( pitch_x > 0.0f ) ? pitch_x : w;
            if ( px + pw > x + w ) pw = x + w - px;
            f32 fu = ( pitch_x > 0.0f ) ? pw / pitch_x : 1.0f;
            f32 pu1 = u0 + ( u1 - u0 ) * fu;

            tess_rect_filled( px, py, pw, ph, u0, v0, pu1, pv1, tex_idx, abgr );
        }
    }
}

/* Tessellate one sprite command.  Resolves the sprite through the source contract, then either
   stretches it over the rect as a single quad or lays the nine-slice grid.

   Slice insets are authored in SOURCE pixels and scaled by the command's `scale`, so one piece of
   art serves several UI scales; when the scaled insets no longer fit the destination they are
   shrunk proportionally rather than allowed to overlap, which is what keeps a frame legible when
   its window is dragged smaller than its own corners. */
static void
tess_sprite( const gui_cmd_ext_t* e )
{
    u32 tex = res_sprite_idx();
    if ( tex == 0 )
        return;                       /* no sprite atlas yet -- nothing was ever registered */

    f32       u0, v0, u1, v1;
    u32       sw = 0, sh = 0;
    gui_pad_t sl = { 0 };
    if ( !sprite_get( e->sprite.sprite, &u0, &v0, &u1, &v1, &sw, &sh, &sl ) || sw == 0 || sh == 0 )
        return;

    tex |= GUI_TEX_MODE( GUI_TEX_RGBA );   /* the texel IS the colour; vertex colour tints it */

    const u32 flags  = e->sprite.flags;
    const bool flipx = ( flags & GUI_BRUSH_FLIP_X ) != 0;
    const bool flipy = ( flags & GUI_BRUSH_FLIP_Y ) != 0;
    const bool tile  = ( flags & GUI_BRUSH_TILE   ) != 0;

    const f32 x = e->sprite.x, y = e->sprite.y, w = e->sprite.w, h = e->sprite.h;
    const f32 s = e->sprite.scale;
    const u32 col = e->sprite.abgr;

    /* Scaled slice insets.  A sprite with none (or a command that did not ask for the expansion)
       is one stretched quad -- the flip is then just a reversed UV span. */
    f32 L = sl.l * s, R = sl.r * s, T = sl.t * s, B = sl.b * s;
    if ( !e->sprite.nine || ( L <= 0.0f && R <= 0.0f && T <= 0.0f && B <= 0.0f ) )
    {
        tess_rect_filled( x, y, w, h,
                          flipx ? u1 : u0, flipy ? v1 : v0,
                          flipx ? u0 : u1, flipy ? v0 : v1, tex, col );
        return;
    }

    /* Shrink insets that no longer fit rather than letting opposite corners overlap. */
    if ( L + R > w && L + R > 0.0f ) { f32 k = w / ( L + R ); L *= k; R *= k; }
    if ( T + B > h && T + B > 0.0f ) { f32 k = h / ( T + B ); T *= k; B *= k; }

    /* A flip mirrors the whole sprite, so the destination edge widths swap with the source columns
       they will sample -- do it here, once, and the grid loop below stays flip-agnostic. */
    if ( flipx ) { f32 t2 = L; L = R; R = t2; }
    if ( flipy ) { f32 t2 = T; T = B; B = t2; }

    /* Destination and source boundaries, three tracks each.  su/sv are in UV; the middle source
       track is the stretchable / tileable span between the insets. */
    const f32 upx = ( u1 - u0 ) / (f32)sw;   /* UV per source pixel */
    const f32 vpx = ( v1 - v0 ) / (f32)sh;

    f32 dx[ 4 ] = { x, x + L, x + w - R, x + w };
    f32 dy[ 4 ] = { y, y + T, y + h - B, y + h };
    f32 su[ 4 ] = { u0, u0 + sl.l * upx, u1 - sl.r * upx, u1 };
    f32 sv[ 4 ] = { v0, v0 + sl.t * vpx, v1 - sl.b * vpx, v1 };

    /* Authored pitch of the middle tracks, for TILE.  The corners never tile (they ARE the fixed
       part); the edges tile along their long axis only, and the centre tiles on both. */
    f32 pitch_x = tile ? ( (f32)sw - sl.l - sl.r ) * s : 0.0f;
    f32 pitch_y = tile ? ( (f32)sh - sl.t - sl.b ) * s : 0.0f;

    for ( u32 j = 0; j < 3; ++j )
        for ( u32 i = 0; i < 3; ++i )
        {
            /* Under a flip the destination track samples the mirrored source track, and the span
               comes out reversed (lo > hi) -- which is exactly the mirrored sampling wanted. */
            u32 si = flipx ? ( 3u - i ) : i;
            u32 sj = flipy ? ( 3u - j ) : j;
            f32 pu0 = su[ si ], pu1 = flipx ? su[ si - 1u ] : su[ si + 1u ];
            f32 pv0 = sv[ sj ], pv1 = flipy ? sv[ sj - 1u ] : sv[ sj + 1u ];

            tess_sprite_piece( dx[ i ], dy[ j ], dx[ i + 1 ] - dx[ i ], dy[ j + 1 ] - dy[ j ],
                               pu0, pv0, pu1, pv1,
                               ( i == 1 ) ? pitch_x : 0.0f,
                               ( j == 1 ) ? pitch_y : 0.0f,
                               tex, col );
        }
}

/* Tessellate a hollow rectangle as four edge quads.

   The frame is snapped to whole pixels ONCE, here, and the four quads are cut from those integer
   edges.  Every fill snaps its own origin (tess_rect_filled) and nothing snaps its extent, so
   handing four unsnapped rects to it rounds four origins independently against four fractional
   far edges: the top rail lands on one row and the side rails start on another, and the right
   rail's inner edge misses the top rail's end.  That reads as pixel gaps at the corners and a
   one-pixel overhang on the right and bottom -- on any fractional origin (a scrolled row, a
   fractional layout position) and worse the thicker the stroke.  Snapped first, the shared edges
   are the same integer on both sides and the joins are exact.

   t is rounded to a whole stroke (never below one pixel, so a hairline cannot vanish) and clamped
   to half the shorter side, so a thick border on a small rect degenerates to a filled rect
   instead of inverted side quads. */
static void
tess_rect_outline( f32 x, f32 y, f32 w, f32 h, f32 t, u32 abgr )
{
    f32 x0 = tess_snap_px( x ),     y0 = tess_snap_px( y );
    f32 x1 = tess_snap_px( x + w ), y1 = tess_snap_px( y + h );
    f32 bw = x1 - x0,               bh = y1 - y0;
    if ( bw <= 0.0f || bh <= 0.0f )
        return;

    f32 tmax = ( bw < bh ? bw : bh ) * 0.5f;
    t = tess_snap_px( t );
    if ( t < 1.0f ) t = 1.0f;
    if ( t > tmax ) t = tmax;

    tess_rect_filled( x0,     y0,     bw, t, 0,0,1,1, 0, abgr );
    tess_rect_filled( x0,     y1 - t, bw, t, 0,0,1,1, 0, abgr );

    f32 mid = bh - 2.0f * t;   /* the span between the rails; zero once t swallowed the box */
    if ( mid > 0.0f )
    {
        tess_rect_filled( x0,     y0 + t, t, mid, 0,0,1,1, 0, abgr );
        tess_rect_filled( x1 - t, y0 + t, t, mid, 0,0,1,1, 0, abgr );
    }
}


// clang-format on
