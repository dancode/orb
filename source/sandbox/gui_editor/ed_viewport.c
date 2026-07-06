/*==============================================================================================

    sandbox/gui_editor/ed_viewport.c -- Scene viewport: offscreen target, UE-style camera, panel.

    The Scene panel displays a real offscreen render: the stub scene is drawn with draw()
    into an RHI color texture each frame (ed_viewport_render, called by the host inside the
    frame command list before the gui pass), and the panel shows it through the gui's
    image_texture widget (bindless RGBA sampling).

    The target tracks the panel's content size: the panel publishes want_w/h each build, and
    ed_viewport_maintain recreates the texture once the requested size has been stable for a
    few frames (so a live resize drag stretches the old image instead of thrashing GPU
    allocations).  Recreation drains the device first -- in-flight frames still sample the old
    texture, and a rare post-resize hitch beats a retire-queue here.

    Included by sb_gui_editor.c (unity build).

==============================================================================================*/
// clang-format off

#define ED_TARGET_MIN       16
#define ED_TARGET_MAX       4096
#define ED_RESIZE_SETTLE    8       /* frames the wanted size must hold before recreating */

/*==============================================================================================
    Column-major matrix helpers (Vulkan NDC: y down, z 0..1) -- base has no 3D camera math yet.
==============================================================================================*/

static void
ed_mat_mul( f32 out[ 16 ], const f32 a[ 16 ], const f32 b[ 16 ] )   /* out = a * b */
{
    f32 t[ 16 ];
    for ( int c = 0; c < 4; c++ )
        for ( int r = 0; r < 4; r++ )
            t[ c * 4 + r ] = a[ 0 * 4 + r ] * b[ c * 4 + 0 ]
                           + a[ 1 * 4 + r ] * b[ c * 4 + 1 ]
                           + a[ 2 * 4 + r ] * b[ c * 4 + 2 ]
                           + a[ 3 * 4 + r ] * b[ c * 4 + 3 ];
    memcpy( out, t, sizeof( t ) );
}

static void
ed_mat_perspective( f32 out[ 16 ], f32 fov_deg, f32 aspect, f32 zn, f32 zf )
{
    f32 t = 1.0f / tanf( fov_deg * 0.5f * 3.14159265f / 180.0f );
    memset( out, 0, 16 * sizeof( f32 ) );
    out[  0 ] = t / aspect;
    out[  5 ] = -t;                      /* Vulkan NDC y points down */
    out[ 10 ] = zf / ( zn - zf );
    out[ 11 ] = -1.0f;
    out[ 14 ] = ( zn * zf ) / ( zn - zf );
}

static void
ed_vec3_norm( f32 v[ 3 ] )
{
    f32 l = sqrtf( v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ] + v[ 2 ] * v[ 2 ] );
    if ( l > 1e-6f ) { v[ 0 ] /= l; v[ 1 ] /= l; v[ 2 ] /= l; }
}

static void
ed_vec3_cross( f32 out[ 3 ], const f32 a[ 3 ], const f32 b[ 3 ] )
{
    out[ 0 ] = a[ 1 ] * b[ 2 ] - a[ 2 ] * b[ 1 ];
    out[ 1 ] = a[ 2 ] * b[ 0 ] - a[ 0 ] * b[ 2 ];
    out[ 2 ] = a[ 0 ] * b[ 1 ] - a[ 1 ] * b[ 0 ];
}

static f32
ed_vec3_dot( const f32 a[ 3 ], const f32 b[ 3 ] )
{
    return a[ 0 ] * b[ 0 ] + a[ 1 ] * b[ 1 ] + a[ 2 ] * b[ 2 ];
}

static void
ed_mat_lookat( f32 out[ 16 ], const f32 eye[ 3 ], const f32 target[ 3 ], const f32 up[ 3 ] )
{
    f32 f[ 3 ] = { target[ 0 ] - eye[ 0 ], target[ 1 ] - eye[ 1 ], target[ 2 ] - eye[ 2 ] };
    ed_vec3_norm( f );
    f32 s[ 3 ];
    ed_vec3_cross( s, f, up );
    ed_vec3_norm( s );
    f32 u[ 3 ];
    ed_vec3_cross( u, s, f );

    out[  0 ] =  s[ 0 ]; out[  1 ] =  u[ 0 ]; out[  2 ] = -f[ 0 ]; out[  3 ] = 0.0f;
    out[  4 ] =  s[ 1 ]; out[  5 ] =  u[ 1 ]; out[  6 ] = -f[ 1 ]; out[  7 ] = 0.0f;
    out[  8 ] =  s[ 2 ]; out[  9 ] =  u[ 2 ]; out[ 10 ] = -f[ 2 ]; out[ 11 ] = 0.0f;
    out[ 12 ] = -ed_vec3_dot( s, eye );
    out[ 13 ] = -ed_vec3_dot( u, eye );
    out[ 14 ] =  ed_vec3_dot( f, eye );
    out[ 15 ] = 1.0f;
}

/*==============================================================================================
    Offscreen target lifecycle
==============================================================================================*/

static void ed_target_destroy( void );   /* forward: create's failure path unwinds through it */

static bool
ed_target_create( i32 w, i32 h )
{
    ed_target_t* t = &g_ed.target;

    for ( u32 i = 0; i < 2; i++ )
    {
        t->tex[ i ] = rhi()->texture_create( &( rhi_texture_desc_t ){
            .width        = (u32)w,
            .height       = (u32)h,
            .depth        = 1,
            .mip_levels   = 1,
            .array_layers = 1,
            .format       = RHI_FORMAT_BGRA8_SRGB,     /* matches draw()'s pipeline color target */
            .usage        = RHI_TEXTURE_USAGE_COLOR_ATTACHMENT | RHI_TEXTURE_USAGE_SAMPLED,
            .memory       = RHI_MEMORY_GPU_ONLY,
            .debug_name   = i ? "ed_scene_target_1" : "ed_scene_target_0",
        } );
        if ( !rhi_handle_valid( t->tex[ i ] ) )
        {
            ed_logf( ED_LOG_ERROR, "Scene target create failed (%dx%d)", w, h );
            ed_target_destroy();
            return false;
        }

        t->bindless_idx[ i ] = rhi()->register_texture( t->tex[ i ] );
        if ( t->bindless_idx[ i ] == 0 )
        {
            rhi()->texture_destroy( t->tex[ i ] );
            t->tex[ i ] = ( rhi_texture_t ){ 0 };
            ed_logf( ED_LOG_ERROR, "Scene target bindless registration failed" );
            ed_target_destroy();
            return false;
        }

        /* Transient depth buffer paired with this color target -- never sampled, never
           bindless-registered.  One per target so two in-flight frames don't share a depth
           image (mirrors the tex[] double-buffering flipped by ed_viewport_flip). */
        t->depth[ i ] = rhi()->texture_create( &( rhi_texture_desc_t ){
            .width        = (u32)w,
            .height       = (u32)h,
            .depth        = 1,
            .mip_levels   = 1,
            .array_layers = 1,
            .format       = DRAW_DEPTH_FORMAT,
            .usage        = RHI_TEXTURE_USAGE_DEPTH_ATTACHMENT,
            .memory       = RHI_MEMORY_GPU_ONLY,
            .debug_name   = i ? "ed_scene_depth_1" : "ed_scene_depth_0",
        } );
        if ( !rhi_handle_valid( t->depth[ i ] ) )
        {
            ed_logf( ED_LOG_ERROR, "Scene depth create failed (%dx%d)", w, h );
            ed_target_destroy();
            return false;
        }

        t->first_frame[ i ] = true;
    }

    t->w   = w;
    t->h   = h;
    t->cur = 0;
    return true;
}

static void
ed_target_destroy( void )
{
    ed_target_t* t = &g_ed.target;
    if ( !t->bindless_idx[ 0 ] && !t->bindless_idx[ 1 ] )
        return;
    rhi()->device_wait_idle();      /* in-flight frames may still sample the old textures */
    for ( u32 i = 0; i < 2; i++ )
    {
        if ( t->bindless_idx[ i ] )
        {
            rhi()->unregister_texture( t->bindless_idx[ i ] );
            rhi()->texture_destroy( t->tex[ i ] );
            t->bindless_idx[ i ] = 0;
            t->tex[ i ] = ( rhi_texture_t ){ 0 };
        }
        if ( rhi_handle_valid( t->depth[ i ] ) )
        {
            rhi()->texture_destroy( t->depth[ i ] );
            t->depth[ i ] = ( rhi_texture_t ){ 0 };
        }
    }
    t->w = t->h = 0;
}

bool
ed_viewport_init( void )
{
    return true;    /* target is created lazily once the panel publishes its first size */
}

void
ed_viewport_shutdown( void )
{
    ed_target_destroy();
}

/* Between frames: create the target on first size request; on a size change, wait until the
   request settles (drag released) then swap the texture. */
/* Flip the write/display target: the previous frame's texture stays untouched for the GPU
   frame still in flight that samples it.  Called by the host ONLY on frames that run the scene
   pass (paired 1:1 with ed_viewport_render, inside a gui emit so the Scene quad bakes the new
   index) -- on all other frames neither runs, the quad keeps sampling the same texture, and
   its content is never touched while referenced.  Keeping the flip off no-scene emits also
   keeps the Scene window's command hash stable, so the gui's retained cache can go clean. */
void
ed_viewport_flip( void )
{
    g_ed.target.cur ^= 1u;
}

/* Coarse change detector for the scene pass: hash everything ed_viewport_render reads --
   the entity pool, the selection, the camera pose, and the target identity (recreate swaps
   bindless indices; a resize changes w/h) -- and compare against the previous call.  One hash
   over a small pool beats instrumenting every mutation site (inspector edits, picks, menu
   add/delete, sim ticks all land here for free).

   Self-sustaining motion: the camera pose only advances inside the Scene panel's emit, so a
   glide/fly reads as "changed" on the NEXT frame's check, which forces that frame to emit and
   advance it again -- the chain runs until the pose stops moving (velocity damped to rest).
   The one cost is a single frame of latency between input and the scene pass seeing it. */
bool
ed_scene_changed( void )
{
    const u8* p;
    u32       h = 2166136261u;

    #define ED_HASH( ptr, bytes )                                    \
        for ( p = (const u8*)( ptr ); p < (const u8*)( ptr ) + ( bytes ); p++ ) \
            h = ( h ^ *p ) * 16777619u

    ED_HASH( g_ed.entities, sizeof( g_ed.entities ) );
    ED_HASH( &g_ed.selected, sizeof( g_ed.selected ) );

    /* Camera pose fields only (see ed_viewcam.h); tuning/dynamics/bookkeeping past them do
       not affect what the pass draws. */
    ED_HASH( &g_ed.cam.yaw,     sizeof( g_ed.cam.yaw )     );
    ED_HASH( &g_ed.cam.pitch,   sizeof( g_ed.cam.pitch )   );
    ED_HASH( &g_ed.cam.dist,    sizeof( g_ed.cam.dist )    );
    ED_HASH( g_ed.cam.target,   sizeof( g_ed.cam.target )  );
    ED_HASH( &g_ed.cam.fov_deg, sizeof( g_ed.cam.fov_deg ) );

    ED_HASH( g_ed.target.bindless_idx, sizeof( g_ed.target.bindless_idx ) );
    ED_HASH( &g_ed.target.w, sizeof( g_ed.target.w ) );
    ED_HASH( &g_ed.target.h, sizeof( g_ed.target.h ) );

    #undef ED_HASH

    static u32  s_prev_hash;
    static bool s_primed;          /* first call always reports changed */
    bool changed = !s_primed || h != s_prev_hash;
    s_prev_hash  = h;
    s_primed     = true;
    return changed;
}

void
ed_viewport_maintain( void )
{
    ed_target_t* t = &g_ed.target;

    i32 w = t->want_w, h = t->want_h;
    if ( w < ED_TARGET_MIN || h < ED_TARGET_MIN )
        return;                                   /* panel hidden or collapsed: keep what we have */
    if ( w > ED_TARGET_MAX ) w = ED_TARGET_MAX;
    if ( h > ED_TARGET_MAX ) h = ED_TARGET_MAX;

    if ( t->bindless_idx[ 0 ] == 0 )
    {
        ed_target_create( w, h );
        return;
    }

    if ( w == t->w && h == t->h )
    {
        t->stable_frames = 0;
        return;
    }

    if ( ++t->stable_frames >= ED_RESIZE_SETTLE )
    {
        ed_target_destroy();
        if ( ed_target_create( w, h ) )
            ed_logf( ED_LOG_INFO, "Scene target resized to %dx%d", w, h );
        t->stable_frames = 0;
    }
}

/*==============================================================================================
    Scene render -- draw() primitives into the offscreen target.

    Uses draw()->begin_depth: the offscreen pass carries its own depth buffer, so geometry
    occludes correctly at any orbit angle with no painter sort and no winding/cull dependency.
    (This is isolated to the viewport -- GUI and 2D draw() callers still use the depth-less
    DRAW_MAT_SOLID path.)
==============================================================================================*/

static void
ed_scene_grid( void )
{
    const f32 line [ 4 ] = { 0.28f, 0.30f, 0.34f, 1.0f };
    const f32 axis_x[ 4 ] = { 0.55f, 0.25f, 0.25f, 1.0f };
    const f32 axis_z[ 4 ] = { 0.25f, 0.35f, 0.60f, 1.0f };
    const f32 slab [ 4 ] = { 0.16f, 0.17f, 0.19f, 1.0f };

    draw()->box( 0.0f, -0.06f, 0.0f, 22.0f, 0.08f, 22.0f, slab );

    for ( i32 i = -10; i <= 10; i++ )
    {
        const f32* cx = ( i == 0 ) ? axis_z : line;   /* the i==0 line along Z is the Z axis */
        const f32* cz = ( i == 0 ) ? axis_x : line;
        draw()->box( (f32)i, 0.0f, 0.0f, 0.03f, 0.02f, 20.0f, cx );
        draw()->box( 0.0f, 0.0f, (f32)i, 20.0f, 0.02f, 0.03f, cz );
    }
}

void
ed_viewport_render( rhi_cmd_t cmd )
{
    ed_target_t* t = &g_ed.target;
    if ( t->bindless_idx[ 0 ] == 0 )
        return;

    u32 cur = t->cur;

    rhi()->cmd_image_barrier( cmd, &( rhi_image_barrier_t ){
        .texture    = t->tex[ cur ],
        .old_layout = t->first_frame[ cur ] ? RHI_LAYOUT_UNDEFINED : RHI_LAYOUT_SHADER_READ,
        .new_layout = RHI_LAYOUT_COLOR_ATTACHMENT,
    }, 1 );
    /* Depth is transient: cleared every pass, never sampled, never preserved -- so its prior
       contents are irrelevant and UNDEFINED is always the correct (and cheapest) old layout. */
    rhi()->cmd_image_barrier( cmd, &( rhi_image_barrier_t ){
        .texture    = t->depth[ cur ],
        .old_layout = RHI_LAYOUT_UNDEFINED,
        .new_layout = RHI_LAYOUT_DEPTH_ATTACHMENT,
    }, 1 );
    t->first_frame[ cur ] = false;

    rhi()->cmd_bind_bindless( cmd );
    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
        .texture  = t->tex[ cur ],
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { 0.09f, 0.10f, 0.12f, 1.0f },
    }, 1, &( rhi_depth_attachment_t ){
        .texture     = t->depth[ cur ],
        .load_op     = RHI_LOAD_OP_CLEAR,
        .store_op    = RHI_STORE_OP_DISCARD,   /* depth is never read after the pass */
        .depth_clear = 1.0f,
    } );

    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f, .width = (f32)t->w, .height = (f32)t->h,
        .min_depth = 0.0f, .max_depth = 1.0f,
    } );
    rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){ .x = 0, .y = 0, .width = t->w, .height = t->h } );

    /* Camera view-projection. */
    f32 eye[ 3 ];
    viewcam_eye( &g_ed.cam, eye );
    f32 up[ 3 ] = { 0.0f, 1.0f, 0.0f };
    f32 view[ 16 ], proj[ 16 ], vp[ 16 ];
    ed_mat_lookat( view, eye, g_ed.cam.target, up );
    ed_mat_perspective( proj, g_ed.cam.fov_deg, (f32)t->w / (f32)t->h, 0.1f, 200.0f );
    ed_mat_mul( vp, proj, view );

    draw()->begin_depth( cmd, vp );

    ed_scene_grid();

    /* With a real depth buffer, entities can be emitted in pool order -- no painter sort. */
    for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
    {
        const ed_entity_t* e = &g_ed.entities[ i ];
        if ( !e->used || !e->active )
            continue;
        bool sel = ( i == g_ed.selected );

        f32 sx = e->scale[ 0 ], sy = e->scale[ 1 ], sz = e->scale[ 2 ];
        if ( e->kind == ED_KIND_LIGHT )  { sx *= 0.5f; sy *= 0.5f; sz *= 0.5f; }
        if ( e->kind == ED_KIND_CAMERA ) { sz *= 1.6f; }

        /* Selection cue: a warm base pad under the entity.  A concentric shell would fully
           enclose (and with depth, occlude) the box; a footprint pad reads as "selected"
           without hiding the object. */
        if ( sel )
        {
            const f32 hl[ 4 ] = { 0.95f, 0.75f, 0.20f, 1.0f };
            f32 base_y = e->pos[ 1 ] - sy * 0.5f;
            draw()->box( e->pos[ 0 ], base_y, e->pos[ 2 ], sx * 1.5f, 0.06f, sz * 1.5f, hl );
        }

        f32 col[ 4 ] = { e->color[ 0 ], e->color[ 1 ], e->color[ 2 ], e->color[ 3 ] };
        if ( e->kind == ED_KIND_LIGHT )
        {
            col[ 0 ] *= e->intensity;  col[ 1 ] *= e->intensity;  col[ 2 ] *= e->intensity;
        }
        draw()->box( e->pos[ 0 ], e->pos[ 1 ], e->pos[ 2 ], sx, sy, sz, col );
    }

    draw()->end();
    rhi()->cmd_end_rendering( cmd );

    rhi()->cmd_image_barrier( cmd, &( rhi_image_barrier_t ){
        .texture    = t->tex[ cur ],
        .old_layout = RHI_LAYOUT_COLOR_ATTACHMENT,
        .new_layout = RHI_LAYOUT_SHADER_READ,
    }, 1 );
}

/*==============================================================================================
    Click-select: cursor ray vs entity bounds.
==============================================================================================*/

/* Slab-test a ray against an axis-aligned box; returns the near hit distance or -1. */
static f32
ed_ray_aabb( const f32 ro[ 3 ], const f32 rd[ 3 ], const f32 center[ 3 ], const f32 half[ 3 ] )
{
    f32 tmin = 0.0f, tmax = 1e9f;
    for ( i32 i = 0; i < 3; i++ )
    {
        f32 lo = center[ i ] - half[ i ], hi = center[ i ] + half[ i ];
        if ( fabsf( rd[ i ] ) < 1e-8f )
        {
            if ( ro[ i ] < lo || ro[ i ] > hi )
                return -1.0f;
            continue;
        }
        f32 t0 = ( lo - ro[ i ] ) / rd[ i ];
        f32 t1 = ( hi - ro[ i ] ) / rd[ i ];
        if ( t0 > t1 ) { f32 t = t0; t0 = t1; t1 = t; }
        if ( t0 > tmin ) tmin = t0;
        if ( t1 < tmax ) tmax = t1;
        if ( tmin > tmax )
            return -1.0f;
    }
    return tmin;
}

/* Pick the nearest entity under panel-space pixel (px,py); empty space clears the selection.
   Bounds mirror the render's per-kind scale tweaks so what you see is what you hit. */
static void
ed_viewport_pick( f32 px, f32 py, gui_rect_t r )
{
    f32 ro[ 3 ], fwd[ 3 ], right[ 3 ], up[ 3 ];
    viewcam_eye  ( &g_ed.cam, ro );
    viewcam_basis( &g_ed.cam, fwd, right, up );

    /* Pixel -> view ray: ndc in [-1,1], scaled by the frustum half-extents at unit depth.
       Screen y grows downward, so +ndy tilts the ray down (-up). */
    f32 th  = tanf( g_ed.cam.fov_deg * 0.5f * 3.14159265f / 180.0f );
    f32 ndx = ( ( px - r.x ) / r.w ) * 2.0f - 1.0f;
    f32 ndy = ( ( py - r.y ) / r.h ) * 2.0f - 1.0f;
    f32 kx  = ndx * th * ( r.w / r.h );
    f32 ky  = -ndy * th;

    f32 rd[ 3 ];
    for ( i32 i = 0; i < 3; i++ )
        rd[ i ] = fwd[ i ] + right[ i ] * kx + up[ i ] * ky;
    ed_vec3_norm( rd );

    i32 best   = -1;
    f32 best_t = 1e9f;
    for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
    {
        const ed_entity_t* e = &g_ed.entities[ i ];
        if ( !e->used || !e->active )
            continue;

        f32 half[ 3 ] = { e->scale[ 0 ] * 0.5f, e->scale[ 1 ] * 0.5f, e->scale[ 2 ] * 0.5f };
        if ( e->kind == ED_KIND_LIGHT )  { half[ 0 ] *= 0.5f; half[ 1 ] *= 0.5f; half[ 2 ] *= 0.5f; }
        if ( e->kind == ED_KIND_CAMERA ) { half[ 2 ] *= 1.6f; }

        f32 t = ed_ray_aabb( ro, rd, e->pos, half );
        if ( t >= 0.0f && t < best_t )
        {
            best_t = t;
            best   = i;
        }
    }

    if ( best != g_ed.selected )
    {
        g_ed.selected = best;
        if ( best >= 0 )
            ed_logf( ED_LOG_INFO, "Selected '%s'", g_ed.entities[ best ].name );
    }
}

/*==============================================================================================
    Scene panel -- the image + the viewcam controller (see ed_viewcam.h for the gesture set).
==============================================================================================*/

void
ed_viewport_panel( void )
{
    if ( !gui()->window_begin( "Scene", GUI_WIN_NOSCROLL ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    gui_vec2_t avail = gui()->content_avail();
    i32 w = (i32)avail.x;
    i32 h = (i32)avail.y;
    g_ed.target.want_w = w;
    g_ed.target.want_h = h;

    if ( g_ed.target.bindless_idx[ 0 ] && w >= ED_TARGET_MIN && h >= ED_TARGET_MIN )
    {
        gui_vec2_t pos = gui()->cursor_screen_pos();
        gui_rect_t r   = { pos.x, pos.y, (f32)w, (f32)h };

        /* Sample the texture this frame's scene pass writes (t->cur, flipped by the host on
           emitted frames); the other one belongs to the still-in-flight previous frame. */
        gui()->image_texture( g_ed.target.bindless_idx[ g_ed.target.cur ], (f32)w, (f32)h, 0 );

        /* Camera input: snapshot the gui io into the viewcam controller (ed_viewcam.h owns the
           gesture grammar and the motion feel); WASD and the arrow keys both map to the move
           axes, Q/E to down/up.  A clean left click comes back out as the select hook. */
        f32 mx, my;
        gui()->get_mouse_pos( &mx, &my );

        viewcam_input_t vin = {
            .dt          = g_ed.frame_dt,
            .mouse_x     = mx,
            .mouse_y     = my,
            .wheel       = gui()->get_mouse_wheel(),
            .hovered     = gui()->is_mouse_hovering_rect( r ),
            .lmb         = gui()->is_mouse_down( APP_MOUSE_LEFT ),
            .rmb         = gui()->is_mouse_down( APP_MOUSE_RIGHT ),
            .mmb         = gui()->is_mouse_down( APP_MOUSE_MIDDLE ),
            .lmb_clicked = gui()->is_mouse_clicked( APP_MOUSE_LEFT ),
            .rmb_clicked = gui()->is_mouse_clicked( APP_MOUSE_RIGHT ),
            .mmb_clicked = gui()->is_mouse_clicked( APP_MOUSE_MIDDLE ),
            .alt         = gui()->is_key_down( APP_KEY_LALT )   || gui()->is_key_down( APP_KEY_RALT ),
            .shift       = gui()->is_key_down( APP_KEY_LSHIFT ) || gui()->is_key_down( APP_KEY_RSHIFT ),
            .key_fwd     = gui()->is_key_down( APP_KEY_W ) || gui()->is_key_down( APP_KEY_UP ),
            .key_back    = gui()->is_key_down( APP_KEY_S ) || gui()->is_key_down( APP_KEY_DOWN ),
            .key_left    = gui()->is_key_down( APP_KEY_A ) || gui()->is_key_down( APP_KEY_LEFT ),
            .key_right   = gui()->is_key_down( APP_KEY_D ) || gui()->is_key_down( APP_KEY_RIGHT ),
            .key_up      = gui()->is_key_down( APP_KEY_E ),
            .key_down    = gui()->is_key_down( APP_KEY_Q ),
        };
        viewcam_update( &g_ed.cam, &vin );

        if ( g_ed.cam.click )
            ed_viewport_pick( g_ed.cam.click_x, g_ed.cam.click_y, r );

        /* Overlay readout, top-left over the image. */
        static const char* mode_names[] = { "EDIT", "PLAY", "PAUSED" };
        char ov[ 128 ];
        snprintf( ov, sizeof( ov ), " %s   %dx%d   RMB look+WASD  LMB drive/select  RMB+LMB pan  Alt+LMB orbit",
                  mode_names[ g_ed.mode ], g_ed.target.w, g_ed.target.h );
        gui()->draw_text_in( ( gui_rect_t ){ r.x + 4, r.y + 2, r.w - 8, gui()->line_h() },
                             GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                             GUI_COLOR( 0xE8, 0xE8, 0xF0, 0xC0 ), ov );
    }
    else
    {
        gui()->text_disabled( "Scene target initializing..." );
    }

    gui()->window_end();
}

// clang-format on
/*============================================================================================*/
