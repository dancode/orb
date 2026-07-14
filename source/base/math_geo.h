/*==============================================================================================

    base/math_geo.h -- geometric primitives and intersection tests.

    The "shapes" layer that sits on top of the linear-algebra headers: bounding volumes, rays and
    planes, plus the picking and culling queries the editor and renderer need.  This is where a
    mouse ray meets a bounding box and where a camera frustum rejects an off-screen object.

      rect2  -- 2D axis-aligned box (min/max), for screen/UI space
      aabb   -- 3D axis-aligned bounding box (min/max)
      ray    -- origin + direction (direction need not be unit; t is measured along it)
      plane  -- n . p + d = 0, normal points to the positive half-space
      sphere -- center + radius
      frustum-- 6 inward-facing planes, extracted from a view-projection matrix

    Conventions follow math_mat.h: right-handed, Vulkan clip (z in [0,1]).  frustum_from_mat4
    assumes that clip range.  Naming: <shape>_<operation>.  Depends on math_vec.h + math_mat.h.

==============================================================================================*/
#ifndef MATH_GEO_H
#define MATH_GEO_H

// clang-format off
/*==============================================================================================
    Types
==============================================================================================*/

typedef struct rect2_s  { vec2_t min, max; } rect2_t;      // 2D AABB
typedef struct aabb_s   { vec3_t min, max; } aabb_t;       // 3D AABB
typedef struct ray_s    { vec3_t origin, dir; } ray_t;
typedef struct plane_s  { vec3_t n; f32 d; } plane_t;      // n . p + d = 0
typedef struct sphere_s { vec3_t center; f32 radius; } sphere_t;
typedef struct frustum_s { plane_t planes[ 6 ]; } frustum_t;   // left,right,bottom,top,near,far

/*==============================================================================================
    rect2  (2D axis-aligned box)
==============================================================================================*/

ORB_INLINE rect2_t rect2_make( vec2_t min, vec2_t max ) { return ( rect2_t ){ .min = min, .max = max }; }

ORB_INLINE rect2_t
rect2_from_center_size( vec2_t center, vec2_t size )
{
    vec2_t h = vec2_scale( size, 0.5f );
    return ( rect2_t ){ .min = vec2_sub( center, h ), .max = vec2_add( center, h ) };
}

ORB_INLINE vec2_t rect2_center( rect2_t r ) { return vec2_scale( vec2_add( r.min, r.max ), 0.5f ); }
ORB_INLINE vec2_t rect2_size  ( rect2_t r ) { return vec2_sub( r.max, r.min ); }

ORB_INLINE b32
rect2_contains_point( rect2_t r, vec2_t p )
{
    return p.x >= r.min.x && p.x <= r.max.x && p.y >= r.min.y && p.y <= r.max.y;
}

ORB_INLINE b32
rect2_intersects( rect2_t a, rect2_t b )
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}

// Overlap of two rects.  If they do not intersect the result is empty (min > max on some axis).
ORB_INLINE rect2_t rect2_intersection( rect2_t a, rect2_t b ) { return ( rect2_t ){ .min = vec2_max( a.min, b.min ), .max = vec2_min( a.max, b.max ) }; }
// Smallest rect containing both.
ORB_INLINE rect2_t rect2_union       ( rect2_t a, rect2_t b ) { return ( rect2_t ){ .min = vec2_min( a.min, b.min ), .max = vec2_max( a.max, b.max ) }; }
// Grow (or shrink, if negative) each edge outward by `amount`.
ORB_INLINE rect2_t rect2_expand      ( rect2_t r, f32 amount ) { return ( rect2_t ){ .min = vec2_sub( r.min, vec2_splat( amount ) ), .max = vec2_add( r.max, vec2_splat( amount ) ) }; }

/*==============================================================================================
    aabb  (3D axis-aligned bounding box)
==============================================================================================*/

ORB_INLINE aabb_t aabb_make( vec3_t min, vec3_t max ) { return ( aabb_t ){ .min = min, .max = max }; }

ORB_INLINE aabb_t
aabb_from_center_extents( vec3_t center, vec3_t extents )
{
    return ( aabb_t ){ .min = vec3_sub( center, extents ), .max = vec3_add( center, extents ) };
}

// An empty box that any merge grows into: min = +inf, max = -inf (so merge_point seeds correctly).
ORB_INLINE aabb_t aabb_empty( void ) { return ( aabb_t ){ .min = vec3_splat( 1e30f ), .max = vec3_splat( -1e30f ) }; }

ORB_INLINE vec3_t aabb_center ( aabb_t b ) { return vec3_scale( vec3_add( b.min, b.max ), 0.5f ); }
ORB_INLINE vec3_t aabb_extents( aabb_t b ) { return vec3_scale( vec3_sub( b.max, b.min ), 0.5f ); }
ORB_INLINE vec3_t aabb_size   ( aabb_t b ) { return vec3_sub( b.max, b.min ); }

ORB_INLINE b32
aabb_contains_point( aabb_t b, vec3_t p )
{
    return p.x >= b.min.x && p.x <= b.max.x &&
           p.y >= b.min.y && p.y <= b.max.y &&
           p.z >= b.min.z && p.z <= b.max.z;
}

ORB_INLINE b32
aabb_intersects( aabb_t a, aabb_t b )
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
           a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

ORB_INLINE aabb_t aabb_merge      ( aabb_t a, aabb_t b ) { return ( aabb_t ){ .min = vec3_min( a.min, b.min ), .max = vec3_max( a.max, b.max ) }; }
ORB_INLINE aabb_t aabb_merge_point( aabb_t b, vec3_t p ) { return ( aabb_t ){ .min = vec3_min( b.min, p ),     .max = vec3_max( b.max, p ) }; }
ORB_INLINE aabb_t aabb_expand     ( aabb_t b, f32 amount ) { return ( aabb_t ){ .min = vec3_sub( b.min, vec3_splat( amount ) ), .max = vec3_add( b.max, vec3_splat( amount ) ) }; }

// Tightest axis-aligned box enclosing `b` after transform `m` (Arvo's method: |M| * extents).
ORB_INLINE aabb_t
aabb_transform( aabb_t b, mat4_t m )
{
    vec3_t c = aabb_center( b );
    vec3_t e = aabb_extents( b );
    vec3_t nc, ne;
    for ( i32 i = 0; i < 3; i++ )
    {
        // row i of the 3x3 is ( m[0*4+i], m[1*4+i], m[2*4+i] ); translation is m[12+i].
        nc.e[ i ] = m.m[ 12 + i ]
                  + m.m[ 0 * 4 + i ] * c.x + m.m[ 1 * 4 + i ] * c.y + m.m[ 2 * 4 + i ] * c.z;
        ne.e[ i ] = f32_abs( m.m[ 0 * 4 + i ] ) * e.x
                  + f32_abs( m.m[ 1 * 4 + i ] ) * e.y
                  + f32_abs( m.m[ 2 * 4 + i ] ) * e.z;
    }
    return aabb_from_center_extents( nc, ne );
}

/*==============================================================================================
    plane
==============================================================================================*/

ORB_INLINE plane_t plane_make        ( vec3_t n, f32 d )              { return ( plane_t ){ .n = n, .d = d }; }
ORB_INLINE plane_t plane_from_point   ( vec3_t n, vec3_t p )          { vec3_t u = vec3_normalize( n ); return ( plane_t ){ .n = u, .d = -vec3_dot( u, p ) }; }

// Plane through three points; normal follows the right-hand winding a->b->c.
ORB_INLINE plane_t
plane_from_points( vec3_t a, vec3_t b, vec3_t c )
{
    vec3_t n = vec3_normalize( vec3_cross( vec3_sub( b, a ), vec3_sub( c, a ) ) );
    return ( plane_t ){ .n = n, .d = -vec3_dot( n, a ) };
}

// Signed distance from p to the plane: >0 in front (normal side), <0 behind.
ORB_INLINE f32     plane_distance( plane_t pl, vec3_t p ) { return vec3_dot( pl.n, p ) + pl.d; }
// Rescale so the normal is unit length (keeps the same plane).
ORB_INLINE plane_t
plane_normalize( plane_t pl )
{
    f32 len = vec3_len( pl.n );
    f32 inv = len > F32_EPSILON ? 1.0f / len : 0.0f;
    return ( plane_t ){ .n = vec3_scale( pl.n, inv ), .d = pl.d * inv };
}

/*==============================================================================================
    sphere
==============================================================================================*/

ORB_INLINE sphere_t sphere_make( vec3_t center, f32 radius ) { return ( sphere_t ){ .center = center, .radius = radius }; }
ORB_INLINE b32      sphere_contains_point   ( sphere_t s, vec3_t p )   { return vec3_dist_sq( s.center, p ) <= s.radius * s.radius; }
ORB_INLINE b32      sphere_intersects_sphere( sphere_t a, sphere_t b ) { f32 r = a.radius + b.radius; return vec3_dist_sq( a.center, b.center ) <= r * r; }

// Sphere vs AABB: distance from the sphere center to the closest point on the box.
ORB_INLINE b32
sphere_intersects_aabb( sphere_t s, aabb_t b )
{
    vec3_t q = vec3_max( b.min, vec3_min( s.center, b.max ) );   // closest point on the box
    return vec3_dist_sq( s.center, q ) <= s.radius * s.radius;
}

/*==============================================================================================
    ray  (origin + direction; t measured along dir, which need not be unit)
==============================================================================================*/

ORB_INLINE ray_t  ray_make( vec3_t origin, vec3_t dir ) { return ( ray_t ){ .origin = origin, .dir = dir }; }
ORB_INLINE vec3_t ray_at  ( ray_t r, f32 t )            { return vec3_add( r.origin, vec3_scale( r.dir, t ) ); }

// Slab test.  On hit, *out_t is the entry distance (0 if the origin is inside).  Misses behind
// the origin (t < 0) are rejected.
ORB_INLINE b32
ray_vs_aabb( ray_t r, aabb_t b, f32* out_t )
{
    f32 tmin = 0.0f, tmax = 1e30f;
    for ( i32 i = 0; i < 3; i++ )
    {
        f32 o = r.origin.e[ i ], d = r.dir.e[ i ];
        if ( f32_abs( d ) < F32_EPSILON )
        {
            if ( o < b.min.e[ i ] || o > b.max.e[ i ] ) return false;   // parallel and outside
        }
        else
        {
            f32 inv = 1.0f / d;
            f32 t1  = ( b.min.e[ i ] - o ) * inv;
            f32 t2  = ( b.max.e[ i ] - o ) * inv;
            if ( t1 > t2 ) { f32 tmp = t1; t1 = t2; t2 = tmp; }
            tmin = f32_max( tmin, t1 );
            tmax = f32_min( tmax, t2 );
            if ( tmin > tmax ) return false;
        }
    }
    if ( out_t ) *out_t = tmin;
    return true;
}

// Single-sided-agnostic ray/plane.  Hit only in front of the origin.
ORB_INLINE b32
ray_vs_plane( ray_t r, plane_t pl, f32* out_t )
{
    f32 denom = vec3_dot( pl.n, r.dir );
    if ( f32_abs( denom ) < F32_EPSILON ) return false;             // parallel
    f32 t = -( vec3_dot( pl.n, r.origin ) + pl.d ) / denom;
    if ( t < 0.0f ) return false;
    if ( out_t ) *out_t = t;
    return true;
}

ORB_INLINE b32
ray_vs_sphere( ray_t r, sphere_t s, f32* out_t )
{
    vec3_t oc = vec3_sub( r.origin, s.center );
    f32    a  = vec3_dot( r.dir, r.dir );
    f32    b  = 2.0f * vec3_dot( oc, r.dir );
    f32    c  = vec3_dot( oc, oc ) - s.radius * s.radius;
    f32    disc = b * b - 4.0f * a * c;
    if ( disc < 0.0f ) return false;
    f32 sq = f32_sqrt( disc );
    f32 t  = ( -b - sq ) / ( 2.0f * a );                            // near root
    if ( t < 0.0f ) t = ( -b + sq ) / ( 2.0f * a );                 // origin inside: use far root
    if ( t < 0.0f ) return false;
    if ( out_t ) *out_t = t;
    return true;
}

// Moller-Trumbore.  Hits either face.  On hit, *out_t is the distance along dir.
ORB_INLINE b32
ray_vs_triangle( ray_t r, vec3_t v0, vec3_t v1, vec3_t v2, f32* out_t )
{
    vec3_t e1 = vec3_sub( v1, v0 );
    vec3_t e2 = vec3_sub( v2, v0 );
    vec3_t p  = vec3_cross( r.dir, e2 );
    f32    det = vec3_dot( e1, p );
    if ( f32_abs( det ) < F32_EPSILON ) return false;               // ray parallel to triangle
    f32    inv = 1.0f / det;

    vec3_t t  = vec3_sub( r.origin, v0 );
    f32    u  = vec3_dot( t, p ) * inv;
    if ( u < 0.0f || u > 1.0f ) return false;

    vec3_t q  = vec3_cross( t, e1 );
    f32    v  = vec3_dot( r.dir, q ) * inv;
    if ( v < 0.0f || u + v > 1.0f ) return false;

    f32 dist = vec3_dot( e2, q ) * inv;
    if ( dist < 0.0f ) return false;
    if ( out_t ) *out_t = dist;
    return true;
}

/*==============================================================================================
    frustum  (Gribb-Hartmann extraction; Vulkan clip z in [0,1])
==============================================================================================*/

// Extract the 6 clip planes from a view-projection matrix.  Normals point INWARD, so a point is
// inside the frustum iff plane_distance >= 0 for all six.  Pass proj*view (world-space cull).
ORB_INLINE frustum_t
frustum_from_mat4( mat4_t m )
{
    // Row r of the matrix is ( m[r], m[4+r], m[8+r], m[12+r] ).
    #define GEO_ROW( r ) m.m[ ( r ) ], m.m[ 4 + ( r ) ], m.m[ 8 + ( r ) ], m.m[ 12 + ( r ) ]
    f32 r0[ 4 ] = { GEO_ROW( 0 ) };
    f32 r1[ 4 ] = { GEO_ROW( 1 ) };
    f32 r2[ 4 ] = { GEO_ROW( 2 ) };
    f32 r3[ 4 ] = { GEO_ROW( 3 ) };
    #undef GEO_ROW

    // combos[k] = { a, b, c, d } for each plane before normalization.
    f32 combo[ 6 ][ 4 ];
    for ( i32 j = 0; j < 4; j++ )
    {
        combo[ 0 ][ j ] = r3[ j ] + r0[ j ];        // left
        combo[ 1 ][ j ] = r3[ j ] - r0[ j ];        // right
        combo[ 2 ][ j ] = r3[ j ] + r1[ j ];        // bottom
        combo[ 3 ][ j ] = r3[ j ] - r1[ j ];        // top
        combo[ 4 ][ j ] = r2[ j ];                  // near  (z in [0,1]: 0*w + z >= 0)
        combo[ 5 ][ j ] = r3[ j ] - r2[ j ];        // far
    }

    frustum_t f;
    for ( i32 k = 0; k < 6; k++ )
    {
        plane_t pl = { .n = vec3_make( combo[ k ][ 0 ], combo[ k ][ 1 ], combo[ k ][ 2 ] ), .d = combo[ k ][ 3 ] };
        f.planes[ k ] = plane_normalize( pl );
    }
    return f;
}

ORB_INLINE b32
frustum_vs_point( frustum_t f, vec3_t p )
{
    for ( i32 k = 0; k < 6; k++ )
        if ( plane_distance( f.planes[ k ], p ) < 0.0f ) return false;
    return true;
}

ORB_INLINE b32
frustum_vs_sphere( frustum_t f, sphere_t s )
{
    for ( i32 k = 0; k < 6; k++ )
        if ( plane_distance( f.planes[ k ], s.center ) < -s.radius ) return false;
    return true;
}

// Conservative AABB test using the positive vertex (the corner farthest along each normal).
ORB_INLINE b32
frustum_vs_aabb( frustum_t f, aabb_t b )
{
    for ( i32 k = 0; k < 6; k++ )
    {
        vec3_t n = f.planes[ k ].n;
        vec3_t pv = ( vec3_t ){
            .x = n.x >= 0.0f ? b.max.x : b.min.x,
            .y = n.y >= 0.0f ? b.max.y : b.min.y,
            .z = n.z >= 0.0f ? b.max.z : b.min.z,
        };
        if ( plane_distance( f.planes[ k ], pv ) < 0.0f ) return false;   // wholly outside this plane
    }
    return true;
}

// clang-format on
/*============================================================================================*/
#endif    // MATH_GEO_H
