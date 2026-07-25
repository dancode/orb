/*==============================================================================================

    render_api.c -- render module wiring.
    Implements the render_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers
==============================================================================================*/

MOD_USE_CORE;
MOD_USE_RHI;
MOD_USE_DRAW;

/*==============================================================================================
    Persistent state  (allocated and zeroed by the module system; preserved across reloads)
==============================================================================================*/

/* Per-frame scene submission list (0.1 minimal).  Filled by submit_rect between frames,
   replayed and cleared by draw_scene.  Replaced later by a real scene / draw-list system. */
#define RENDER_MAX_RECTS 256

typedef struct render_rect_s
{
    f32 cx, cy, w, h;
    f32 rgba[ 4 ];

} render_rect_t;

/* Per-context render slot.  Indexed by ctx_id directly (ctx_id is [0..RHI_CTX_MAX)).
   Each context owns its submission list so multi-window hosts don't cross streams. */
typedef struct render_ctx_slot_s
{
    bool               active;
    rhi_cmd_t cmd;       /* valid between begin_frame / end_frame; RHI_CMD_INVALID otherwise */
    rhi_color_t        clear;     /* default: RHI_CLEAR_DEFAULT_* dark slate */

    render_rect_t      rects[ RENDER_MAX_RECTS ];
    i32                rect_count;

} render_ctx_slot_t;

/* Offscreen target slot.  Double-buffered color texture (no depth -- the 0.1 scene path is
   2D through draw()'s SOLID pipeline, which declares no depth attachment): the CPU records
   up to RHI_MAX_FRAMES_IN_FLIGHT ahead, so the buffer a still-in-flight frame samples must
   never be the one this frame writes.  cur flips via target_flip before the gui emit. */
typedef struct render_target_s
{
    bool          active;
    rhi_texture_t tex[ 2 ];
    u32           bindless[ 2 ];       /* rhi bindless indices; 0 = invalid            */
    bool          first_frame[ 2 ];    /* first write: old layout UNDEFINED, else READ */
    u32           cur;                 /* write/display buffer this frame              */
    i32           w, h;
    rhi_color_t   clear;

    render_rect_t rects[ RENDER_MAX_RECTS ];
    i32           rect_count;

} render_target_t;

typedef struct render_state_s
{
    render_ctx_slot_t  ctx[ RHI_CTX_MAX ];
    render_target_t    target[ RENDER_TARGET_MAX ];
    f32                total_time;

} render_state_t;

static render_state_t* g_state = NULL;

/*==============================================================================================
    Context management
==============================================================================================*/

static void
render_context_register( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    s->active     = true;
    s->cmd        = RHI_CMD_INVALID;
    s->rect_count = 0;
    s->clear.r    = RHI_CLEAR_DEFAULT_R;
    s->clear.g    = RHI_CLEAR_DEFAULT_G;
    s->clear.b    = RHI_CLEAR_DEFAULT_B;
    s->clear.a    = RHI_CLEAR_DEFAULT_A;
}

static void
render_context_unregister( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    s->active = false;
    s->cmd    = RHI_CMD_INVALID;
}

/*==============================================================================================
    Offscreen targets
==============================================================================================*/

/* target_id -> slot, or NULL when out of range / inactive. */
static render_target_t*
target_slot( i32 target_id )
{
    i32 i = target_id - RENDER_TARGET_ID_BASE;
    if ( !g_state || i < 0 || i >= RENDER_TARGET_MAX )
        return NULL;

    render_target_t* t = &g_state->target[ i ];
    return t->active ? t : NULL;
}

/* Release a slot's GPU resources.  Drains the device first: in-flight frames may still
   sample the old textures, and a rare recreate hitch beats a retire queue here. */
static void
target_release( render_target_t* t )
{
    if ( !t->bindless[ 0 ] && !t->bindless[ 1 ] )
        return;

    rhi()->device_wait_idle();
    for ( u32 i = 0; i < 2; i++ )
    {
        if ( t->bindless[ i ] )
        {
            rhi()->unregister_texture( t->bindless[ i ] );
            rhi()->texture_destroy( t->tex[ i ] );
            t->bindless[ i ] = 0;
            t->tex[ i ]      = ( rhi_texture_t ){ 0 };
        }
    }
    t->w = t->h = 0;
}

/* Create both buffers at (w,h); unwinds through target_release on any failure. */
static bool
target_alloc( render_target_t* t, i32 w, i32 h )
{
    for ( u32 i = 0; i < 2; i++ )
    {
        t->tex[ i ] = rhi()->texture_create( &( rhi_texture_desc_t ){
            .width        = ( u32 )w,
            .height       = ( u32 )h,
            .depth        = 1,
            .mip_levels   = 1,
            .array_layers = 1,
            .format       = RHI_FORMAT_BGRA8_SRGB,    /* matches draw()'s pipeline color target */
            .usage        = RHI_TEXTURE_USAGE_COLOR_ATTACHMENT | RHI_TEXTURE_USAGE_SAMPLED,
            .memory       = RHI_MEMORY_GPU_ONLY,
            .debug_name   = i ? "render_target_1" : "render_target_0",
        } );
        if ( !rhi_handle_valid( t->tex[ i ] ) )
        {
            LOG_ERROR( "target create failed (%dx%d)", w, h );
            target_release( t );
            return false;
        }

        t->bindless[ i ] = rhi()->register_texture( t->tex[ i ] );
        if ( t->bindless[ i ] == 0 )
        {
            rhi()->texture_destroy( t->tex[ i ] );
            t->tex[ i ] = ( rhi_texture_t ){ 0 };
            LOG_ERROR( "target bindless registration failed" );
            target_release( t );
            return false;
        }

        t->first_frame[ i ] = true;
    }

    t->w   = w;
    t->h   = h;
    t->cur = 0;
    return true;
}

static i32
render_target_create( i32 w, i32 h )
{
    if ( !g_state || w <= 0 || h <= 0 )
        return -1;

    for ( i32 i = 0; i < RENDER_TARGET_MAX; i++ )
    {
        render_target_t* t = &g_state->target[ i ];
        if ( t->active )
            continue;

        if ( !target_alloc( t, w, h ) )
            return -1;

        t->active     = true;
        t->rect_count = 0;
        t->clear.r    = RHI_CLEAR_DEFAULT_R;
        t->clear.g    = RHI_CLEAR_DEFAULT_G;
        t->clear.b    = RHI_CLEAR_DEFAULT_B;
        t->clear.a    = RHI_CLEAR_DEFAULT_A;

        LOG_INFO( "target %d created %dx%d", RENDER_TARGET_ID_BASE + i, w, h );
        return RENDER_TARGET_ID_BASE + i;
    }
    return -1;    /* pool exhausted */
}

static void
render_target_destroy( i32 target_id )
{
    render_target_t* t = target_slot( target_id );
    if ( !t )
        return;

    target_release( t );
    t->active     = false;
    t->rect_count = 0;
}

static bool
render_target_resize( i32 target_id, i32 w, i32 h )
{
    render_target_t* t = target_slot( target_id );
    if ( !t || w <= 0 || h <= 0 )
        return false;
    if ( w == t->w && h == t->h )
        return true;

    target_release( t );
    if ( !target_alloc( t, w, h ) )
    {
        t->active = false;
        return false;
    }

    LOG_INFO( "target %d resized %dx%d", target_id, w, h );
    return true;
}

static u32
render_target_texture( i32 target_id )
{
    render_target_t* t = target_slot( target_id );
    return t ? t->bindless[ t->cur ] : 0;
}

static void
render_target_flip( i32 target_id )
{
    render_target_t* t = target_slot( target_id );
    if ( t )
        t->cur ^= 1u;
}

static void
render_target_size( i32 target_id, i32* w, i32* h )
{
    render_target_t* t = target_slot( target_id );
    if ( w ) *w = t ? t->w : 0;
    if ( h ) *h = t ? t->h : 0;
}

/* Drain every pending target into its texture -- called at the top of draw_scene, before
   the swapchain pass, on the frame's open command list (no pass is open there).  Each
   target records its own pass; the closing barrier hands the buffer to the gui's sampler. */
static void
render_draw_targets( rhi_cmd_t cmd )
{
    for ( i32 i = 0; i < RENDER_TARGET_MAX; i++ )
    {
        render_target_t* t = &g_state->target[ i ];
        if ( !t->active || t->bindless[ 0 ] == 0 )
            continue;

        /* A never-written buffer must still get one clear pass: sampling an image that
           was never transitioned out of UNDEFINED is invalid, and a fresh target is
           displayed before the first play session ever submits. */
        if ( t->rect_count <= 0 && !t->first_frame[ t->cur ] )
            continue;

        u32 cur = t->cur;

        rhi()->cmd_image_barrier( cmd, &( rhi_image_barrier_t ){
            .texture    = t->tex[ cur ],
            .old_layout = t->first_frame[ cur ] ? RHI_LAYOUT_UNDEFINED : RHI_LAYOUT_SHADER_READ,
            .new_layout = RHI_LAYOUT_COLOR_ATTACHMENT,
        }, 1 );
        t->first_frame[ cur ] = false;

        rhi()->cmd_bind_bindless( cmd );
        rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
            .texture  = t->tex[ cur ],
            .load_op  = RHI_LOAD_OP_CLEAR,
            .store_op = RHI_STORE_OP_STORE,
            .clear    = t->clear,
        }, 1, NULL );

        rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
            .x = 0.0f, .y = 0.0f,
            .width     = ( f32 )t->w,
            .height    = ( f32 )t->h,
            .min_depth = 0.0f,
            .max_depth = 1.0f,
        } );
        rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){
            .x = 0, .y = 0, .width = t->w, .height = t->h,
        } );

        f32 vp[ 16 ];
        draw()->ortho_2d( vp, ( f32 )t->w, ( f32 )t->h );
        draw()->begin( cmd, vp );

        for ( i32 r = 0; r < t->rect_count; ++r )
        {
            render_rect_t* rc = &t->rects[ r ];
            draw()->rect( rc->cx, rc->cy, rc->w, rc->h, rc->rgba );
        }

        draw()->end();
        rhi()->cmd_end_rendering( cmd );

        rhi()->cmd_image_barrier( cmd, &( rhi_image_barrier_t ){
            .texture    = t->tex[ cur ],
            .old_layout = RHI_LAYOUT_COLOR_ATTACHMENT,
            .new_layout = RHI_LAYOUT_SHADER_READ,
        }, 1 );

        t->rect_count = 0;
    }
}

/*==============================================================================================
    Frame
==============================================================================================*/

static bool
render_begin_frame( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return false;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    if ( !s->active )
        return false;

    s->cmd = rhi()->frame_begin( ctx_id );
    if ( !rhi_cmd_valid( s->cmd ) )
        return false;

    /* The frame is open but no pass is -- draw_scene owns the scene pass (open, clear,
       draw, close) so the host can composite overlay passes (gui()->render) on this
       command list between draw_scene and end_frame. */
    return true;
}

static void
render_submit_rect( i32 ctx_id, f32 cx, f32 cy, f32 w, f32 h, const f32 rgba[ 4 ] )
{
    /* Offscreen target ids route to the target's own bucket -- drained by draw_scene's
       target pre-pass instead of the swapchain pass. */
    if ( ctx_id >= RENDER_TARGET_ID_BASE )
    {
        render_target_t* t = target_slot( ctx_id );
        if ( !t || t->rect_count >= RENDER_MAX_RECTS )
            return;

        render_rect_t* r = &t->rects[ t->rect_count++ ];
        r->cx        = cx;
        r->cy        = cy;
        r->w         = w;
        r->h         = h;
        r->rgba[ 0 ] = rgba[ 0 ];
        r->rgba[ 1 ] = rgba[ 1 ];
        r->rgba[ 2 ] = rgba[ 2 ];
        r->rgba[ 3 ] = rgba[ 3 ];
        return;
    }

    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    if ( s->rect_count >= RENDER_MAX_RECTS )
        return;

    render_rect_t* r = &s->rects[ s->rect_count++ ];
    r->cx        = cx;
    r->cy        = cy;
    r->w         = w;
    r->h         = h;
    r->rgba[ 0 ] = rgba[ 0 ];
    r->rgba[ 1 ] = rgba[ 1 ];
    r->rgba[ 2 ] = rgba[ 2 ];
    r->rgba[ 3 ] = rgba[ 3 ];
}

static void
render_draw_scene( i32 ctx_id, f32 dt )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    if ( !s->active || !rhi_cmd_valid( s->cmd ) )
        return;

    g_state->total_time += dt;

    /* Offscreen targets first: no pass is open yet, so each target records its own pass
       into this frame's command list before the swapchain pass below.  Their closing
       barriers make the textures samplable by the gui composite later this frame. */
    render_draw_targets( s->cmd );

    /* Open the scene pass against the swapchain; cleared to the slot's clear color.
       Color-only for now: the 0.1 scene is a 2D overlay drawn through the draw service,
       whose SOLID pipeline declares no depth attachment -- under dynamic rendering the
       pass and pipeline must match.  The depth attachment returns with a real 3D scene
       path (draw()->begin_depth + DRAW_DEPTH_FORMAT). */
    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = s->clear,
    };
    rhi()->cmd_begin_rendering( s->cmd, &color_att, 1, NULL );

    /* Replay this context's submission list through the draw service, then clear it.
       Viewport/scissor are dynamic state nothing has set on this command list yet. */
    if ( s->rect_count > 0 )
    {
        i32 w = 0, h = 0;
        if ( !rhi()->context_size( ctx_id, &w, &h ) || w <= 0 || h <= 0 )
        {
            s->rect_count = 0;
            rhi()->cmd_end_rendering( s->cmd );
            return;
        }

        rhi()->cmd_bind_bindless( s->cmd );
        rhi()->cmd_set_viewport( s->cmd, &( rhi_viewport_t ){
            .x = 0.0f, .y = 0.0f,
            .width     = ( f32 )w,
            .height    = ( f32 )h,
            .min_depth = 0.0f,
            .max_depth = 1.0f,
        } );
        rhi()->cmd_set_scissor( s->cmd, &( rhi_rect_t ){
            .x = 0, .y = 0, .width = w, .height = h,
        } );

        f32 vp[ 16 ];
        draw()->ortho_2d( vp, ( f32 )w, ( f32 )h );
        draw()->begin( s->cmd, vp );

        for ( i32 i = 0; i < s->rect_count; ++i )
        {
            render_rect_t* r = &s->rects[ i ];
            draw()->rect( r->cx, r->cy, r->w, r->h, r->rgba );
        }

        draw()->end();
        s->rect_count = 0;
    }

    /* Close the scene pass -- the frame stays open for composite passes until end_frame. */
    rhi()->cmd_end_rendering( s->cmd );
}

static void
render_end_frame( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    if ( !s->active )
        return;

    if ( rhi_cmd_valid( s->cmd ) )
    {
        /* draw_scene already closed the scene pass; just submit and present. */
        rhi()->frame_end( ctx_id );
        s->cmd = RHI_CMD_INVALID;
    }
}

static rhi_cmd_t
render_frame_cmd( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return RHI_CMD_INVALID;

    return g_state->ctx[ ctx_id ].cmd;
}

static void
render_set_clear_color( i32 ctx_id, f32 r, f32 g, f32 b, f32 a )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    s->clear.r = r;
    s->clear.g = g;
    s->clear.b = b;
    s->clear.a = a;
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const render_api_t g_render_api_struct = {
    .context_register   = render_context_register,
    .context_unregister = render_context_unregister,
    .begin_frame        = render_begin_frame,
    .draw_scene         = render_draw_scene,
    .end_frame          = render_end_frame,
    .submit_rect        = render_submit_rect,
    .set_clear_color    = render_set_clear_color,
    .frame_cmd          = render_frame_cmd,
    .target_create      = render_target_create,
    .target_destroy     = render_target_destroy,
    .target_resize      = render_target_resize,
    .target_texture     = render_target_texture,
    .target_flip        = render_target_flip,
    .target_size        = render_target_size,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
render_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( render_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )
        return false;

    if ( !MOD_FETCH_RHI )
    {
        LOG_ERROR( "failed to fetch rhi_api" );
        return false;
    }

    if ( !MOD_FETCH_DRAW )
    {
        LOG_ERROR( "failed to fetch draw_api" );
        return false;
    }

    return true;
}

static bool
render_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    g_state = ( render_state_t* )raw_state;

    if ( !MOD_FETCH_CORE )
        return false;

    if ( !MOD_FETCH_RHI )
    {
        LOG_ERROR( "failed to re-fetch rhi_api after reload" );
        return false;
    }

    if ( !MOD_FETCH_DRAW )
    {
        LOG_ERROR( "failed to re-fetch draw_api after reload" );
        return false;
    }

    LOG_INFO( "reloaded" );
    return true;
}

static void
render_exit( void* raw_state )
{
    UNUSED( raw_state );
    if ( core() )
        LOG_INFO( "exit" );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
render_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( render_state_t ),
        .func_api_size = sizeof( render_api_t ),
        .deps          = { "core", "rhi", "draw" },
        .dep_count     = 3,
        .func_api      = &g_render_api_struct,
        .init          = render_init,
        .exit          = render_exit,
        .reload        = render_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( render )

/*============================================================================================*/
