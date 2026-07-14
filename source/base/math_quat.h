/*==============================================================================================

    base/math_quat.h -- unit quaternions for 3D rotation.

    A quaternion is (x,y,z,w) with the vector part first and scalar w last -- the same field
    order as vec4_t, so a quat aliases a vec4 in memory.  Identity is (0,0,0,1).  Rotations
    compose like matrices: quat_mul( a, b ) applies b first, then a.

    Handedness and the derived matrix match math_mat.h (right-handed, column-major), so
    quat_to_mat4 slots straight into a model/view chain.

    Naming: quat_<operation>, value-returning.  Depends on math_vec.h + math_mat.h.

==============================================================================================*/
#ifndef MATH_QUAT_H
#define MATH_QUAT_H

// clang-format off
/*==============================================================================================
    Type
==============================================================================================*/

typedef union ORB_ALIGNAS( 16 ) quat_s
{
    struct { f32 x, y, z, w; };
    struct { vec3_t xyz; f32 _w0; };    // vector part / scalar part
    vec4_t v;                           // aliases as a plain vec4
    f32    e[ 4 ];
} quat_t;

/*==============================================================================================
    Constructors
==============================================================================================*/

ORB_INLINE quat_t quat_make    ( f32 x, f32 y, f32 z, f32 w ) { return ( quat_t ){ .x = x, .y = y, .z = z, .w = w }; }
ORB_INLINE quat_t quat_identity( void )                       { return ( quat_t ){ .x = 0, .y = 0, .z = 0, .w = 1 }; }

// Rotation of `radians` about a unit `axis`.
ORB_INLINE quat_t
quat_from_axis_angle( vec3_t axis, f32 radians )
{
    vec3_t a    = vec3_normalize( axis );
    f32    half = radians * 0.5f;
    f32    s    = f32_sin( half );
    return ( quat_t ){ .x = a.x * s, .y = a.y * s, .z = a.z * s, .w = f32_cos( half ) };
}

// From intrinsic Tait-Bryan angles (radians): applied yaw (Y), then pitch (X), then roll (Z).
ORB_INLINE quat_t
quat_from_euler( f32 pitch_x, f32 yaw_y, f32 roll_z )
{
    f32 cx = f32_cos( pitch_x * 0.5f ), sx = f32_sin( pitch_x * 0.5f );
    f32 cy = f32_cos( yaw_y   * 0.5f ), sy = f32_sin( yaw_y   * 0.5f );
    f32 cz = f32_cos( roll_z  * 0.5f ), sz = f32_sin( roll_z  * 0.5f );
    return ( quat_t ){
        .x = sx * cy * cz + cx * sy * sz,
        .y = cx * sy * cz - sx * cy * sz,
        .z = cx * cy * sz - sx * sy * cz,
        .w = cx * cy * cz + sx * sy * sz,
    };
}

/*==============================================================================================
    Core ops
==============================================================================================*/

ORB_INLINE f32    quat_dot      ( quat_t a, quat_t b ) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
ORB_INLINE f32    quat_len      ( quat_t a )           { return f32_sqrt( quat_dot( a, a ) ); }
ORB_INLINE quat_t quat_conjugate( quat_t a )           { return ( quat_t ){ .x = -a.x, .y = -a.y, .z = -a.z, .w = a.w }; }

ORB_INLINE quat_t
quat_normalize( quat_t a )
{
    f32 len = quat_len( a );
    f32 inv = len > F32_EPSILON ? 1.0f / len : 0.0f;
    return ( quat_t ){ .x = a.x * inv, .y = a.y * inv, .z = a.z * inv, .w = a.w * inv };
}

// out = a * b  (apply b first, then a).  Hamilton product.
ORB_INLINE quat_t
quat_mul( quat_t a, quat_t b )
{
    return ( quat_t ){
        .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Rotate a vector by a unit quaternion (t = 2*(q.xyz x v); v' = v + q.w*t + q.xyz x t).
ORB_INLINE vec3_t
quat_rotate_vec3( quat_t q, vec3_t v )
{
    vec3_t t = vec3_scale( vec3_cross( q.xyz, v ), 2.0f );
    return vec3_add( vec3_add( v, vec3_scale( t, q.w ) ), vec3_cross( q.xyz, t ) );
}

// Rotation matrix (assumes a unit quaternion), column-major to match math_mat.h.
ORB_INLINE mat4_t
quat_to_mat4( quat_t q )
{
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    mat4_t o = mat4_identity();
    o.m[ 0 ] = 1.0f - 2.0f * ( yy + zz ); o.m[ 1 ] = 2.0f * ( xy + wz );        o.m[ 2 ]  = 2.0f * ( xz - wy );
    o.m[ 4 ] = 2.0f * ( xy - wz );        o.m[ 5 ] = 1.0f - 2.0f * ( xx + zz ); o.m[ 6 ]  = 2.0f * ( yz + wx );
    o.m[ 8 ] = 2.0f * ( xz + wy );        o.m[ 9 ] = 2.0f * ( yz - wx );        o.m[ 10 ] = 1.0f - 2.0f * ( xx + yy );
    return o;
}

// Build a quaternion from an orthonormal rotation matrix (upper-left 3x3 of `m`).
ORB_INLINE quat_t
quat_from_mat4( mat4_t m )
{
    f32 r00 = m.m[ 0 ], r10 = m.m[ 1 ], r20 = m.m[ 2 ];
    f32 r01 = m.m[ 4 ], r11 = m.m[ 5 ], r21 = m.m[ 6 ];
    f32 r02 = m.m[ 8 ], r12 = m.m[ 9 ], r22 = m.m[ 10 ];
    f32 trace = r00 + r11 + r22;

    quat_t q;
    if ( trace > 0.0f )
    {
        f32 s = f32_sqrt( trace + 1.0f ) * 2.0f;    // s = 4w
        q.w = 0.25f * s;
        q.x = ( r21 - r12 ) / s;
        q.y = ( r02 - r20 ) / s;
        q.z = ( r10 - r01 ) / s;
    }
    else if ( r00 > r11 && r00 > r22 )
    {
        f32 s = f32_sqrt( 1.0f + r00 - r11 - r22 ) * 2.0f;   // s = 4x
        q.w = ( r21 - r12 ) / s;
        q.x = 0.25f * s;
        q.y = ( r01 + r10 ) / s;
        q.z = ( r02 + r20 ) / s;
    }
    else if ( r11 > r22 )
    {
        f32 s = f32_sqrt( 1.0f + r11 - r00 - r22 ) * 2.0f;   // s = 4y
        q.w = ( r02 - r20 ) / s;
        q.x = ( r01 + r10 ) / s;
        q.y = 0.25f * s;
        q.z = ( r12 + r21 ) / s;
    }
    else
    {
        f32 s = f32_sqrt( 1.0f + r22 - r00 - r11 ) * 2.0f;   // s = 4z
        q.w = ( r10 - r01 ) / s;
        q.x = ( r02 + r20 ) / s;
        q.y = ( r12 + r21 ) / s;
        q.z = 0.25f * s;
    }
    return quat_normalize( q );
}

/*==============================================================================================
    Derived rotations
==============================================================================================*/

// Total rotation angle of a unit quaternion, in radians [0, PI].
ORB_INLINE f32 quat_angle( quat_t q ) { return 2.0f * f32_acos( f32_min( f32_abs( q.w ), 1.0f ) ); }

// Shortest rotation that turns unit vector `from` onto unit vector `to`.
ORB_INLINE quat_t
quat_from_to( vec3_t from, vec3_t to )
{
    vec3_t a = vec3_normalize( from );
    vec3_t b = vec3_normalize( to );
    f32    d = vec3_dot( a, b );

    if ( d >= 1.0f - F32_EPSILON )                  // already aligned
        return quat_identity();
    if ( d <= -1.0f + F32_EPSILON )                 // opposite: 180 about any perpendicular axis
    {
        vec3_t axis = vec3_cross( vec3_make( 1, 0, 0 ), a );
        if ( vec3_len_sq( axis ) < F32_EPSILON ) axis = vec3_cross( vec3_make( 0, 1, 0 ), a );
        axis = vec3_normalize( axis );
        return ( quat_t ){ .x = axis.x, .y = axis.y, .z = axis.z, .w = 0.0f };
    }
    vec3_t c = vec3_cross( a, b );
    return quat_normalize( ( quat_t ){ .x = c.x, .y = c.y, .z = c.z, .w = 1.0f + d } );
}

// Orientation whose -Z axis points along `forward` and whose +Y aligns with `up` (camera-style,
// matching mat4_look_at).  quat_to_mat4 of this equals the look_at rotation.
ORB_INLINE quat_t
quat_look_rotation( vec3_t forward, vec3_t up )
{
    vec3_t f = vec3_normalize( forward );
    vec3_t s = vec3_normalize( vec3_cross( f, up ) );   // right
    vec3_t u = vec3_cross( s, f );                      // true up

    mat4_t basis = mat4_identity();                     // columns: right, up, -forward
    basis.m[ 0 ] = s.x; basis.m[ 1 ] = s.y; basis.m[ 2 ]  = s.z;
    basis.m[ 4 ] = u.x; basis.m[ 5 ] = u.y; basis.m[ 6 ]  = u.z;
    basis.m[ 8 ] = -f.x; basis.m[ 9 ] = -f.y; basis.m[ 10 ] = -f.z;
    return quat_from_mat4( basis );
}

// Extract intrinsic Tait-Bryan angles (radians) matching quat_from_euler: (pitch X, yaw Y, roll Z).
// Returned as a vec3 ( .x=pitch, .y=yaw, .z=roll ).
ORB_INLINE vec3_t
quat_to_euler( quat_t q )
{
    mat4_t m = quat_to_mat4( q );
    f32    r12 = m.m[ 9 ];
    vec3_t e;
    if ( r12 < 1.0f - F32_EPSILON && r12 > -1.0f + F32_EPSILON )
    {
        e.x = f32_asin( -r12 );                     // pitch (X)
        e.y = f32_atan2( m.m[ 8 ], m.m[ 10 ] );     // yaw   (Y)
        e.z = f32_atan2( m.m[ 1 ], m.m[ 5 ] );      // roll  (Z)
    }
    else                                            // gimbal lock: pitch at +/-90, fold roll into yaw
    {
        e.x = -r12 > 0.0f ? MATH_PI_OVER_2 : -MATH_PI_OVER_2;
        e.y = f32_atan2( -m.m[ 2 ], m.m[ 0 ] );
        e.z = 0.0f;
    }
    return e;
}

/*==============================================================================================
    Interpolation
==============================================================================================*/

// Normalized lerp: cheap, commutative, constant-speed only for small arcs.  Takes the short path.
ORB_INLINE quat_t
quat_nlerp( quat_t a, quat_t b, f32 t )
{
    f32    d = quat_dot( a, b );
    f32    s = d < 0.0f ? -1.0f : 1.0f;         // flip b to the near hemisphere
    quat_t r = { .x = f32_lerp( a.x, s * b.x, t ),
                 .y = f32_lerp( a.y, s * b.y, t ),
                 .z = f32_lerp( a.z, s * b.z, t ),
                 .w = f32_lerp( a.w, s * b.w, t ) };
    return quat_normalize( r );
}

// Spherical lerp: constant angular velocity.  Falls back to nlerp when the arc is tiny.
ORB_INLINE quat_t
quat_slerp( quat_t a, quat_t b, f32 t )
{
    f32 d = quat_dot( a, b );
    f32 s = 1.0f;
    if ( d < 0.0f ) { d = -d; s = -1.0f; }      // short path

    if ( d > 0.9995f )                          // nearly parallel: lerp to avoid div-by-~0
        return quat_nlerp( a, b, t );

    f32 theta = f32_acos( d );
    f32 sin_t = f32_sin( theta );
    f32 wa    = f32_sin( ( 1.0f - t ) * theta ) / sin_t;
    f32 wb    = s * f32_sin( t * theta ) / sin_t;
    return ( quat_t ){ .x = wa * a.x + wb * b.x,
                       .y = wa * a.y + wb * b.y,
                       .z = wa * a.z + wb * b.z,
                       .w = wa * a.w + wb * b.w };
}

/*==============================================================================================
    Compose  (needs quat, so it lives here rather than in math_mat.h)
==============================================================================================*/

// Build the affine transform T * R * S: scale, then rotate, then translate.
ORB_INLINE mat4_t
mat4_trs( vec3_t translation, quat_t rotation, vec3_t scale )
{
    mat4_t r = quat_to_mat4( rotation );
    // Fold scale into the rotation columns, then set the translation column -- avoids two muls.
    r.col[ 0 ] = vec4_scale( r.col[ 0 ], scale.x );
    r.col[ 1 ] = vec4_scale( r.col[ 1 ], scale.y );
    r.col[ 2 ] = vec4_scale( r.col[ 2 ], scale.z );
    r.m[ 12 ]  = translation.x;
    r.m[ 13 ]  = translation.y;
    r.m[ 14 ]  = translation.z;
    return r;
}

// clang-format on
/*============================================================================================*/
#endif    // MATH_QUAT_H
