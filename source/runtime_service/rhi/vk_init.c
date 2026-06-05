/*==============================================================================================

    runtime/services/rhi/vk_init.c -- Vulkan backend global init/shutdown and
    render-context create/destroy.

    Global init owns the instance and device; it requires no window.
    Context create/destroy own everything per-window: surface, swapchain, sync,
    command pool/buffers.

    Included by rhi.c after all other vk_*.c files so the subsystem helpers
    (vk_instance_create, vk_surface_create, etc.) are visible.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Context pool helpers
==============================================================================================*/

static i32
vk_ctx_alloc( void )
{
    for ( int i = 0; i < RHI_CTX_MAX; ++i )
    {
        if ( !( vk.ctx_alloc & ( 1u << i ) ) )
        {
            vk.ctx_alloc |= ( 1u << i );
            return ( i32 )i;
        }
    }
    return RHI_CTX_INVALID;
}

static void
vk_ctx_free( i32 id )
{
    if ( id >= 0 && id < RHI_CTX_MAX )
        vk.ctx_alloc &= ~( 1u << id );
}

static vk_context_t*
vk_ctx_get( i32 id )
{
    if ( id < 0 || id >= RHI_CTX_MAX )
        return NULL;
    if ( !( vk.ctx_alloc & ( 1u << id ) ) )
        return NULL;
    return &vk.contexts[ id ];
}

/*==============================================================================================
    Global lifecycle  (instance + device, no window)
==============================================================================================*/

static bool
vk_init( void )
{
    if ( vk.initialized )
    {
        LOG_INFO( "init: already initialized\n" );
        return true;
    }

    LOG_LINE();
    LOG_INFO( "vk_instance_create..." );

    if ( vk.use_vk_alloc_cb ) {
        vk_allocation_callback_init();
    }
    
    if ( !vk_instance_init() ) 
        goto fail_after_nothing;

    /* turn on regular logging levels */
    vk_debug_set_min_level( LOG_LEVEL_WARN );

    LOG_LINE();
    LOG_INFO( "vk_device_create..." );

    if ( !vk_device_create() ) 
        goto fail_after_instance;

    vk.initialized = true;
    LOG_LINE();
    return true;

fail_after_instance:  vk_instance_destroy();
fail_after_nothing:

    LOG_ERROR( "vk_init failed\n" );
    LOG_LINE();
    return false;
}

static void
vk_shutdown( void )
{
    if ( !vk.initialized )
        return;

    LOG_LINE();
    LOG_INFO( "vk_shutdown..." );

    vk_device_wait_idle();
    vk_device_destroy();
    vk_instance_destroy();

    vk.initialized = false;    
    LOG_LINE();
}

/*==============================================================================================
    Per-context lifecycle  (surface + swapchain + sync + commands)
==============================================================================================*/

static i32
vk_context_create( i32 win_id, void* native_window, i32 w, i32 h )
{
    if ( !vk.initialized ) {
         LOG_ERROR( "[rhi] context_create: global init not done\n" );
         return RHI_CTX_INVALID;
    }
    if ( !native_window ) {
         LOG_ERROR( "[rhi] context_create: native_window is NULL\n" );
         return RHI_CTX_INVALID;
    }

    i32  id = vk_ctx_alloc();
    if ( id == RHI_CTX_INVALID )
    {
        LOG_ERROR( "[rhi] context_create: pool full (max %d)\n", RHI_CTX_MAX );
        return RHI_CTX_INVALID;
    }

    vk_context_t* ctx  = &vk.contexts[ id ];
    ctx->id            = id;
    ctx->win_id        = win_id;
    ctx->native_window = native_window;
    ctx->width         = w;
    ctx->height        = h;
    ctx->current_frame = 0;
    ctx->resize_pending = false;

    printf( "[rhi] context_create begin (ctx %d, win %d)\n", id, win_id );

    /* Order matters: surface before swapchain, swapchain before sync/commands. */
    if ( !vk_surface_create( ctx )   ) goto fail_after_nothing;
    if ( !vk_swapchain_create( ctx ) ) goto fail_after_surface;
    if ( !vk_sync_create( ctx )      ) goto fail_after_swapchain;
    if ( !vk_command_create( ctx )   ) goto fail_after_sync;

    printf( "[rhi] context_create complete (ctx %d, win %d)\n", id, win_id );
    return id;

fail_after_sync:      vk_sync_destroy( ctx );
fail_after_swapchain: vk_swapchain_destroy( ctx );
fail_after_surface:   vk_surface_destroy( ctx );
fail_after_nothing:

    vk_ctx_free( id );
    memset( ctx, 0, sizeof( *ctx ) );
    printf( "[rhi] context_create failed (win %d)\n", win_id );
    return RHI_CTX_INVALID;
}

static void
vk_context_destroy( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return;

    printf( "[rhi] context_destroy begin (ctx %d, win %d)\n", ctx->id, ctx->win_id );

    /* Drain all queues before touching any context-owned Vulkan objects. */
    vk_device_wait_idle();

    vk_command_destroy( ctx );
    vk_sync_destroy( ctx );
    vk_swapchain_destroy( ctx );
    vk_surface_destroy( ctx );

    vk_ctx_free( ctx_id );
    memset( ctx, 0, sizeof( *ctx ) );

    printf( "[rhi] context_destroy complete (ctx %d)\n", ctx_id );
}

static bool
vk_context_resize( i32 ctx_id, i32 width, i32 height )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return false;
    if ( width <= 0 || height <= 0 )
        return false;

    ctx->width          = width;
    ctx->height         = height;
    ctx->resize_pending = true;

    /* Actual swapchain recreation happens at the top of the next frame_begin,
       between frames, never mid-recording. */
    return true;
}

/*============================================================================================*/
// clang-format on