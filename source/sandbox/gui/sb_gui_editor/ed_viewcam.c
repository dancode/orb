/*==============================================================================================

    sandbox/gui/sb_gui_editor/ed_viewcam.c -- Viewport camera controller implementation.

    See ed_viewcam.h for the gesture grammar.  Self-contained: math.h only, no engine
    services -- the caller feeds a viewcam_input_t snapshot and reads the pose back out.

    Included by sb_gui_editor.c (unity build).

==============================================================================================*/
// clang-format off

#define VC_CLICK_THRESH   3.0f    /* px an LMB press may wander and still count as a click */
#define VC_PITCH_LIMIT    1.50f   /* rad, shy of the poles so the look-at basis never flips */

/*==============================================================================================
    Small vector helpers (local: the unit stays dependency-free)
==============================================================================================*/

static void
vc_norm( f32 v[ 3 ] )
{
    f32 l = sqrtf( v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ] + v[ 2 ] * v[ 2 ] );
    if ( l > 1e-6f ) { v[ 0 ] /= l; v[ 1 ] /= l; v[ 2 ] /= l; }
}

static void
vc_cross( f32 out[ 3 ], const f32 a[ 3 ], const f32 b[ 3 ] )
{
    out[ 0 ] = a[ 1 ] * b[ 2 ] - a[ 2 ] * b[ 1 ];
    out[ 1 ] = a[ 2 ] * b[ 0 ] - a[ 0 ] * b[ 2 ];
    out[ 2 ] = a[ 0 ] * b[ 1 ] - a[ 1 ] * b[ 0 ];
}

/* Frame-rate independent ease factor: the fraction of remaining distance covered this frame
   at rate r (1/sec).  1 - exp(-r*dt) so two 8ms frames land where one 16ms frame would. */
static f32
vc_ease( f32 rate, f32 dt )
{
    f32 a = 1.0f - expf( -rate * dt );
    return ( a < 0.0f ) ? 0.0f : ( a > 1.0f ) ? 1.0f : a;
}

static void
vc_clamp_pitch( viewcam_t* c )
{
    if ( c->pitch >  VC_PITCH_LIMIT ) c->pitch =  VC_PITCH_LIMIT;
    if ( c->pitch < -VC_PITCH_LIMIT ) c->pitch = -VC_PITCH_LIMIT;
}

static void
vc_clamp_dist( viewcam_t* c )
{
    if ( c->dist < c->min_dist ) c->dist = c->min_dist;
    if ( c->dist > c->max_dist ) c->dist = c->max_dist;
}

/*==============================================================================================
    Public pose readers
==============================================================================================*/

void
viewcam_eye( const viewcam_t* c, f32 out_eye[ 3 ] )
{
    f32 cp = cosf( c->pitch ), sp = sinf( c->pitch );
    f32 cy = cosf( c->yaw ),   sy = sinf( c->yaw );
    out_eye[ 0 ] = c->target[ 0 ] + c->dist * cp * cy;
    out_eye[ 1 ] = c->target[ 1 ] + c->dist * sp;
    out_eye[ 2 ] = c->target[ 2 ] + c->dist * cp * sy;
}

void
viewcam_basis( const viewcam_t* c, f32 fwd[ 3 ], f32 right[ 3 ], f32 up[ 3 ] )
{
    f32 cp = cosf( c->pitch ), sp = sinf( c->pitch );
    f32 cy = cosf( c->yaw ),   sy = sinf( c->yaw );
    fwd[ 0 ] = -cp * cy;  fwd[ 1 ] = -sp;  fwd[ 2 ] = -cp * sy;   /* eye -> target */
    vc_cross( right, fwd, ( f32[ 3 ] ){ 0.0f, 1.0f, 0.0f } );
    vc_norm( right );
    vc_cross( up, right, fwd );
}

/* Re-seat the focus point so the given eye stays fixed under the current yaw/pitch/dist --
   the core of every rotate-in-place gesture. */
static void
vc_target_from_eye( viewcam_t* c, const f32 eye[ 3 ] )
{
    f32 cp = cosf( c->pitch ), sp = sinf( c->pitch );
    f32 cy = cosf( c->yaw ),   sy = sinf( c->yaw );
    c->target[ 0 ] = eye[ 0 ] - c->dist * cp * cy;
    c->target[ 1 ] = eye[ 1 ] - c->dist * sp;
    c->target[ 2 ] = eye[ 2 ] - c->dist * cp * sy;
}

/*==============================================================================================
    Init
==============================================================================================*/

void
viewcam_init( viewcam_t* c )
{
    memset( c, 0, sizeof( *c ) );

    c->yaw     = 0.7f;
    c->pitch   = 0.5f;
    c->dist    = 14.0f;
    c->fov_deg = 55.0f;

    c->look_sens     = 0.0040f;
    c->orbit_sens    = 0.0080f;
    c->drive_sens    = 0.0040f;
    c->drive_move    = 0.0040f;
    c->pan_scale     = 0.0016f;
    c->dolly_scale   = 0.0060f;
    c->fly_speed     = 15.0f;
    c->fly_speed_min = 0.5f;
    c->fly_speed_max = 128.0f;
    c->fly_boost     = 3.0f;
    c->fly_accel     = 10.0f;
    c->fly_decel     = 18.0f;
    c->look_smooth   = 0.0f; // 25.0f; -- smoothing is nice but it makes the camera feel less responsive
    c->min_dist      = 2.0f;
    c->max_dist      = 80.0f;
}

/*==============================================================================================
    Update
==============================================================================================*/

void
viewcam_update( viewcam_t* c, const viewcam_input_t* in )
{
    f32 dt = in->dt;
    if ( dt < 0.0f )     dt = 0.0f;
    if ( dt > 0.1f )     dt = 0.1f;      /* stall clamp: never integrate a debugger pause */

    c->click = false;

    /*------------------------------------------------------------------------------------
        Capture edges.  A press must start hovered; a captured button then owns the camera
        until release even off-panel.  Chords form in either order.
    ------------------------------------------------------------------------------------*/

    bool was_cap = c->cap_l || c->cap_r || c->cap_m;

    if ( in->hovered && in->rmb_clicked )
    {
        c->cap_r = true;
        if ( in->lmb && c->cap_l )
            c->l_chord = true;           /* held LMB press upgrades into the pan chord */
    }
    if ( in->hovered && in->lmb_clicked )
    {
        c->cap_l   = true;
        c->l_moved = false;
        c->l_chord = c->cap_r;
        c->l_alt   = in->alt;            /* latched: the press decides orbit vs drive/click */
        c->press_x = in->mouse_x;
        c->press_y = in->mouse_y;
    }
    if ( in->hovered && in->mmb_clicked )
        c->cap_m = true;

    if ( c->cap_l && c->cap_r )
        c->l_chord = true;

    if ( c->cap_l && !in->lmb )
    {
        /* A press that never wandered, chorded, or orbited is the host's select click. */
        if ( !c->l_moved && !c->l_chord && !c->l_alt )
        {
            c->click   = true;
            c->click_x = in->mouse_x;
            c->click_y = in->mouse_y;
        }
        c->cap_l = false;
    }
    if ( !in->rmb ) c->cap_r = false;
    if ( !in->mmb ) c->cap_m = false;

    bool cap = c->cap_l || c->cap_r || c->cap_m;
    if ( cap && !was_cap )
    {
        c->last_x = in->mouse_x;
        c->last_y = in->mouse_y;
        c->sdx = c->sdy = 0.0f;
    }

    /*------------------------------------------------------------------------------------
        Mouse deltas + click threshold + rotational smoothing.
    ------------------------------------------------------------------------------------*/

    f32 dx = 0.0f, dy = 0.0f;
    if ( cap )
    {
        dx = in->mouse_x - c->last_x;
        dy = in->mouse_y - c->last_y;
        c->last_x = in->mouse_x;
        c->last_y = in->mouse_y;
    }

    if ( c->cap_l && !c->l_moved )
    {
        f32 px = in->mouse_x - c->press_x;
        f32 py = in->mouse_y - c->press_y;
        if ( px * px + py * py > VC_CLICK_THRESH * VC_CLICK_THRESH )
            c->l_moved = true;
    }

    /* Light exponential smoothing on rotational deltas only -- pan/dolly stay 1:1 with the
       cursor so tracked content does not swim. */
    f32 sa  = ( c->look_smooth > 0.0f ) ? vc_ease( c->look_smooth, dt ) : 1.0f;
    c->sdx += ( dx - c->sdx ) * sa;
    c->sdy += ( dy - c->sdy ) * sa;

    f32 fwd[ 3 ], right[ 3 ], up[ 3 ];
    viewcam_basis( c, fwd, right, up );

    /*------------------------------------------------------------------------------------
        Gesture dispatch (one gesture per frame, most specific first).
    ------------------------------------------------------------------------------------*/

    bool pan   = ( c->cap_l && c->cap_r ) || c->cap_m;
    bool dolly = !pan && c->cap_r && in->alt;
    bool look  = !pan && !dolly && c->cap_r;
    bool orbit = !pan && !c->cap_r && c->cap_l && c->l_alt;
    bool drive = !pan && !c->cap_r && c->cap_l && !c->l_alt && c->l_moved;

    /* Fly keys ride any non-Alt capture (UE: WASD moves with LMB, RMB, or both held). */
    c->flying    = look || drive || pan || ( c->cap_l && !c->l_alt && !c->l_moved );
    c->capturing = cap;

    if ( pan && ( dx != 0.0f || dy != 0.0f ) )
    {
        /* Cursor-follow (drag right moves the view right), scaled with distance;
           pan_invert flips both axes back to grab-the-world. */
        f32 k = c->dist * c->pan_scale * ( c->pan_invert ? -1.0f : 1.0f );
        for ( i32 i = 0; i < 3; i++ )
            c->target[ i ] += ( right[ i ] * dx - up[ i ] * dy ) * k;
    }
    else if ( dolly && ( dx != 0.0f || dy != 0.0f ) )
    {
        /* Drag down/right backs away from the focus point. */
        c->dist *= 1.0f + ( dx + dy ) * c->dolly_scale;
        vc_clamp_dist( c );
    }
    else if ( look && ( c->sdx != 0.0f || c->sdy != 0.0f ) )
    {
        /* Look around: the eye stays pinned, yaw/pitch re-aim, the focus point swings. */
        f32 eye[ 3 ];
        viewcam_eye( c, eye );
        c->yaw   += c->sdx * c->look_sens;
        c->pitch += c->sdy * c->look_sens;
        vc_clamp_pitch( c );
        vc_target_from_eye( c, eye );
    }
    else if ( orbit && ( c->sdx != 0.0f || c->sdy != 0.0f ) )
    {
        c->yaw   += c->sdx * c->orbit_sens;
        c->pitch += c->sdy * c->orbit_sens;
        vc_clamp_pitch( c );
    }
    else if ( drive && ( c->sdx != 0.0f || c->sdy != 0.0f ) )
    {
        /* UE LMB drive: horizontal turns level about the eye, vertical walks the ground
           plane along the flattened forward (pitch untouched, so the horizon holds). */
        f32 eye[ 3 ];
        viewcam_eye( c, eye );
        c->yaw += c->sdx * c->drive_sens;

        f32 flat[ 3 ] = { -cosf( c->yaw ), 0.0f, -sinf( c->yaw ) };
        f32 step      = -c->sdy * c->dist * c->drive_move;
        eye[ 0 ] += flat[ 0 ] * step;
        eye[ 2 ] += flat[ 2 ] * step;
        vc_target_from_eye( c, eye );
    }

    /*------------------------------------------------------------------------------------
        Fly: velocity eases toward the wish direction and damps to rest -- integrated every
        frame (not just while flying) so releasing RMB glides out instead of freezing.
    ------------------------------------------------------------------------------------*/

    f32 wish[ 3 ] = { 0.0f, 0.0f, 0.0f };
    if ( c->flying )
    {
        if ( in->key_fwd   ) for ( i32 i = 0; i < 3; i++ ) wish[ i ] += fwd  [ i ];
        if ( in->key_back  ) for ( i32 i = 0; i < 3; i++ ) wish[ i ] -= fwd  [ i ];
        if ( in->key_right ) for ( i32 i = 0; i < 3; i++ ) wish[ i ] += right[ i ];
        if ( in->key_left  ) for ( i32 i = 0; i < 3; i++ ) wish[ i ] -= right[ i ];
        if ( in->key_up    ) wish[ 1 ] += 1.0f;
        if ( in->key_down  ) wish[ 1 ] -= 1.0f;
    }

    bool wants = ( wish[ 0 ] != 0.0f || wish[ 1 ] != 0.0f || wish[ 2 ] != 0.0f );
    if ( wants )
    {
        if ( c->cap_l )
            c->l_chord = true;    /* flying during an LMB press is a move, not a select click */
        vc_norm( wish );
        f32 spd = c->fly_speed * ( in->shift ? c->fly_boost : 1.0f );
        for ( i32 i = 0; i < 3; i++ )
            wish[ i ] *= spd;
    }

    f32 fa = vc_ease( wants ? c->fly_accel : c->fly_decel, dt );
    for ( i32 i = 0; i < 3; i++ )
        c->vel[ i ] += ( wish[ i ] - c->vel[ i ] ) * fa;

    f32 v2 = c->vel[ 0 ] * c->vel[ 0 ] + c->vel[ 1 ] * c->vel[ 1 ] + c->vel[ 2 ] * c->vel[ 2 ];
    if ( !wants && v2 < 1e-4f )
    {
        c->vel[ 0 ] = c->vel[ 1 ] = c->vel[ 2 ] = 0.0f;
    }
    else
    {
        for ( i32 i = 0; i < 3; i++ )
            c->target[ i ] += c->vel[ i ] * dt;     /* moving the eye = moving the focus */
    }

    /*------------------------------------------------------------------------------------
        Wheel: fly speed while flying, orbit-distance zoom otherwise.
    ------------------------------------------------------------------------------------*/

    f32 wheel = ( in->hovered || c->cap_r ) ? in->wheel : 0.0f;
    if ( wheel != 0.0f )
    {
        if ( c->flying )
        {
            c->fly_speed *= ( wheel > 0.0f ) ? 1.25f : 0.80f;
            if ( c->fly_speed < c->fly_speed_min ) c->fly_speed = c->fly_speed_min;
            if ( c->fly_speed > c->fly_speed_max ) c->fly_speed = c->fly_speed_max;
        }
        else
        {
            c->dist *= ( wheel > 0.0f ) ? 0.90f : 1.11f;
            vc_clamp_dist( c );
        }
    }
}

// clang-format on
/*============================================================================================*/
