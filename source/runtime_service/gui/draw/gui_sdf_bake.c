/*==============================================================================================

    runtime_service/gui/draw/gui_sdf_bake.c -- Coverage bitmap -> signed distance field.

    The converter both distance-field tenants go through: SDF icons (gui_icon.c) and baked SHAPES
    (gui_shape.c).  Everything else they need already existed -- the SDF atlas is a generic tenant
    packer (res_sdf_add) and the fragment resolves coverage from a field -- but art arrives as
    COVERAGE, alpha from a PNG or a rasterizer, and a coverage byte is not a distance.  This file is
    that conversion, plus the PADDING policy the field's reach depends on.  Pure functions on a
    bitmap: no atlas, no registry, no GPU.

    Why a distance field is worth it for authored art.  A coverage atlas must be sampled NEAREST (a
    filtered glyph atlas stops being crisp), so art drawn at any size other than the one it was
    baked at is either aliased or blurry, and it cannot rotate at all.  A field is resolution
    independent -- the fragment takes fwidth() of it, so the antialiasing band is one pixel wide at
    any scale -- and it costs the vertex format nothing, because the sampling model already travels
    in the record's tex word.

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

    THE SPREAD IS THE REACH.  A stored byte holds 127 levels either side of the outline, so the
    spread trades effect reach against quantization: 4 texels leaves ~32 levels across the one-pixel
    antialiasing band, 8 leaves ~16, and 16 would leave 8 and band visibly.  Nothing past the spread
    exists in the field at all -- it saturates -- so a border, a glow or a swell that asks to travel
    further simply stops.  That is why a shape bake pads DELIBERATELY (sdf_bake_shape below) rather
    than hoping the source art brought a margin.

    Included by gui_draw.c ahead of gui_icon.c and gui_shape.c, its two callers.

==============================================================================================*/
// clang-format off

/* Farther than any source dimension, and small enough that its square doubled still fits i32
   (9999^2 * 2 is about 2.0e8). */
#define SDF_BAKE_INF   9999

/* Offset from a pixel to the nearest seed pixel.  The transform propagates these rather than
   distances, which is what makes the result exactly euclidean: adding a step to a VECTOR is exact
   where adding one to a scalar distance is the chamfer approximation. */
typedef struct
{
    i16 dx, dy;   // offset to the nearest seed (0,0 at a seed; INF,INF before propagation)

} sdf_bake_pt_t;

static inline i32
sdf_bake_d2( sdf_bake_pt_t p )
{
    return (i32)p.dx * (i32)p.dx + (i32)p.dy * (i32)p.dy;
}

/* Relax (x,y) against the neighbour at (+ox,+oy): if that neighbour's seed is closer to US once
   the step between us is added, adopt it. */
static void
sdf_bake_compare( sdf_bake_pt_t* g, u32 w, u32 h, i32 x, i32 y, i32 ox, i32 oy )
{
    i32 nx = x + ox, ny = y + oy;
    if ( nx < 0 || ny < 0 || nx >= (i32)w || ny >= (i32)h )
        return;

    sdf_bake_pt_t o = g[ (u32)ny * w + (u32)nx ];
    o.dx = (i16)( o.dx + ox );
    o.dy = (i16)( o.dy + oy );
    if ( sdf_bake_d2( o ) < sdf_bake_d2( g[ (u32)y * w + (u32)x ] ) )
        g[ (u32)y * w + (u32)x ] = o;
}

/* The two sweeps.  Forward visits the four already-settled neighbours above and left, then a
   left-to-right pass along the row; backward mirrors both.  Two sweeps is enough for the 8-point
   form -- every pixel's shortest path to a seed is monotone in one of the two directions. */
static void
sdf_bake_propagate( sdf_bake_pt_t* g, u32 w, u32 h )
{
    for ( i32 y = 0; y < (i32)h; ++y )
    {
        for ( i32 x = 0; x < (i32)w; ++x )
        {
            sdf_bake_compare( g, w, h, x, y, -1,  0 );
            sdf_bake_compare( g, w, h, x, y,  0, -1 );
            sdf_bake_compare( g, w, h, x, y, -1, -1 );
            sdf_bake_compare( g, w, h, x, y,  1, -1 );
        }
        for ( i32 x = (i32)w - 1; x >= 0; --x )
            sdf_bake_compare( g, w, h, x, y, 1, 0 );
    }
    for ( i32 y = (i32)h - 1; y >= 0; --y )
    {
        for ( i32 x = (i32)w - 1; x >= 0; --x )
        {
            sdf_bake_compare( g, w, h, x, y,  1, 0 );
            sdf_bake_compare( g, w, h, x, y,  0, 1 );
            sdf_bake_compare( g, w, h, x, y, -1, 1 );
            sdf_bake_compare( g, w, h, x, y,  1, 1 );
        }
        for ( i32 x = 0; x < (i32)w; ++x )
            sdf_bake_compare( g, w, h, x, y, -1, 0 );
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
   spread would mean something different for every bake. */
static void
sdf_bake_encode( const f32* dist, u32 w, u32 h, u8* out, u32 ow, u32 oh, f32 spread )
{
    f32 sx    = (f32)w / (f32)ow;
    f32 sy    = (f32)h / (f32)oh;
    f32 scale = 0.5f * ( sx + sy );          /* source px per stored texel */
    f32 enc   = 127.0f / ( ( spread > 0.0f ) ? spread : 1.0f );

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
               atlas uses, because the fragment tests one threshold for both (gui_fx.hlsli). */
            f32 d = ( n ? acc / (f32)n : 0.0f ) / scale;
            i32 v = (i32)( 128.0f + d * enc + 0.5f );
            if ( v < 0 )   v = 0;
            if ( v > 255 ) v = 255;
            out[ y * ow + x ] = (u8)v;
        }
    }
}

/*==============================================================================================
    sdf_bake_build -- coverage (w x h) -> distance field (ow x oh) at `spread` texels.  `out` is
    caller-owned and must hold ow * oh bytes.  Returns false only on allocation failure.

    Coverage is thresholded at 128 rather than read as a partial area, and that is not a shortcut
    being papered over: the source is meant to be several times the stored size, so a threshold
    error is a fraction of a STORED texel, and the box filter below averages it away.  Reading
    partial coverage as a sub-pixel edge offset would matter if the transform ran at display
    resolution -- which is exactly the case this design tells callers not to be in.
==============================================================================================*/

static bool
sdf_bake_build( const u8* cov, u32 w, u32 h, u8* out, u32 ow, u32 oh, f32 spread )
{
    u32            n    = w * h;
    sdf_bake_pt_t* gin  = (sdf_bake_pt_t*)malloc( n * sizeof( sdf_bake_pt_t ) );
    sdf_bake_pt_t* gout = (sdf_bake_pt_t*)malloc( n * sizeof( sdf_bake_pt_t ) );
    f32*           dist = (f32*)malloc( n * sizeof( f32 ) );

    if ( !gin || !gout || !dist )
    {
        free( gin );
        free( gout );
        free( dist );
        return false;
    }

    const sdf_bake_pt_t zero = { 0, 0 };
    const sdf_bake_pt_t far  = { SDF_BAKE_INF, SDF_BAKE_INF };

    for ( u32 i = 0; i < n; ++i )
    {
        bool inside = ( cov[ i ] >= 128u );
        gin [ i ] = inside ? zero : far;    /* seeded at the inside pixels  */
        gout[ i ] = inside ? far  : zero;   /* seeded at the outside pixels */
    }

    sdf_bake_propagate( gin,  w, h );
    sdf_bake_propagate( gout, w, h );

    /* Positive inside: deep inside, the distance to an OUTSIDE pixel is large and the distance to
       an inside one is zero.

       The half subtracted from each side is not a fudge.  The transform measures pixel CENTRE to
       pixel CENTRE, but the boundary the field is supposed to describe runs BETWEEN two centres --
       so the raw result overstates the magnitude by about half a pixel on both sides at once.  The
       zero crossing stays exactly right either way (both sides are overstated equally, so they
       still meet where they should), which is why this is invisible in the shape and shows up
       instead as a KINK in the gradient across the edge pixel: the field falls twice as fast there
       as it should.  fwidth() hides even that, since it normalizes by the local slope -- but the
       kink would make the spread a lie, and an outline authored as 2 px would come out 1.
       Clamped at zero because a seed pixel measures 0 to its own region and must stay there. */
    for ( u32 i = 0; i < n; ++i )
    {
        f32 dout = sqrtf( (f32)sdf_bake_d2( gout[ i ] ) ) - 0.5f;
        f32 din  = sqrtf( (f32)sdf_bake_d2( gin [ i ] ) ) - 0.5f;
        if ( dout < 0.0f ) dout = 0.0f;
        if ( din  < 0.0f ) din  = 0.0f;
        dist[ i ] = dout - din;
    }

    sdf_bake_encode( dist, w, h, out, ow, oh, spread );

    free( gin );
    free( gout );
    free( dist );
    return true;
}

/*==============================================================================================
    sdf_bake_touches_border -- does the shape run off the edge of its own bitmap?

    Worth a diagnostic because the failure is silent and looks like a shader bug.  The transform
    only knows the pixels it is given, so a shape reaching the bitmap edge has no OUTSIDE there for
    the field to fall off into: it stays saturated to the border, and the art renders with a hard
    machine-straight cut instead of an antialiased edge -- and an outline drawn on it stops dead at
    the same line.  An SDF glyph never hits this because FreeType pads every glyph by the spread;
    icon art has to bring its own transparent margin, and a SHAPE bake makes the margin itself
    (sdf_bake_shape).
==============================================================================================*/

static bool
sdf_bake_touches_border( const u8* cov, u32 w, u32 h )
{
    for ( u32 x = 0; x < w; ++x )
        if ( cov[ x ] >= 128u || cov[ ( h - 1 ) * w + x ] >= 128u )
            return true;
    for ( u32 y = 0; y < h; ++y )
        if ( cov[ y * w ] >= 128u || cov[ y * w + ( w - 1 ) ] >= 128u )
            return true;
    return false;
}

/*==============================================================================================
    THE SHAPE BAKE -- one field plus the geometry that describes where its ink sits.

    A shape's effects are measured in the field, so the bake owes two things a plain icon bake does
    not: a KNOWN margin (the reach every border, glow and swell is bounded by) and the ink's box
    inside the padded tenant (what the draw verb maps the caller's rect onto).

    All three padding policies run one path -- crop to the ink, add margin, downsample -- and differ
    only in what `out_max` names:

        GROW   out_max is the INK's longest edge; the tenant comes out 2 * spread larger
        INSET  out_max is the TENANT's longest edge; the ink shrinks to make room inside it
        KEEP   out_max is the TENANT's longest edge; the source is used uncropped and the margin
               it already carries is MEASURED rather than made

    KEEP is the import path for art that was authored with spacing.  Its margin may be short, and
    then the effective spread is the margin -- reported back so shape_reach cannot overstate what
    the field actually holds.
==============================================================================================*/

/* Where the ink sits in a coverage bitmap, thresholded like the transform.  False = no ink at all,
   which is not a shape and the caller rejects. */
static bool
sdf_bake_ink_bounds( const u8* cov, u32 w, u32 h, u32* bx0, u32* by0, u32* bx1, u32* by1 )
{
    u32 x0 = w, y0 = h, x1 = 0, y1 = 0;

    for ( u32 y = 0; y < h; ++y )
        for ( u32 x = 0; x < w; ++x )
            if ( cov[ y * w + x ] >= 128u )
            {
                if ( x < x0 ) x0 = x;
                if ( y < y0 ) y0 = y;
                if ( x > x1 ) x1 = x;
                if ( y > y1 ) y1 = y;
            }

    if ( x0 > x1 || y0 > y1 )
        return false;

    *bx0 = x0;  *by0 = y0;
    *bx1 = x1 + 1u;  *by1 = y1 + 1u;   /* half-open, so the box is (x1 - x0) wide */
    return true;
}

/* What a shape bake produced: the field to pack, its size, and where the ink landed in it. */
typedef struct
{
    u8* field;              // malloc'd ow * oh bytes; the caller frees
    u32 full_w, full_h;     // stored tenant size, texels
    u32 ink_x, ink_y;       // the art's box within the tenant, texels
    u32 ink_w, ink_h;
    f32 spread;             // texels of field either side of the outline the tenant ACTUALLY holds

} sdf_bake_out_t;

static bool
sdf_bake_shape( const u8* cov, u32 w, u32 h, u32 out_max, f32 spread, gui_sdf_pad_t policy,
                sdf_bake_out_t* out )
{
    u32 bx0, by0, bx1, by1;
    if ( !sdf_bake_ink_bounds( cov, w, h, &bx0, &by0, &bx1, &by1 ) )
        return false;

    memset( out, 0, sizeof( *out ) );
    u32 pad = (u32)( spread + 0.999f );          /* whole texels of margin the field needs */

    if ( policy == GUI_SDF_PAD_KEEP )
    {
        /* The source is the canvas.  Fit it whole to out_max and measure what margin it brought;
           the field cannot be wider than that however much was asked for. */
        u32 longest = ( w > h ) ? w : h;
        f32 s       = (f32)out_max / (f32)longest;

        out->full_w = (u32)( (f32)w * s + 0.5f );
        out->full_h = (u32)( (f32)h * s + 0.5f );
        if ( out->full_w == 0 ) out->full_w = 1;
        if ( out->full_h == 0 ) out->full_h = 1;

        out->ink_x = (u32)( (f32)bx0 * s );
        out->ink_y = (u32)( (f32)by0 * s );
        out->ink_w = (u32)( (f32)( bx1 - bx0 ) * s + 0.5f );
        out->ink_h = (u32)( (f32)( by1 - by0 ) * s + 0.5f );
        if ( out->ink_w == 0 ) out->ink_w = 1;
        if ( out->ink_h == 0 ) out->ink_h = 1;

        /* The margin is the tightest of the four sides -- a field is only as deep as its shallowest
           approach, and an effect reaching past it saturates on that side alone, which reads as the
           halo being clipped square. */
        u32 ml = out->ink_x;
        u32 mt = out->ink_y;
        u32 mr = ( out->full_w > out->ink_x + out->ink_w ) ? out->full_w - out->ink_x - out->ink_w : 0u;
        u32 mb = ( out->full_h > out->ink_y + out->ink_h ) ? out->full_h - out->ink_y - out->ink_h : 0u;
        u32 m  = ml;
        if ( mt < m ) m = mt;
        if ( mr < m ) m = mr;
        if ( mb < m ) m = mb;

        out->spread = ( (f32)m < spread ) ? (f32)m : spread;

        out->field = (u8*)malloc( (size_t)out->full_w * out->full_h );
        if ( !out->field )
            return false;
        if ( !sdf_bake_build( cov, w, h, out->field, out->full_w, out->full_h,
                              ( out->spread > 0.0f ) ? out->spread : 1.0f ) )
        {
            free( out->field );
            out->field = NULL;
            return false;
        }
        return true;
    }

    /* GROW and INSET both crop to the ink and build the margin.  Only the size they solve for
       differs, so that is the only branch. */
    u32 iw = bx1 - bx0, ih = by1 - by0;
    u32 ilong = ( iw > ih ) ? iw : ih;

    u32 ink_long_tex;
    if ( policy == GUI_SDF_PAD_INSET )
    {
        /* The tenant is pinned; the art gives up the room.  Two texels of ink is the floor at which
           a shape is still a shape -- below it the pad has eaten the art and the caller wants a
           bigger out_max, not a silently invisible tenant. */
        ink_long_tex = ( out_max > 2u * pad + 2u ) ? out_max - 2u * pad : 2u;
    }
    else
    {
        ink_long_tex = out_max;
    }
    if ( ink_long_tex > ilong )
        ink_long_tex = ilong;              /* upsampling invents no detail, it only spends atlas */
    if ( ink_long_tex == 0 )
        ink_long_tex = 1;

    f32 s = (f32)ink_long_tex / (f32)ilong;      /* stored texels per source pixel */

    out->ink_w = (u32)( (f32)iw * s + 0.5f );
    out->ink_h = (u32)( (f32)ih * s + 0.5f );
    if ( out->ink_w == 0 ) out->ink_w = 1;
    if ( out->ink_h == 0 ) out->ink_h = 1;

    out->ink_x  = pad;
    out->ink_y  = pad;
    out->full_w = out->ink_w + 2u * pad;
    out->full_h = out->ink_h + 2u * pad;
    out->spread = spread;

    /* The padded SOURCE canvas: the ink box copied into the middle of a transparent border whose
       width is the margin measured back in source pixels.  Building it for real is what gives the
       transform an outside to fall off into -- the margin has to exist before the sweep runs, not
       after it. */
    u32 pad_src = (u32)( (f32)pad / ( ( s > 0.0f ) ? s : 1.0f ) + 0.5f );
    u32 cw      = iw + 2u * pad_src;
    u32 ch      = ih + 2u * pad_src;

    u8* canvas = (u8*)calloc( (size_t)cw * ch, 1u );
    if ( !canvas )
        return false;
    for ( u32 y = 0; y < ih; ++y )
        memcpy( canvas + (size_t)( y + pad_src ) * cw + pad_src,
                cov + (size_t)( by0 + y ) * w + bx0, iw );

    out->field = (u8*)malloc( (size_t)out->full_w * out->full_h );
    if ( !out->field )
    {
        free( canvas );
        return false;
    }

    bool ok = sdf_bake_build( canvas, cw, ch, out->field, out->full_w, out->full_h, spread );
    free( canvas );
    if ( !ok )
    {
        free( out->field );
        out->field = NULL;
        return false;
    }
    return true;
}

// clang-format on
/*============================================================================================*/
