/*==============================================================================================

    base/math_mat.h -- 3x3 and 4x4 float matrices.

    CONVENTIONS (fixed engine-wide -- these match the GPU push-constant layout and the existing
    hand-rolled draw/editor projections, so a matrix built here drops straight into a shader):

      - Column-major storage.  Element at (row r, col c) is m[ c*4 + r ].  This is what GLSL/
        SPIR-V expect, so mat4_t memcpys directly into a push constant with no transpose.
      - Column vectors.  A transform is applied as  v' = M * v  (mat4_mul_vec4).
      - Composition reads right-to-left:  mat4_mul( A, B ) applies B first, then A.
      - Right-handed world space, camera looks down -Z.
      - Vulkan clip space: NDC depth in [0,1], Y points DOWN (projections flip Y).

    Naming: mat<N>_<operation>, value-returning.

==============================================================================================*/
#ifndef MATH_MAT_H
#define MATH_MAT_H

// clang-format off
/*==============================================================================================
    Types
==============================================================================================*/

typedef union mat3_s
{
    f32    m[ 9 ];      // column-major: m[ col*3 + row ]
    vec3_t col[ 3 ];
} mat3_t;

typedef union ORB_ALIGNAS( 16 ) mat4_s
{
    f32    m[ 16 ];     // column-major: m[ col*4 + row ]
    vec4_t col[ 4 ];
} mat4_t;

/*==============================================================================================
    mat4 -- basics
==============================================================================================*/

ORB_INLINE mat4_t
mat4_identity( void )
{
    return ( mat4_t ){ .m = { 1, 0, 0, 0,
                              0, 1, 0, 0,
                              0, 0, 1, 0,
                              0, 0, 0, 1 } };
}

// out = a * b  (apply b first, then a).
ORB_INLINE mat4_t
mat4_mul( mat4_t a, mat4_t b )
{
    mat4_t o;
    for ( i32 c = 0; c < 4; c++ )
        for ( i32 r = 0; r < 4; r++ )
            o.m[ c * 4 + r ] = a.m[ 0 * 4 + r ] * b.m[ c * 4 + 0 ]
                             + a.m[ 1 * 4 + r ] * b.m[ c * 4 + 1 ]
                             + a.m[ 2 * 4 + r ] * b.m[ c * 4 + 2 ]
                             + a.m[ 3 * 4 + r ] * b.m[ c * 4 + 3 ];
    return o;
}

ORB_INLINE vec4_t
mat4_mul_vec4( mat4_t a, vec4_t v )
{
    return ( vec4_t ){
        .x = a.m[ 0 ] * v.x + a.m[ 4 ] * v.y + a.m[  8 ] * v.z + a.m[ 12 ] * v.w,
        .y = a.m[ 1 ] * v.x + a.m[ 5 ] * v.y + a.m[  9 ] * v.z + a.m[ 13 ] * v.w,
        .z = a.m[ 2 ] * v.x + a.m[ 6 ] * v.y + a.m[ 10 ] * v.z + a.m[ 14 ] * v.w,
        .w = a.m[ 3 ] * v.x + a.m[ 7 ] * v.y + a.m[ 11 ] * v.z + a.m[ 15 ] * v.w,
    };
}

// Transform a position (implicit w=1); the perspective divide is NOT applied.
ORB_INLINE vec3_t mat4_transform_point( mat4_t a, vec3_t p ) { return mat4_mul_vec4( a, vec3_to_vec4( p, 1.0f ) ).xyz; }
// Transform a direction (w=0): rotation/scale only, translation ignored.
ORB_INLINE vec3_t mat4_transform_dir  ( mat4_t a, vec3_t d ) { return mat4_mul_vec4( a, vec3_to_vec4( d, 0.0f ) ).xyz; }

ORB_INLINE mat4_t
mat4_transpose( mat4_t a )
{
    mat4_t o;
    for ( i32 c = 0; c < 4; c++ )
        for ( i32 r = 0; r < 4; r++ )
            o.m[ c * 4 + r ] = a.m[ r * 4 + c ];
    return o;
}

/*==============================================================================================
    mat4 -- affine builders
==============================================================================================*/

ORB_INLINE mat4_t
mat4_translation( vec3_t t )
{
    mat4_t o = mat4_identity();
    o.m[ 12 ] = t.x; o.m[ 13 ] = t.y; o.m[ 14 ] = t.z;
    return o;
}

ORB_INLINE mat4_t
mat4_scaling( vec3_t s )
{
    mat4_t o = mat4_identity();
    o.m[ 0 ] = s.x; o.m[ 5 ] = s.y; o.m[ 10 ] = s.z;
    return o;
}

// Rotation about a unit axis by `radians` (right-handed).
ORB_INLINE mat4_t
mat4_rotation_axis( vec3_t axis, f32 radians )
{
    vec3_t a = vec3_normalize( axis );
    f32    c = f32_cos( radians );
    f32    s = f32_sin( radians );
    f32    t = 1.0f - c;

    mat4_t o = mat4_identity();
    o.m[ 0 ] = c + a.x * a.x * t;         o.m[ 1 ] = a.y * a.x * t + a.z * s;   o.m[ 2 ]  = a.z * a.x * t - a.y * s;
    o.m[ 4 ] = a.x * a.y * t - a.z * s;   o.m[ 5 ] = c + a.y * a.y * t;         o.m[ 6 ]  = a.z * a.y * t + a.x * s;
    o.m[ 8 ] = a.x * a.z * t + a.y * s;   o.m[ 9 ] = a.y * a.z * t - a.x * s;   o.m[ 10 ] = c + a.z * a.z * t;
    return o;
}

/*==============================================================================================
    mat4 -- camera & projection  (right-handed, Vulkan clip: z in [0,1], y-down)
==============================================================================================*/

// View matrix for a camera at `eye` looking at `target`, with world up `up`.
ORB_INLINE mat4_t
mat4_look_at( vec3_t eye, vec3_t target, vec3_t up )
{
    vec3_t f = vec3_normalize( vec3_sub( target, eye ) );   // forward (-Z maps here)
    vec3_t s = vec3_normalize( vec3_cross( f, up ) );       // right
    vec3_t u = vec3_cross( s, f );                          // true up

    mat4_t o = mat4_identity();
    o.m[ 0 ] =  s.x; o.m[ 1 ] =  u.x; o.m[ 2 ]  = -f.x;
    o.m[ 4 ] =  s.y; o.m[ 5 ] =  u.y; o.m[ 6 ]  = -f.y;
    o.m[ 8 ] =  s.z; o.m[ 9 ] =  u.z; o.m[ 10 ] = -f.z;
    o.m[ 12 ] = -vec3_dot( s, eye );
    o.m[ 13 ] = -vec3_dot( u, eye );
    o.m[ 14 ] =  vec3_dot( f, eye );
    return o;
}

// Perspective projection.  fov_y in radians (vertical), aspect = width/height, near/far > 0.
ORB_INLINE mat4_t
mat4_perspective( f32 fov_y, f32 aspect, f32 z_near, f32 z_far )
{
    f32    t = 1.0f / f32_tan( fov_y * 0.5f );
    mat4_t o = ( mat4_t ){ 0 };
    o.m[ 0 ]  = t / aspect;
    o.m[ 5 ]  = -t;                                 // Vulkan NDC y points down
    o.m[ 10 ] = z_far / ( z_near - z_far );
    o.m[ 11 ] = -1.0f;
    o.m[ 14 ] = ( z_near * z_far ) / ( z_near - z_far );
    return o;
}

// Orthographic projection.  y-down: `top` maps to NDC -1, `bottom` to +1 (screen convention).
ORB_INLINE mat4_t
mat4_ortho( f32 left, f32 right, f32 top, f32 bottom, f32 z_near, f32 z_far )
{
    mat4_t o = ( mat4_t ){ 0 };
    o.m[ 0 ]  = 2.0f / ( right - left );
    o.m[ 5 ]  = 2.0f / ( bottom - top );
    o.m[ 10 ] = 1.0f / ( z_far - z_near );
    o.m[ 12 ] = -( right + left ) / ( right - left );
    o.m[ 13 ] = -( bottom + top ) / ( bottom - top );
    o.m[ 14 ] = -z_near / ( z_far - z_near );
    o.m[ 15 ] = 1.0f;
    return o;
}

// Pixel-space 2D ortho: maps ([0,w] x [0,h], origin top-left) to Vulkan NDC, z passthrough.
ORB_INLINE mat4_t mat4_ortho_2d( f32 w, f32 h ) { return mat4_ortho( 0.0f, w, 0.0f, h, 0.0f, 1.0f ); }

/*==============================================================================================
    mat4 -- general inverse

    Full 4x4 cofactor inverse for arbitrary matrices (projections, non-uniform scale).  Returns
    the identity if the matrix is singular (|det| < epsilon).  For a pure rigid transform, a
    transpose-of-rotation + negated translation is far cheaper -- reach for that when you know
    the shape.
==============================================================================================*/

ORB_INLINE mat4_t
mat4_inverse( mat4_t a )
{
    const f32* m = a.m;
    f32 inv[ 16 ];

    inv[ 0 ]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[ 4 ]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[ 8 ]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[ 12 ] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[ 1 ]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[ 5 ]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[ 9 ]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[ 13 ] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[ 2 ]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[ 6 ]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[ 10 ] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[ 14 ] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[ 3 ]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[ 7 ]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[ 11 ] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[ 15 ] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    f32 det = m[ 0 ] * inv[ 0 ] + m[ 1 ] * inv[ 4 ] + m[ 2 ] * inv[ 8 ] + m[ 3 ] * inv[ 12 ];
    if ( f32_abs( det ) < F32_EPSILON )
        return mat4_identity();         // singular: no inverse

    f32    inv_det = 1.0f / det;
    mat4_t o;
    for ( i32 i = 0; i < 16; i++ )
        o.m[ i ] = inv[ i ] * inv_det;
    return o;
}

/*==============================================================================================
    mat4 -- fast shape-specific inverses

    When you know the matrix is a pure rigid or affine transform, skip the full cofactor inverse.
    inverse_rigid assumes an orthonormal 3x3 (rotation only, no scale) -- ideal for view matrices.
    inverse_affine tolerates rotation + non-uniform scale but still no perspective row.
==============================================================================================*/

// Inverse of translation*rotation (orthonormal upper 3x3): transpose R, negate the rotated t.
ORB_INLINE mat4_t
mat4_inverse_rigid( mat4_t a )
{
    mat4_t o = mat4_identity();
    // R^-1 = R^T : mirror the upper-left 3x3.
    o.m[ 0 ] = a.m[ 0 ]; o.m[ 1 ] = a.m[ 4 ]; o.m[ 2 ]  = a.m[ 8 ];
    o.m[ 4 ] = a.m[ 1 ]; o.m[ 5 ] = a.m[ 5 ]; o.m[ 6 ]  = a.m[ 9 ];
    o.m[ 8 ] = a.m[ 2 ]; o.m[ 9 ] = a.m[ 6 ]; o.m[ 10 ] = a.m[ 10 ];
    // t' = -R^T * t
    vec3_t t = ( vec3_t ){ .x = a.m[ 12 ], .y = a.m[ 13 ], .z = a.m[ 14 ] };
    o.m[ 12 ] = -( o.m[ 0 ] * t.x + o.m[ 4 ] * t.y + o.m[ 8 ]  * t.z );
    o.m[ 13 ] = -( o.m[ 1 ] * t.x + o.m[ 5 ] * t.y + o.m[ 9 ]  * t.z );
    o.m[ 14 ] = -( o.m[ 2 ] * t.x + o.m[ 6 ] * t.y + o.m[ 10 ] * t.z );
    return o;
}

/*==============================================================================================
    mat3
==============================================================================================*/

ORB_INLINE mat3_t
mat3_identity( void )
{
    return ( mat3_t ){ .m = { 1, 0, 0,
                              0, 1, 0,
                              0, 0, 1 } };
}

// Upper-left 3x3 of a mat4 (rotation/scale, drops translation).
ORB_INLINE mat3_t
mat3_from_mat4( mat4_t a )
{
    return ( mat3_t ){ .m = { a.m[ 0 ], a.m[ 1 ], a.m[ 2 ],
                              a.m[ 4 ], a.m[ 5 ], a.m[ 6 ],
                              a.m[ 8 ], a.m[ 9 ], a.m[ 10 ] } };
}

ORB_INLINE vec3_t
mat3_mul_vec3( mat3_t a, vec3_t v )
{
    return ( vec3_t ){
        .x = a.m[ 0 ] * v.x + a.m[ 3 ] * v.y + a.m[ 6 ] * v.z,
        .y = a.m[ 1 ] * v.x + a.m[ 4 ] * v.y + a.m[ 7 ] * v.z,
        .z = a.m[ 2 ] * v.x + a.m[ 5 ] * v.y + a.m[ 8 ] * v.z,
    };
}

ORB_INLINE mat3_t
mat3_transpose( mat3_t a )
{
    mat3_t o;
    for ( i32 c = 0; c < 3; c++ )
        for ( i32 r = 0; r < 3; r++ )
            o.m[ c * 3 + r ] = a.m[ r * 3 + c ];
    return o;
}

// out = a * b  (apply b first, then a).
ORB_INLINE mat3_t
mat3_mul( mat3_t a, mat3_t b )
{
    mat3_t o;
    for ( i32 c = 0; c < 3; c++ )
        for ( i32 r = 0; r < 3; r++ )
            o.m[ c * 3 + r ] = a.m[ 0 * 3 + r ] * b.m[ c * 3 + 0 ]
                             + a.m[ 1 * 3 + r ] * b.m[ c * 3 + 1 ]
                             + a.m[ 2 * 3 + r ] * b.m[ c * 3 + 2 ];
    return o;
}

ORB_INLINE f32
mat3_determinant( mat3_t a )
{
    const f32* m = a.m;
    return m[ 0 ] * ( m[ 4 ] * m[ 8 ] - m[ 7 ] * m[ 5 ] )
         - m[ 3 ] * ( m[ 1 ] * m[ 8 ] - m[ 7 ] * m[ 2 ] )
         + m[ 6 ] * ( m[ 1 ] * m[ 5 ] - m[ 4 ] * m[ 2 ] );
}

// Cofactor inverse.  Returns the identity if the matrix is singular (|det| < epsilon).
ORB_INLINE mat3_t
mat3_inverse( mat3_t a )
{
    const f32* m = a.m;
    // Cofactors (column-major output: o.m[ col*3 + row ]).
    f32 c00 = m[ 4 ] * m[ 8 ] - m[ 7 ] * m[ 5 ];
    f32 c01 = m[ 7 ] * m[ 2 ] - m[ 1 ] * m[ 8 ];
    f32 c02 = m[ 1 ] * m[ 5 ] - m[ 4 ] * m[ 2 ];
    f32 det = m[ 0 ] * c00 + m[ 3 ] * c01 + m[ 6 ] * c02;
    if ( f32_abs( det ) < F32_EPSILON )
        return mat3_identity();

    f32 inv_det = 1.0f / det;
    mat3_t o;
    o.m[ 0 ] = c00 * inv_det;
    o.m[ 1 ] = c01 * inv_det;
    o.m[ 2 ] = c02 * inv_det;
    o.m[ 3 ] = ( m[ 6 ] * m[ 5 ] - m[ 3 ] * m[ 8 ] ) * inv_det;
    o.m[ 4 ] = ( m[ 0 ] * m[ 8 ] - m[ 6 ] * m[ 2 ] ) * inv_det;
    o.m[ 5 ] = ( m[ 3 ] * m[ 2 ] - m[ 0 ] * m[ 5 ] ) * inv_det;
    o.m[ 6 ] = ( m[ 3 ] * m[ 7 ] - m[ 6 ] * m[ 4 ] ) * inv_det;
    o.m[ 7 ] = ( m[ 6 ] * m[ 1 ] - m[ 0 ] * m[ 7 ] ) * inv_det;
    o.m[ 8 ] = ( m[ 0 ] * m[ 4 ] - m[ 3 ] * m[ 1 ] ) * inv_det;
    return o;
}

// Normal matrix: inverse-transpose of a model's upper 3x3.  Transforms normals correctly under
// non-uniform scale.  (For a pure rotation this equals the 3x3 itself.)
ORB_INLINE mat3_t mat3_normal_matrix( mat4_t model ) { return mat3_transpose( mat3_inverse( mat3_from_mat4( model ) ) ); }

/*==============================================================================================
    mat4 -- affine inverse (needs mat3_inverse, so it trails the mat3 section)

    Inverts rotation + non-uniform scale + translation (no perspective).  Cheaper and more stable
    than the full cofactor mat4_inverse when you know the bottom row is (0,0,0,1).
==============================================================================================*/

ORB_INLINE mat4_t
mat4_inverse_affine( mat4_t a )
{
    mat3_t inv3 = mat3_inverse( mat3_from_mat4( a ) );
    vec3_t t    = ( vec3_t ){ .x = a.m[ 12 ], .y = a.m[ 13 ], .z = a.m[ 14 ] };
    vec3_t nt   = vec3_neg( mat3_mul_vec3( inv3, t ) );

    mat4_t o = mat4_identity();
    o.m[ 0 ] = inv3.m[ 0 ]; o.m[ 1 ] = inv3.m[ 1 ]; o.m[ 2 ]  = inv3.m[ 2 ];
    o.m[ 4 ] = inv3.m[ 3 ]; o.m[ 5 ] = inv3.m[ 4 ]; o.m[ 6 ]  = inv3.m[ 5 ];
    o.m[ 8 ] = inv3.m[ 6 ]; o.m[ 9 ] = inv3.m[ 7 ]; o.m[ 10 ] = inv3.m[ 8 ];
    o.m[ 12 ] = nt.x; o.m[ 13 ] = nt.y; o.m[ 14 ] = nt.z;
    return o;
}

// clang-format on
/*============================================================================================*/
#endif    // MATH_MAT_H
