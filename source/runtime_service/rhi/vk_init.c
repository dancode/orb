/*==============================================================================================

    runtime/services/rhi/vk_init.c -- Vulkan backend global init/shutdown and
    render-context create/destroy.

    Global init owns the instance and device; it requires no window.
    Context create/destroy own everything per-window: surface, swapchain, sync,
    command pool/buffers.

    Included by rhi.c after all other vk_*.c files so the subsystem helpers
    (vk_instance_create, vk_surface_create, etc.) are visible.

==============================================================================================*/

/*==============================================================================================
    Context pool helpers
==============================================================================================*/

static i32
vk_ctx_alloc( void )
{
    for ( int i = 0; i < RHI_CTX_MAX; ++i )
    {
        if ( !( g_vk.ctx_alloc & ( 1u << i ) ) )
        {
            g_vk.ctx_alloc |= ( 1u << i );
            return ( i32 )i;
        }
    }
    return RHI_CTX_INVALID;
}

static void
vk_ctx_free( i32 id )
{
    if ( id >= 0 && id < RHI_CTX_MAX )
        g_vk.ctx_alloc &= ~( 1u << id );
}

static vk_context_t*
vk_ctx_get( i32 id )
{
    if ( id < 0 || id >= RHI_CTX_MAX )
        return NULL;
    if ( !( g_vk.ctx_alloc & ( 1u << id ) ) )
        return NULL;
    return &g_vk.contexts[ id ];
}

/*==============================================================================================
    Global lifecycle  (instance + device, no window)
==============================================================================================*/

static bool
vk_init( void )
{
    if ( g_vk.initialized )
    {
        printf( "[rhi] init: already initialized\n" );
        return true;
    }

    printf( "[rhi] init begin\n" );

    /* Order: instance first, then device.
       NOTE: In real Vulkan, physical device selection queries present support
       against a specific VkSurfaceKHR. The standard workaround is to select a
       device whose queue family advertises support for the target platform
       surface type (checked via vkGetPhysicalDeviceSurfaceSupportKHR with a
       temporary surface, or via platform-specific caps). The first
       vk_context_create call will validate the choice against a real surface. */

    if ( !vk_instance_create() ) goto fail_after_nothing;
    if ( !vk_device_create()   ) goto fail_after_instance;

    g_vk.initialized = true;
    printf( "[rhi] init complete\n" );
    return true;

fail_after_instance:  vk_instance_destroy();
fail_after_nothing:
    printf( "[rhi] init failed\n" );
    return false;
}

static void
vk_shutdown( void )
{
    if ( !g_vk.initialized )
        return;

    printf( "[rhi] shutdown begin\n" );

    /* TODO: vkDeviceWaitIdle before destroying device-owned objects. */

    vk_device_destroy();
    vk_instance_destroy();

    g_vk.initialized = false;
    printf( "[rhi] shutdown complete\n" );
}

/*==============================================================================================
    Per-context lifecycle  (surface + swapchain + sync + commands)
==============================================================================================*/

static i32
vk_context_create( i32 win_id, void* native_window, i32 w, i32 h )
{
    if ( !g_vk.initialized )
    {
        printf( "[rhi] context_create: global init not done\n" );
        return RHI_CTX_INVALID;
    }
    if ( !native_window )
    {
        printf( "[rhi] context_create: native_window is NULL\n" );
        return RHI_CTX_INVALID;
    }

    i32 id = vk_ctx_alloc();
    if ( id == RHI_CTX_INVALID )
    {
        printf( "[rhi] context_create: pool full (max %d)\n", RHI_CTX_MAX );
        return RHI_CTX_INVALID;
    }

    vk_context_t* ctx  = &g_vk.contexts[ id ];
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

    /* TODO: vkDeviceWaitIdle before destroying any Vulkan objects. */

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
