/*==============================================================================================

    base/math_ease.h -- easing curves, smoothing and frame-rate-independent damping.

    The tweening/animation cluster: scalar shaping functions that turn a linear parameter into a
    nicer motion.  Two families live here:

      - Curve shapers f( t ) with t in [0,1] -> [0,1]: smoothstep and the classic ease_* set
        (quad/cubic/quart/quint/sine/expo/circ/back/elastic/bounce, each in/out/inout).  Feed
        these a normalized progress and lerp with the result.
      - Time-based smoothing: f32_damp (exponential approach, correct under variable dt) and its
        angular twin.  Prefer these over a raw lerp-per-frame, which is frame-rate dependent.

    Naming: f32_<curve> for shapers, f32_damp* for smoothing.  Depends on math.h.

==============================================================================================*/
#ifndef MATH_EASE_H
#define MATH_EASE_H

// clang-format off
/*==============================================================================================
    Smoothstep
==============================================================================================*/

// Hermite smoothstep on the raw t (assumes t already in [0,1]): 3t^2 - 2t^3.  Zero slope at ends.
ORB_INLINE f32 f32_smoothstep01 ( f32 t ) { return t * t * ( 3.0f - 2.0f * t ); }
// Ken Perlin's smootherstep: 6t^5 - 15t^4 + 10t^3.  Zero 1st AND 2nd derivative at the ends.
ORB_INLINE f32 f32_smootherstep01( f32 t ) { return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f ); }

// GLSL smoothstep: remaps x from [edge0,edge1] to [0,1] then applies the Hermite curve.
ORB_INLINE f32
f32_smoothstep( f32 edge0, f32 edge1, f32 x )
{
    f32 t = f32_saturate( ( x - edge0 ) / ( edge1 - edge0 ) );
    return f32_smoothstep01( t );
}

/*==============================================================================================
    Time-based smoothing  (frame-rate independent)
==============================================================================================*/

// Exponentially approach `target` from `current`.  `rate` is the approach speed (1/seconds);
// larger = snappier.  Correct for any dt because the decay is exp(-rate*dt), not a fixed step.
ORB_INLINE f32
f32_damp( f32 current, f32 target, f32 rate, f32 dt )
{
    f32 k = 1.0f - f32_exp( -rate * dt );
    return f32_lerp( current, target, k );
}

// Same, but takes the shortest arc between two angles (radians).
ORB_INLINE f32
f32_damp_angle( f32 current, f32 target, f32 rate, f32 dt )
{
    f32 k = 1.0f - f32_exp( -rate * dt );
    return f32_lerp_angle( current, target, k );
}

/*==============================================================================================
    Penner easing family  (t in [0,1] -> [0,1])

    in    -- accelerates from rest
    out   -- decelerates to rest
    inout -- accelerates then decelerates (symmetric)
==============================================================================================*/

ORB_INLINE f32 f32_ease_in_quad ( f32 t ) { return t * t; }
ORB_INLINE f32 f32_ease_out_quad( f32 t ) { return t * ( 2.0f - t ); }
ORB_INLINE f32 f32_ease_inout_quad( f32 t ) { return t < 0.5f ? 2.0f * t * t : 1.0f - 0.5f * ( -2.0f * t + 2.0f ) * ( -2.0f * t + 2.0f ); }

ORB_INLINE f32 f32_ease_in_cubic ( f32 t ) { return t * t * t; }
ORB_INLINE f32 f32_ease_out_cubic( f32 t ) { f32 u = 1.0f - t; return 1.0f - u * u * u; }
ORB_INLINE f32 f32_ease_inout_cubic( f32 t ) { f32 u = -2.0f * t + 2.0f; return t < 0.5f ? 4.0f * t * t * t : 1.0f - 0.5f * u * u * u; }

ORB_INLINE f32 f32_ease_in_quart ( f32 t ) { return t * t * t * t; }
ORB_INLINE f32 f32_ease_out_quart( f32 t ) { f32 u = 1.0f - t; return 1.0f - u * u * u * u; }
ORB_INLINE f32 f32_ease_inout_quart( f32 t ) { f32 u = -2.0f * t + 2.0f; return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 0.5f * u * u * u * u; }

ORB_INLINE f32 f32_ease_in_quint ( f32 t ) { return t * t * t * t * t; }
ORB_INLINE f32 f32_ease_out_quint( f32 t ) { f32 u = 1.0f - t; return 1.0f - u * u * u * u * u; }
ORB_INLINE f32 f32_ease_inout_quint( f32 t ) { f32 u = -2.0f * t + 2.0f; return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - 0.5f * u * u * u * u * u; }

ORB_INLINE f32 f32_ease_in_sine ( f32 t ) { return 1.0f - f32_cos( t * MATH_PI_OVER_2 ); }
ORB_INLINE f32 f32_ease_out_sine( f32 t ) { return f32_sin( t * MATH_PI_OVER_2 ); }
ORB_INLINE f32 f32_ease_inout_sine( f32 t ) { return -0.5f * ( f32_cos( MATH_PI * t ) - 1.0f ); }

ORB_INLINE f32 f32_ease_in_expo ( f32 t ) { return t <= 0.0f ? 0.0f : f32_pow( 2.0f, 10.0f * t - 10.0f ); }
ORB_INLINE f32 f32_ease_out_expo( f32 t ) { return t >= 1.0f ? 1.0f : 1.0f - f32_pow( 2.0f, -10.0f * t ); }
ORB_INLINE f32
f32_ease_inout_expo( f32 t )
{
    if ( t <= 0.0f ) return 0.0f;
    if ( t >= 1.0f ) return 1.0f;
    return t < 0.5f ? 0.5f * f32_pow( 2.0f, 20.0f * t - 10.0f )
                    : 1.0f - 0.5f * f32_pow( 2.0f, -20.0f * t + 10.0f );
}

ORB_INLINE f32 f32_ease_in_circ ( f32 t ) { return 1.0f - f32_sqrt( 1.0f - t * t ); }
ORB_INLINE f32 f32_ease_out_circ( f32 t ) { f32 u = t - 1.0f; return f32_sqrt( 1.0f - u * u ); }
ORB_INLINE f32
f32_ease_inout_circ( f32 t )
{
    if ( t < 0.5f ) { f32 u = 2.0f * t;        return 0.5f * ( 1.0f - f32_sqrt( 1.0f - u * u ) ); }
    f32 u = -2.0f * t + 2.0f;                  return 0.5f * ( f32_sqrt( 1.0f - u * u ) + 1.0f );
}

// Back: overshoots slightly past the endpoint before settling.
ORB_INLINE f32 f32_ease_in_back ( f32 t ) { const f32 c1 = 1.70158f, c3 = 2.70158f; return c3 * t * t * t - c1 * t * t; }
ORB_INLINE f32 f32_ease_out_back( f32 t ) { const f32 c1 = 1.70158f, c3 = 2.70158f; f32 u = t - 1.0f; return 1.0f + c3 * u * u * u + c1 * u * u; }
ORB_INLINE f32
f32_ease_inout_back( f32 t )
{
    const f32 c2 = 1.70158f * 1.525f;
    if ( t < 0.5f ) { f32 u = 2.0f * t;       return 0.5f * ( u * u * ( ( c2 + 1.0f ) * u - c2 ) ); }
    f32 u = 2.0f * t - 2.0f;                   return 0.5f * ( u * u * ( ( c2 + 1.0f ) * u + c2 ) + 2.0f );
}

// Elastic: springy overshoot with decaying oscillation.
ORB_INLINE f32
f32_ease_in_elastic( f32 t )
{
    if ( t <= 0.0f ) return 0.0f;
    if ( t >= 1.0f ) return 1.0f;
    const f32 c4 = MATH_TAU / 3.0f;
    return -f32_pow( 2.0f, 10.0f * t - 10.0f ) * f32_sin( ( t * 10.0f - 10.75f ) * c4 );
}
ORB_INLINE f32
f32_ease_out_elastic( f32 t )
{
    if ( t <= 0.0f ) return 0.0f;
    if ( t >= 1.0f ) return 1.0f;
    const f32 c4 = MATH_TAU / 3.0f;
    return f32_pow( 2.0f, -10.0f * t ) * f32_sin( ( t * 10.0f - 0.75f ) * c4 ) + 1.0f;
}
ORB_INLINE f32
f32_ease_inout_elastic( f32 t )
{
    if ( t <= 0.0f ) return 0.0f;
    if ( t >= 1.0f ) return 1.0f;
    const f32 c5 = MATH_TAU / 4.5f;
    if ( t < 0.5f )
        return -0.5f * f32_pow( 2.0f, 20.0f * t - 10.0f ) * f32_sin( ( 20.0f * t - 11.125f ) * c5 );
    return 0.5f * f32_pow( 2.0f, -20.0f * t + 10.0f ) * f32_sin( ( 20.0f * t - 11.125f ) * c5 ) + 1.0f;
}

// Bounce: settles like a dropped ball.  out is the primitive; in/inout mirror it.
ORB_INLINE f32
f32_ease_out_bounce( f32 t )
{
    const f32 n1 = 7.5625f, d1 = 2.75f;
    if ( t < 1.0f / d1 )      return n1 * t * t;
    if ( t < 2.0f / d1 )      { t -= 1.5f  / d1; return n1 * t * t + 0.75f; }
    if ( t < 2.5f / d1 )      { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    t -= 2.625f / d1;         return n1 * t * t + 0.984375f;
}
ORB_INLINE f32 f32_ease_in_bounce( f32 t ) { return 1.0f - f32_ease_out_bounce( 1.0f - t ); }
ORB_INLINE f32
f32_ease_inout_bounce( f32 t )
{
    return t < 0.5f ? 0.5f * ( 1.0f - f32_ease_out_bounce( 1.0f - 2.0f * t ) )
                    : 0.5f * ( 1.0f + f32_ease_out_bounce( 2.0f * t - 1.0f ) );
}

// clang-format on
/*============================================================================================*/
#endif    // MATH_EASE_H
