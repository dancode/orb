/*==============================================================================================

    base/math_rng.h -- Pseudo-random number generation (PCG32).

    Core generator is PCG-XSH-RR 32: 8-byte state + 8-byte stream selector, excellent
    statistical quality, one multiply + shifts per draw. Deterministic across platforms.

    base is stateless -- there is NO global generator. The host owns every rng_t:

        rng_t rng;
        rng_seed( &rng, seed_value );            // e.g. from sys time on startup
        u32 roll = rng_below( &rng, 6 ) + 1;

    For deterministic parallel streams (jobs, entities, net lockstep), seed each consumer
    with the SAME seed but a UNIQUE stream id -- streams never correlate:

        rng_seed_stream( &rng, world_seed, entity_id );

    A zeroed rng_t is NOT valid; always seed before drawing.
    Naming scheme: rng_<operation>. Hot single-draw ops are inline; composite sampling
    (gauss, shuffle, sphere, weighted) lives in math_rng.c.

==============================================================================================*/
#ifndef MATH_RNG_H
#define MATH_RNG_H

// clang-format off
/*==============================================================================================
    State
==============================================================================================*/

typedef struct rng_s
{
    u64 state;          // PCG32 internal state, advances every draw
    u64 inc;            // stream selector, always odd; picked by rng_seed_stream()
    f32 gauss_spare;    // cached second sample from the last gaussian pair
    b32 gauss_ready;    // gauss_spare holds a valid sample
} rng_t;

/*==============================================================================================
    Stateless scramble
==============================================================================================*/

// splitmix64 finalizer: hash an integer into a well-mixed random-looking u64.
// Stateless one-shot randomness (per-entity variation, seed whitening) without an rng_t.
ORB_INLINE u64
rng_scramble_u64( u64 x )
{
    x += 0x9E3779B97F4A7C15ULL;
    x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
    x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBULL;
    return x ^ ( x >> 31 );
}

/*==============================================================================================
    Core draws
==============================================================================================*/

// Next 32 random bits (PCG-XSH-RR).
ORB_INLINE u32
rng_u32( rng_t* r )
{
    u64 old  = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    u32 xsh  = ( u32 )( ( ( old >> 18 ) ^ old ) >> 27 );
    return bit_u32_rotr( xsh, ( i32 )( old >> 59 ) );
}

// Next 64 random bits (two 32-bit draws).
ORB_INLINE u64
rng_u64( rng_t* r )
{
    u64 hi = ( u64 )rng_u32( r );
    return ( hi << 32 ) | ( u64 )rng_u32( r );
}

/*==============================================================================================
    Seeding
==============================================================================================*/

// Seed with an explicit stream id; equal seeds on different streams give unrelated sequences.
ORB_INLINE void
rng_seed_stream( rng_t* r, u64 seed, u64 stream )
{
    r->state = 0;
    r->inc   = ( stream << 1 ) | 1ULL;    // stream selector must be odd
    rng_u32( r );
    r->state += rng_scramble_u64( seed );
    rng_u32( r );
    r->gauss_spare = 0.0f;
    r->gauss_ready = false;
}

// Seed on the default stream.
ORB_INLINE void
rng_seed( rng_t* r, u64 seed )
{
    rng_seed_stream( r, seed, 0 );
}

/*==============================================================================================
    Bounded integers
==============================================================================================*/

// Uniform integer in [0, bound). Unbiased (Lemire multiply-shift with rejection). bound 0 -> 0.
ORB_INLINE u32
rng_below( rng_t* r, u32 bound )
{
    if ( bound == 0 ) return 0;
    u64 m  = ( u64 )rng_u32( r ) * ( u64 )bound;
    u32 lo = ( u32 )m;
    if ( lo < bound )
    {
        u32 t = ( 0u - bound ) % bound;    // reject draws below 2^32 mod bound
        while ( lo < t )
        {
            m  = ( u64 )rng_u32( r ) * ( u64 )bound;
            lo = ( u32 )m;
        }
    }
    return ( u32 )( m >> 32 );
}

// Uniform integer in [lo, hi] inclusive. Requires lo <= hi.
ORB_INLINE i32
rng_range_i32( rng_t* r, i32 lo, i32 hi )
{
    return lo + ( i32 )rng_below( r, ( u32 )( hi - lo ) + 1u );
}

/*==============================================================================================
    Floats
==============================================================================================*/

// Uniform f32 in [0, 1). 24-bit mantissa resolution; never returns 1.0f.
ORB_INLINE f32
rng_f32( rng_t* r )
{
    return ( f32 )( rng_u32( r ) >> 8 ) * ( 1.0f / 16777216.0f );
}

// Uniform f64 in [0, 1). 53-bit mantissa resolution; never returns 1.0.
ORB_INLINE f64
rng_f64( rng_t* r )
{
    return ( f64 )( rng_u64( r ) >> 11 ) * ( 1.0 / 9007199254740992.0 );
}

// Uniform f32 in [lo, hi).
ORB_INLINE f32
rng_range_f32( rng_t* r, f32 lo, f32 hi )
{
    return lo + ( hi - lo ) * rng_f32( r );
}

/*==============================================================================================
    Convenience
==============================================================================================*/

// True with probability p (p <= 0 never, p >= 1 always).
ORB_INLINE b32
rng_chance( rng_t* r, f32 p )
{
    return rng_f32( r ) < p;
}

// Uniformly -1 or +1.
ORB_INLINE i32
rng_sign( rng_t* r )
{
    return ( rng_u32( r ) & 1u ) ? 1 : -1;
}

// Uniform angle in [0, TAU).
ORB_INLINE f32
rng_angle( rng_t* r )
{
    return rng_f32( r ) * MATH_TAU;
}

/*==============================================================================================
    Composite sampling (math_rng.c)
==============================================================================================*/

// Standard normal sample (mean 0, stddev 1). Marsaglia polar; second sample is cached.
f32 rng_gauss_f32( rng_t* r );

// Uniform point on the unit circle / unit sphere (out params until vec types land).
void rng_unit2( rng_t* r, f32* x, f32* y );
void rng_unit3( rng_t* r, f32* x, f32* y, f32* z );

// Uniform point inside the unit disk / unit ball.
void rng_in_disk( rng_t* r, f32* x, f32* y );
void rng_in_sphere( rng_t* r, f32* x, f32* y, f32* z );

// In-place Fisher-Yates shuffle of count items of stride bytes.
void rng_shuffle( rng_t* r, void* items, u32 count, usize stride );

// Pick an index with probability proportional to weights[i]. Negative weights count as 0.
// Returns 0 if count is 0 or total weight is 0.
u32 rng_weighted( rng_t* r, const f32* weights, u32 count );

// clang-format on
/*============================================================================================*/
#endif    // MATH_RNG_H
