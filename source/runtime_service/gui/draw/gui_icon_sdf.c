/*==============================================================================================

    runtime_service/gui/draw/gui_icon_sdf.c -- Coverage bitmap -> signed distance field.

    The one piece of NEW math the icon SDF fork needed.  Everything else it rides on already
    existed: the SDF atlas is a generic tenant packer (res_sdf_add), the fragment already recovers
    coverage from a distance field, and GUI_FX_TEXT_EDGE already paints an outline from one.  What
    was missing is that icons arrive as COVERAGE -- alpha from a PNG -- and a coverage byte is not
    a distance.  This file is the converter, and it is a pure function on a bitmap: no atlas, no
    registry, no GPU.

    Why a distance field is worth it for an icon.  A coverage atlas must be sampled NEAREST (a
    filtered glyph atlas stops being crisp), so an icon drawn at any size other than the one it was
    baked at is either aliased or blurry, and it cannot rotate at all.  A field is resolution
    independent -- the fragment takes fwidth() of it, so the antialiasing band is one pixel wide at
    any scale -- and it costs the vertex format nothing, because the sampling model already travels
    in the vertex's tex word.

    The algorithm is 8SSEDT (8-point sequential signed euclidean distance transform): two offset
    grids, one seeded at the inside pixels and one at the outside, each swept forward and backward
    with an 8-neighbour compare.  Two passes, no queue, and the result is euclidean rather than the
    chamfer approximation a cheaper transform would give -- which matters here because the error
    would show up as a wobbling outline, not as noise.

    THE SOURCE SHOULD BE BIGGER THAN THE FIELD.  A transform of a 16 px bitmap knows the shape to
    16 px, and storing that as a field buys nothing: the field is smooth but the SHAPE inside it is
    still a 16 px staircase.  So the caller hands in art several times the display size, the
    transform runs at that resolution, and the FIELD is box-filtered down -- averaging distances,
    not coverage, which is what keeps the sub-pixel edge position the high-res art knew.

    Included by gui_draw.c before gui_icon.c, whose icon_register_sdf is its only caller.

==============================================================================================*/
// clang-format off

/* The stored field's half-range, in TEXELS of the stored (downsampled) field: byte 128 is on the
   outline, 255 is this far inside, 1 this far outside.  It bounds two things and neither is the
   antialiasing -- the fragment recovers that from fwidth() and does not care what the spread is.
   What it bounds is (a) how wide a GUI_FX_TEXT_EDGE outline can grow before the field saturates
   and the outline stops growing, and (b) the quantization: 127 levels over the spread, so 4 px
   gives ~32 levels across the one-pixel AA band.  Widening it to 16 would leave 8, which bands
   visibly.  Four is the balance. */
#define ICON_SDF_SPREAD        4.0f

/* Longest edge of the stored field when the caller does not name one.  Large enough that a toolbar
   icon never magnifies past its own texel grid, small enough that a full set fits the SDF atlas
   (1024x512): 64x64 is 4 KB, so ~128 icons share the page with the fonts. */
#define ICON_SDF_SIZE_DEFAULT  64u

/* Farther than any icon dimension, and small enough that its square doubled still fits i32
   (9999^2 * 2 is about 2.0e8). */
#define ICON_SDF_INF           9999

/* Offset from a pixel to the nearest seed pixel.  The transform propagates these rather than
   distances, which is what makes the result exactly euclidean: adding a step to a VECTOR is exact
   where adding one to a scalar distance is the chamfer approximation. */
typedef struct
{
    i16 dx, dy;   // offset to the nearest seed (0,0 at a seed; INF,INF before propagation)

} icon_sdf_pt_t;

static inline i32
icon_sdf_d2( icon_sdf_pt_t p )
{
    return (i32)p.dx * (i32)p.dx + (i32)p.dy * (i32)p.dy;
}

/* Relax (x,y) against the neighbour at (+ox,+oy): if that neighbour's seed is closer to US once
   the step between us is added, adopt it. */
static void
icon_sdf_compare( icon_sdf_pt_t* g, u32 w, u32 h, i32 x, i32 y, i32 ox, i32 oy )
{
    i32 nx = x + ox, ny = y + oy;
    if ( nx < 0 || ny < 0 || nx >= (i32)w || ny >= (i32)h )
        return;

    icon_sdf_pt_t o = g[ (u32)ny * w + (u32)nx ];
    o.dx = (i16)( o.dx + ox );
    o.dy = (i16)( o.dy + oy );
    if ( icon_sdf_d2( o ) < icon_sdf_d2( g[ (u32)y * w + (u32)x ] ) )
        g[ (u32)y * w + (u32)x ] = o;
}

/* The two sweeps.  Forward visits the four already-settled neighbours above and left, then a
   left-to-right pass along the row; backward mirrors both.  Two sweeps is enough for the 8-point
   form -- every pixel's shortest path to a seed is monotone in one of the two directions. */
static void
icon_sdf_propagate( icon_sdf_pt_t* g, u32 w, u32 h )
{
    for ( i32 y = 0; y < (i32)h; ++y )
    {
        for ( i32 x = 0; x < (i32)w; ++x )
        {
            icon_sdf_compare( g, w, h, x, y, -1,  0 );
            icon_sdf_compare( g, w, h, x, y,  0, -1 );
            icon_sdf_compare( g, w, h, x, y, -1, -1 );
            icon_sdf_compare( g, w, h, x, y,  1, -1 );
        }
        for ( i32 x = (i32)w - 1; x >= 0; --x )
            icon_sdf_compare( g, w, h, x, y, 1, 0 );
    }
    for ( i32 y = (i32)h - 1; y >= 0; --y )
    {
        for ( i32 x = (i32)w - 1; x >= 0; --x )
        {
            icon_sdf_compare( g, w, h, x, y,  1, 0 );
            icon_sdf_compare( g, w, h, x, y,  0, 1 );
            icon_sdf_compare( g, w, h, x, y, -1, 1 );
            icon_sdf_compare( g, w, h, x, y,  1, 1 );
        }
        for ( i32 x = 0; x < (i32)w; ++x )
            icon_sdf_compare( g, w, h, x, y, -1, 0 );
    }
}

/* Box-filter the signed distances down to the stored size and encode.  Averaging DISTANCES is the
   whole reason the downsample happens here rather than on the source coverage: the mean of a
   linear field across a block is the field's value at the block centre, so the sub-pixel edge the
   high-res art knew survives into a texel that is far too coarse to have represented it as
   coverage.  Downsampling coverage first and transforming after would have thrown that away before
   the transform ever saw it.
   `scale` converts source pixels to stored texels so the encoded field is in the units the stored
   image is sampled at -- without it a 4x source would encode a field four times too steep, and the
   spread would mean something different for every icon. */
static void
icon_sdf_encode( const f32* dist, u32 w, u32 h, u8* out, u32 ow, u32 oh )
{
    f32 sx    = (f32)w / (f32)ow;
    f32 sy    = (f32)h / (f32)oh;
    f32 scale = 0.5f * ( sx + sy );          /* source px per stored texel */

    for ( u32 y = 0; y < oh; ++y )
    {
        u32 y0 = (u32)( (f32)y * sy );
        u32 y1 = (u32)( (f32)( y + 1 ) * sy );
        if ( y1 <= y0 ) y1 = y0 + 1;
        if ( y1 > h )   y1 = h;

        for ( u32 x = 0; x < ow; ++x )
        {
            u32 x0 = (u32)( (f32)x * sx );
            u32 x1 = (u32)( (f32)( x + 1 ) * sx );
            if ( x1 <= x0 ) x1 = x0 + 1;
            if ( x1 > w )   x1 = w;

            f32 acc = 0.0f;
            u32 n   = 0;
            for ( u32 yy = y0; yy < y1; ++yy )
                for ( u32 xx = x0; xx < x1; ++xx )
                {
                    acc += dist[ yy * w + xx ];
                    ++n;
                }

            /* 128 is ON the outline and positive is INSIDE -- the same convention the SDF font
               atlas uses, because the fragment tests one threshold for both (gui.ps.hlsl). */
            f32 d = ( n ? acc / (f32)n : 0.0f ) / scale;
            i32 v = (i32)( 128.0f + d * ( 127.0f / ICON_SDF_SPREAD ) + 0.5f );
            if ( v < 0 )   v = 0;
            if ( v > 255 ) v = 255;
            out[ y * ow + x ] = (u8)v;
        }
    }
}

/*==============================================================================================
    icon_sdf_build -- coverage (w x h) -> distance field (ow x oh).  `out` is caller-owned and
    must hold ow * oh bytes.  Returns false only on allocation failure.

    Coverage is thresholded at 128 rather than read as a partial area, and that is not a shortcut
    being papered over: the source is meant to be several times the stored size, so a threshold
    error is a fraction of a STORED texel, and the box filter below averages it away.  Reading
    partial coverage as a sub-pixel edge offset would matter if the transform ran at display
    resolution -- which is exactly the case this design tells callers not to be in.
==============================================================================================*/

static bool
icon_sdf_build( const u8* cov, u32 w, u32 h, u8* out, u32 ow, u32 oh )
{
    u32            n    = w * h;
    icon_sdf_pt_t* gin  = (icon_sdf_pt_t*)malloc( n * sizeof( icon_sdf_pt_t ) );
    icon_sdf_pt_t* gout = (icon_sdf_pt_t*)malloc( n * sizeof( icon_sdf_pt_t ) );
    f32*           dist = (f32*)malloc( n * sizeof( f32 ) );

    if ( !gin || !gout || !dist )
    {
        free( gin );
        free( gout );
        free( dist );
        return false;
    }

    const icon_sdf_pt_t zero = { 0, 0 };
    const icon_sdf_pt_t far  = { ICON_SDF_INF, ICON_SDF_INF };

    for ( u32 i = 0; i < n; ++i )
    {
        bool inside = ( cov[ i ] >= 128u );
        gin [ i ] = inside ? zero : far;    /* seeded at the inside pixels  */
        gout[ i ] = inside ? far  : zero;   /* seeded at the outside pixels */
    }

    icon_sdf_propagate( gin,  w, h );
    icon_sdf_propagate( gout, w, h );

    /* Positive inside: deep inside, the distance to an OUTSIDE pixel is large and the distance to
       an inside one is zero.

       The half subtracted from each side is not a fudge.  The transform measures pixel CENTRE to
       pixel CENTRE, but the boundary the field is supposed to describe runs BETWEEN two centres --
       so the raw result overstates the magnitude by about half a pixel on both sides at once.  The
       zero crossing stays exactly right either way (both sides are overstated equally, so they
       still meet where they should), which is why this is invisible in the shape and shows up
       instead as a KINK in the gradient across the edge pixel: the field falls twice as fast there
       as it should.  fwidth() hides even that, since it normalizes by the local slope -- but the
       kink would make ICON_SDF_SPREAD a lie, and an outline authored as 2 px would come out 1.
       Clamped at zero because a seed pixel measures 0 to its own region and must stay there. */
    for ( u32 i = 0; i < n; ++i )
    {
        f32 dout = sqrtf( (f32)icon_sdf_d2( gout[ i ] ) ) - 0.5f;
        f32 din  = sqrtf( (f32)icon_sdf_d2( gin [ i ] ) ) - 0.5f;
        if ( dout < 0.0f ) dout = 0.0f;
        if ( din  < 0.0f ) din  = 0.0f;
        dist[ i ] = dout - din;
    }

    icon_sdf_encode( dist, w, h, out, ow, oh );

    free( gin );
    free( gout );
    free( dist );
    return true;
}

/*==============================================================================================
    icon_sdf_touches_border -- does the shape run off the edge of its own bitmap?

    Worth a diagnostic because the failure is silent and looks like a shader bug.  The transform
    only knows the pixels it is given, so a shape reaching the bitmap edge has no OUTSIDE there for
    the field to fall off into: it stays saturated to the border, and the icon renders with a hard
    machine-straight cut instead of an antialiased edge -- and an outline drawn on it stops dead at
    the same line.  An SDF glyph never hits this because FreeType pads every glyph by the spread;
    icon art has to bring its own transparent margin.
==============================================================================================*/

static bool
icon_sdf_touches_border( const u8* cov, u32 w, u32 h )
{
    for ( u32 x = 0; x < w; ++x )
        if ( cov[ x ] >= 128u || cov[ ( h - 1 ) * w + x ] >= 128u )
            return true;
    for ( u32 y = 0; y < h; ++y )
        if ( cov[ y * w ] >= 128u || cov[ y * w + ( w - 1 ) ] >= 128u )
            return true;
    return false;
}

// clang-format on
/*============================================================================================*/
