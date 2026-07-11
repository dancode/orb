/*==============================================================================================

    base/math_rng.c -- Composite random sampling built on the PCG32 core in math_rng.h.

==============================================================================================*/

/*==============================================================================================
    Gaussian
==============================================================================================*/

f32
rng_gauss_f32( rng_t* r )
{
    if ( r->gauss_ready )
    {
        r->gauss_ready = false;
        return r->gauss_spare;
    }

    // Marsaglia polar method: draw pairs in the unit disk, transform both.
    f32 u, v, s;
    do
    {
        u = rng_f32( r ) * 2.0f - 1.0f;
        v = rng_f32( r ) * 2.0f - 1.0f;
        s = u * u + v * v;
    }
    while ( s >= 1.0f || s == 0.0f );

    f32 k = f32_sqrt( -2.0f * logf( s ) / s );

    r->gauss_spare = v * k;
    r->gauss_ready = true;
    return u * k;
}

/*==============================================================================================
    Directions and volumes
==============================================================================================*/

void
rng_unit2( rng_t* r, f32* x, f32* y )
{
    f32 a = rng_angle( r );
    *x    = f32_cos( a );
    *y    = f32_sin( a );
}

void
rng_unit3( rng_t* r, f32* x, f32* y, f32* z )
{
    // Marsaglia: uniform z plus uniform azimuth is area-uniform on the sphere.
    f32 zz = rng_f32( r ) * 2.0f - 1.0f;
    f32 a  = rng_angle( r );
    f32 s  = f32_sqrt( 1.0f - zz * zz );
    *x     = s * f32_cos( a );
    *y     = s * f32_sin( a );
    *z     = zz;
}

void
rng_in_disk( rng_t* r, f32* x, f32* y )
{
    // sqrt of a uniform radius keeps density uniform over area.
    f32 rad = f32_sqrt( rng_f32( r ) );
    f32 a   = rng_angle( r );
    *x      = rad * f32_cos( a );
    *y      = rad * f32_sin( a );
}

void
rng_in_sphere( rng_t* r, f32* x, f32* y, f32* z )
{
    // Cube rejection: ~1.9 draws on average, no cube root.
    f32 px, py, pz;
    do
    {
        px = rng_f32( r ) * 2.0f - 1.0f;
        py = rng_f32( r ) * 2.0f - 1.0f;
        pz = rng_f32( r ) * 2.0f - 1.0f;
    }
    while ( px * px + py * py + pz * pz > 1.0f );
    *x = px;
    *y = py;
    *z = pz;
}

/*==============================================================================================
    Collections
==============================================================================================*/

void
rng_shuffle( rng_t* r, void* items, u32 count, usize stride )
{
    if ( count < 2 ) return;
    u8* base = ( u8* )items;
    for ( u32 i = count - 1; i > 0; --i )
    {
        u32 j = rng_below( r, i + 1 );
        if ( j != i ) mem_swap( base + ( usize )i * stride, base + ( usize )j * stride, stride );
    }
}

u32
rng_weighted( rng_t* r, const f32* weights, u32 count )
{
    f32 total = 0.0f;
    for ( u32 i = 0; i < count; ++i )
    {
        if ( weights[ i ] > 0.0f ) total += weights[ i ];
    }
    if ( total <= 0.0f ) return 0;

    f32 t = rng_f32( r ) * total;
    for ( u32 i = 0; i < count; ++i )
    {
        if ( weights[ i ] <= 0.0f ) continue;
        t -= weights[ i ];
        if ( t < 0.0f ) return i;
    }
    return count - 1;    // float roundoff fallback: last positive-weight bucket is close enough
}

/*============================================================================================*/
