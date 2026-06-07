/*==============================================================================================

    render_api.c -- render module wiring.
    Implements the render_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers
==============================================================================================*/

MOD_USE_CORE;
MOD_USE_RHI;

/*==============================================================================================
    Persistent state  (allocated and zeroed by the module system; preserved across reloads)
==============================================================================================*/

/* Per-context render slot.  Indexed by ctx_id directly (ctx_id is [0..RHI_CTX_MAX)). */
typedef struct render_ctx_slot_s
{
    bool               active;
    rhi_cmd_list_t cmd;       /* valid between begin_frame / end_frame; RHI_CMD_INVALID otherwise */
    rhi_color_t        clear;     /* default: dark charcoal */

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
    s->active    = true;
    s->cmd       = RHI_CMD_INVALID;
    s->clear.r   = 0.08f;
    s->clear.g   = 0.10f;
    s->clear.b   = 0.14f;
    s->clear.a   = 1.0f;
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

    /* Open the main pass against the swapchain; cleared to the slot's clear color. */
    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = s->clear,
    };
    rhi_depth_attachment_t depth_att = {
        .texture      = { .id = RHI_SWAPCHAIN_DEPTH },
        .load_op      = RHI_LOAD_OP_CLEAR,
        .store_op     = RHI_STORE_OP_DISCARD,
        .depth_clear  = 1.0f,
        .stencil_clear = 0,
    };
    rhi()->cmd_begin_rendering( s->cmd, &color_att, 1, &depth_att );
    return true;
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

    /* TODO: submit scene draw calls for this context.
       rhi()->cmd_bind_bindless( s->cmd )
       -- iterate scene draw list, bind pipelines, push constants, draw meshes
    */
}

static void
render_draw_editor_impl( i32 ctx_id, f32 dt )
{
    UNUSED( dt );

    if ( !g_state || ctx_id < 0 || ctx_id >= RHI_CTX_MAX )
        return;

    render_ctx_slot_t* s = &g_state->ctx[ ctx_id ];
    if ( !s->active || !rhi_cmd_valid( s->cmd ) )
        return;

    /* TODO: ImGui render pass for editor contexts.
       imgui_api()->render( s->cmd )
    */
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
        rhi()->cmd_end_rendering( s->cmd );
        rhi()->frame_end( ctx_id );
        s->cmd = RHI_CMD_INVALID;
    }
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
    .draw_editor        = render_draw_editor_impl,
    .end_frame          = render_end_frame_impl,
    .set_clear_color    = render_set_clear_color_impl,
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
        .deps          = { "core", "rhi" },
        .dep_count     = 2,
        .func_api      = &g_render_api_struct,
        .init          = render_init,
        .exit          = render_exit,
        .reload        = render_reload,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( render )

/*============================================================================================*/
