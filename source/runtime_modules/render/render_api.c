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

typedef struct render_state_s
{
    render_ctx_slot_t  ctx[ RHI_CTX_MAX ];
    f32                total_time;

} render_state_t;

static render_state_t* g_state = NULL;

/*==============================================================================================
    Context management
==============================================================================================*/

static void
render_context_register_impl( i32 ctx_id )
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
render_context_unregister_impl( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    s->active = false;
    s->cmd    = RHI_CMD_INVALID;
}

/*==============================================================================================
    Frame
==============================================================================================*/

static bool
render_begin_frame_impl( i32 ctx_id )
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
render_submit_rect_impl( i32 ctx_id, f32 cx, f32 cy, f32 w, f32 h, const f32 rgba[ 4 ] )
{
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
render_draw_scene_impl( i32 ctx_id, f32 dt )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    if ( !s->active || !rhi_cmd_valid( s->cmd ) )
        return;

    g_state->total_time += dt;

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
render_end_frame_impl( i32 ctx_id )
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
render_frame_cmd_impl( i32 ctx_id )
{
    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return RHI_CMD_INVALID;

    return g_state->ctx[ ctx_id ].cmd;
}

static void
render_set_clear_color_impl( i32 ctx_id, f32 r, f32 g, f32 b, f32 a )
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
    .context_register   = render_context_register_impl,
    .context_unregister = render_context_unregister_impl,
    .begin_frame        = render_begin_frame_impl,
    .draw_scene         = render_draw_scene_impl,
    .end_frame          = render_end_frame_impl,
    .submit_rect        = render_submit_rect_impl,
    .set_clear_color    = render_set_clear_color_impl,
    .frame_cmd          = render_frame_cmd_impl,
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
