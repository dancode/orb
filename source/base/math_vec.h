/*==============================================================================================

    base/math_vec.h -- 2D/3D/4D float vectors and 2D integer vectors.

    The shared geometry vocabulary for the whole engine: gui, draw, rhi, camera, game and
    editor all speak these types so a position/color/extent can cross a DLL boundary or land in
    a GPU vertex/push-constant without a reinterpret.

    Layout is fixed ABI.  Each float vector is a union: named fields (.x/.y/.z/.w, .r/.g/.b/.a),
    nested sub-vectors (.xy, .xyz), and indexed access (.e[]) all alias the same storage.
    vec4_t is ORB_ALIGNAS(16) (SIMD/GPU friendly); vec3_t is a bare 12 bytes on purpose -- it is
    the vertex-stream workhorse and aligning it would waste a quarter of every buffer.

    Naming: vec<N>_<operation>, value-returning, no aliasing hazards.  Use compound literals for
    ad-hoc construction ( (vec3_t){ 1, 2, 3 } ) or vecN_make / vecN_splat for clarity.

==============================================================================================*/
#ifndef MATH_VEC_H
#define MATH_VEC_H

// clang-format off
/*==============================================================================================
    Types
==============================================================================================*/

typedef union vec2_s
{
    struct { f32 x, y; };
    struct { f32 w, h; };       // extent alias (size, not the 4th component)
    f32 e[ 2 ];
} vec2_t;

typedef union vec3_s
{
    struct { f32 x, y, z; };
    struct { f32 r, g, b; };
    struct { vec2_t xy; f32 _z0; };
    struct { f32 _x0; vec2_t yz; };
    f32 e[ 3 ];
} vec3_t;

typedef union ORB_ALIGNAS( 16 ) vec4_s
{
    struct { f32 x, y, z, w; };
    struct { f32 r, g, b, a; };
    struct { vec3_t xyz; f32 _w0; };
    struct { vec2_t xy; vec2_t zw; };
    f32 e[ 4 ];
} vec4_t;

typedef union vec2i_s
{
    struct { i32 x, y; };
    struct { i32 w, h; };       // extent alias
    i32 e[ 2 ];
} vec2i_t;

/*==============================================================================================
    Constructors
==============================================================================================*/

ORB_INLINE vec2_t vec2_make ( f32 x, f32 y )               { return ( vec2_t ){ .x = x, .y = y }; }
ORB_INLINE vec3_t vec3_make ( f32 x, f32 y, f32 z )        { return ( vec3_t ){ .x = x, .y = y, .z = z }; }
ORB_INLINE vec4_t vec4_make ( f32 x, f32 y, f32 z, f32 w ) { return ( vec4_t ){ .x = x, .y = y, .z = z, .w = w }; }

ORB_INLINE vec2_t vec2_splat( f32 s ) { return ( vec2_t ){ .x = s, .y = s }; }
ORB_INLINE vec3_t vec3_splat( f32 s ) { return ( vec3_t ){ .x = s, .y = s, .z = s }; }
ORB_INLINE vec4_t vec4_splat( f32 s ) { return ( vec4_t ){ .x = s, .y = s, .z = s, .w = s }; }

ORB_INLINE vec2_t vec2_zero( void ) { return ( vec2_t ){ 0 }; }
ORB_INLINE vec3_t vec3_zero( void ) { return ( vec3_t ){ 0 }; }
ORB_INLINE vec4_t vec4_zero( void ) { return ( vec4_t ){ 0 }; }

// Widen / narrow: vec3 from vec2 (+z), vec4 from vec3 (+w), and truncations back down.
ORB_INLINE vec3_t vec2_to_vec3( vec2_t v, f32 z ) { return ( vec3_t ){ .x = v.x, .y = v.y, .z = z }; }
ORB_INLINE vec4_t vec3_to_vec4( vec3_t v, f32 w ) { return ( vec4_t ){ .x = v.x, .y = v.y, .z = v.z, .w = w }; }
ORB_INLINE vec2_t vec3_to_vec2( vec3_t v )        { return v.xy; }
ORB_INLINE vec3_t vec4_to_vec3( vec4_t v )        { return v.xyz; }

/*==============================================================================================
    vec2
==============================================================================================*/

ORB_INLINE vec2_t vec2_add  ( vec2_t a, vec2_t b ) { return ( vec2_t ){ .x = a.x + b.x, .y = a.y + b.y }; }
ORB_INLINE vec2_t vec2_sub  ( vec2_t a, vec2_t b ) { return ( vec2_t ){ .x = a.x - b.x, .y = a.y - b.y }; }
ORB_INLINE vec2_t vec2_mul  ( vec2_t a, vec2_t b ) { return ( vec2_t ){ .x = a.x * b.x, .y = a.y * b.y }; }
ORB_INLINE vec2_t vec2_scale( vec2_t a, f32 s )    { return ( vec2_t ){ .x = a.x * s,   .y = a.y * s }; }
ORB_INLINE vec2_t vec2_neg  ( vec2_t a )           { return ( vec2_t ){ .x = -a.x,      .y = -a.y }; }

ORB_INLINE f32 vec2_dot( vec2_t a, vec2_t b ) { return a.x * b.x + a.y * b.y; }
// 2D cross product: the z of (a x b); >0 if b is counter-clockwise from a.  Also = signed area.
ORB_INLINE f32 vec2_cross( vec2_t a, vec2_t b ) { return a.x * b.y - a.y * b.x; }
// Left perpendicular (90 degrees CCW): (x,y) -> (-y,x).
ORB_INLINE vec2_t vec2_perp( vec2_t a ) { return ( vec2_t ){ .x = -a.y, .y = a.x }; }

ORB_INLINE f32 vec2_len_sq( vec2_t a ) { return vec2_dot( a, a ); }
ORB_INLINE f32 vec2_len   ( vec2_t a ) { return f32_sqrt( vec2_dot( a, a ) ); }

ORB_INLINE vec2_t
vec2_normalize( vec2_t a )
{
    f32 len = vec2_len( a );
    f32 inv = len > F32_EPSILON ? 1.0f / len : 0.0f;
    return vec2_scale( a, inv );        // degenerate (near-zero) input normalizes to zero
}

ORB_INLINE vec2_t vec2_lerp( vec2_t a, vec2_t b, f32 t ) { return vec2_add( vec2_scale( a, 1.0f - t ), vec2_scale( b, t ) ); }
ORB_INLINE vec2_t vec2_min ( vec2_t a, vec2_t b ) { return ( vec2_t ){ .x = f32_min( a.x, b.x ), .y = f32_min( a.y, b.y ) }; }
ORB_INLINE vec2_t vec2_max ( vec2_t a, vec2_t b ) { return ( vec2_t ){ .x = f32_max( a.x, b.x ), .y = f32_max( a.y, b.y ) }; }

/*==============================================================================================
    vec3
==============================================================================================*/

ORB_INLINE vec3_t vec3_add  ( vec3_t a, vec3_t b ) { return ( vec3_t ){ .x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z }; }
ORB_INLINE vec3_t vec3_sub  ( vec3_t a, vec3_t b ) { return ( vec3_t ){ .x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z }; }
ORB_INLINE vec3_t vec3_mul  ( vec3_t a, vec3_t b ) { return ( vec3_t ){ .x = a.x * b.x, .y = a.y * b.y, .z = a.z * b.z }; }
ORB_INLINE vec3_t vec3_scale( vec3_t a, f32 s )    { return ( vec3_t ){ .x = a.x * s,   .y = a.y * s,   .z = a.z * s }; }
ORB_INLINE vec3_t vec3_neg  ( vec3_t a )           { return ( vec3_t ){ .x = -a.x,      .y = -a.y,      .z = -a.z }; }

ORB_INLINE f32 vec3_dot( vec3_t a, vec3_t b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }

ORB_INLINE vec3_t
vec3_cross( vec3_t a, vec3_t b )
{
    return ( vec3_t ){ .x = a.y * b.z - a.z * b.y,
                       .y = a.z * b.x - a.x * b.z,
                       .z = a.x * b.y - a.y * b.x };
}

ORB_INLINE f32 vec3_len_sq( vec3_t a ) { return vec3_dot( a, a ); }
ORB_INLINE f32 vec3_len   ( vec3_t a ) { return f32_sqrt( vec3_dot( a, a ) ); }

ORB_INLINE vec3_t
vec3_normalize( vec3_t a )
{
    f32 len = vec3_len( a );
    f32 inv = len > F32_EPSILON ? 1.0f / len : 0.0f;
    return vec3_scale( a, inv );        // degenerate (near-zero) input normalizes to zero
}

ORB_INLINE f32    vec3_dist   ( vec3_t a, vec3_t b ) { return vec3_len( vec3_sub( a, b ) ); }
ORB_INLINE vec3_t vec3_lerp   ( vec3_t a, vec3_t b, f32 t ) { return vec3_add( vec3_scale( a, 1.0f - t ), vec3_scale( b, t ) ); }
ORB_INLINE vec3_t vec3_min    ( vec3_t a, vec3_t b ) { return ( vec3_t ){ .x = f32_min( a.x, b.x ), .y = f32_min( a.y, b.y ), .z = f32_min( a.z, b.z ) }; }
ORB_INLINE vec3_t vec3_max    ( vec3_t a, vec3_t b ) { return ( vec3_t ){ .x = f32_max( a.x, b.x ), .y = f32_max( a.y, b.y ), .z = f32_max( a.z, b.z ) }; }

// Reflect incident vector i about a unit normal n:  i - 2*(i.n)*n.
ORB_INLINE vec3_t vec3_reflect( vec3_t i, vec3_t n ) { return vec3_sub( i, vec3_scale( n, 2.0f * vec3_dot( i, n ) ) ); }

/*==============================================================================================
    vec4
==============================================================================================*/

ORB_INLINE vec4_t vec4_add  ( vec4_t a, vec4_t b ) { return ( vec4_t ){ .x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z, .w = a.w + b.w }; }
ORB_INLINE vec4_t vec4_sub  ( vec4_t a, vec4_t b ) { return ( vec4_t ){ .x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z, .w = a.w - b.w }; }
ORB_INLINE vec4_t vec4_mul  ( vec4_t a, vec4_t b ) { return ( vec4_t ){ .x = a.x * b.x, .y = a.y * b.y, .z = a.z * b.z, .w = a.w * b.w }; }
ORB_INLINE vec4_t vec4_scale( vec4_t a, f32 s )    { return ( vec4_t ){ .x = a.x * s,   .y = a.y * s,   .z = a.z * s,   .w = a.w * s }; }
ORB_INLINE vec4_t vec4_neg  ( vec4_t a )           { return ( vec4_t ){ .x = -a.x,      .y = -a.y,      .z = -a.z,      .w = -a.w }; }

ORB_INLINE f32 vec4_dot   ( vec4_t a, vec4_t b ) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
ORB_INLINE f32 vec4_len_sq( vec4_t a )           { return vec4_dot( a, a ); }
ORB_INLINE f32 vec4_len   ( vec4_t a )           { return f32_sqrt( vec4_dot( a, a ) ); }

ORB_INLINE vec4_t
vec4_normalize( vec4_t a )
{
    f32 len = vec4_len( a );
    f32 inv = len > F32_EPSILON ? 1.0f / len : 0.0f;
    return vec4_scale( a, inv );
}

ORB_INLINE vec4_t vec4_lerp( vec4_t a, vec4_t b, f32 t ) { return vec4_add( vec4_scale( a, 1.0f - t ), vec4_scale( b, t ) ); }

/*==============================================================================================
    vec2i
==============================================================================================*/

ORB_INLINE vec2i_t vec2i_make( i32 x, i32 y )      { return ( vec2i_t ){ .x = x, .y = y }; }
ORB_INLINE vec2i_t vec2i_add ( vec2i_t a, vec2i_t b ) { return ( vec2i_t ){ .x = a.x + b.x, .y = a.y + b.y }; }
ORB_INLINE vec2i_t vec2i_sub ( vec2i_t a, vec2i_t b ) { return ( vec2i_t ){ .x = a.x - b.x, .y = a.y - b.y }; }
ORB_INLINE b32     vec2i_eq  ( vec2i_t a, vec2i_t b ) { return a.x == b.x && a.y == b.y; }

// clang-format on
/*============================================================================================*/
#endif    // MATH_VEC_H
