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
