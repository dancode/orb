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
    Platform helpers
==============================================================================================*/
/* Query whether the display driver stack supports variable refresh rate (GSync / FreeSync).
   Uses DXGI on Windows; returns false on all other platforms. */

#if OS_WINDOWS
static bool
vk_platform_check_vrr( void )
{
    /* IID_IDXGIFactory1 {770AAE78-F26F-4DBA-A829-253C83D1B387} */
    static const GUID s_iid_factory1 = {
        0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 }
    };
    /* IID_IDXGIFactory5 {7632E1F5-EE65-4DCA-87FD-84CD75F8838D} */
    static const GUID s_iid_factory5 = {
        0x7632e1f5, 0xee65, 0x4dca, { 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d }
    };

    IDXGIFactory1* factory1 = NULL;
    if ( FAILED( CreateDXGIFactory1( &s_iid_factory1, (void**)&factory1 ) ) )
        return false;

    IDXGIFactory5* factory5 = NULL;
    HRESULT hr = factory1->lpVtbl->QueryInterface( factory1, &s_iid_factory5, (void**)&factory5 );
    factory1->lpVtbl->Release( factory1 );
    if ( FAILED( hr ) || !factory5 )
        return false;

    BOOL allow_tearing = FALSE;
    hr = factory5->lpVtbl->CheckFeatureSupport(
        factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof( allow_tearing ) );
    factory5->lpVtbl->Release( factory5 );
    return SUCCEEDED( hr ) && ( allow_tearing == TRUE );
}
#else
static bool vk_platform_check_vrr( void ) { return false; }
#endif

/*==============================================================================================
    Global lifecycle  (instance + device, no window)
==============================================================================================*/

static bool
vk_init( void )
{
    if ( vk.initialized ) {
         LOG_INFO( "init: already initialized\n" );
         return true;
    }

    LOG_LINE();
    LOG_INFO( "vk_instance_create..." );

    vk.has_vrr = vk_platform_check_vrr();
    LOG_INFO( "VRR (GSync/FreeSync): %s", vk.has_vrr ? "detected" : "not detected" );

    if ( vk.use_vk_alloc_cb ) {
         vk_allocation_callback_init();
    }
    
    if ( !vk_instance_init() ) 
         goto fail_after_nothing;

    /* turn on regular vk logging levels */
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

    LOG_ERROR( "vk_init failed" );
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
         LOG_ERROR( "global init not done\n" );
         return RHI_CTX_INVALID;
    }
    if ( !native_window ) {
         LOG_ERROR( "native_window is NULL\n" );
         return RHI_CTX_INVALID;
    }

    i32  id = vk_ctx_alloc();
    if ( id == RHI_CTX_INVALID ) {
         LOG_ERROR( "pool full (max %d)\n", RHI_CTX_MAX );
         return RHI_CTX_INVALID;
    }

    vk_context_t* ctx   = &vk.contexts[ id ];
    ctx->id             = id;
    ctx->win_id         = win_id;
    ctx->native_window  = native_window;
    ctx->width          = w;
    ctx->height         = h;
    ctx->current_frame  = 0;
    ctx->resize_pending = false;

    LOG_LINE();
    LOG_INFO( "vk_context_create... (ctx %d, win %d)", id, win_id );

    /* Order matters: surface before swapchain, swapchain before sync/commands. */
    if ( !vk_surface_create( ctx ) ) 
         goto fail_after_nothing;

    /* Probe the surface extent before attempting swapchain creation. A window 
       that starts minimized reports currentExtent {0,0};  Defer the swapchain 
       to the first non-minimized frame_begin via resize_pending */

    VkSurfaceCapabilitiesKHR caps = { 0 };
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physical_device, ctx->surface, &caps );
    bool valid_extent = ( caps.currentExtent.width > 0 && caps.currentExtent.height > 0 );
    if ( valid_extent )
    {
        if ( !vk_swapchain_create( ctx, VK_NULL_HANDLE ) ) goto fail_after_surface;
    }
    else
    {
        LOG_INFO( "vk_swapchain_create: deferred on zero extent", id, win_id );
        ctx->resize_pending = true;
    }
    
    if ( !vk_sync_create( ctx )    ) goto fail_after_swapchain;
    if ( !vk_command_create( ctx ) ) goto fail_after_sync;

    LOG_INFO( "vk_context_create: complete (ctx %d, win %d)", id, win_id );
    LOG_LINE();

    return id;

fail_after_sync:      vk_sync_destroy( ctx );
fail_after_swapchain: vk_swapchain_destroy( ctx );
fail_after_surface:   vk_surface_destroy( ctx );
fail_after_nothing:

    vk_ctx_free( id );
    memset( ctx, 0, sizeof( *ctx ) );
    LOG_ERROR( "failed (win %d)", win_id );
    return RHI_CTX_INVALID;
}

static void
vk_context_destroy( i32 ctx_id )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return;

    LOG_INFO( "context_destroy: begin (ctx %d, win %d)", ctx->id, ctx->win_id );

    /* Drain all queues before touching any context-owned Vulkan objects. */
    vk_device_wait_idle();

    /* GPU work is complete: count this context as having checked in for the current epoch.
       If it was the last pending context, advance the epoch now.  Clear the stale bit after
       vk_ctx_free so epoch_ack_mask remains a subset of ctx_alloc going forward. */
    vk.epoch_ack_mask |= ( 1u << ctx_id );
    if ( vk.epoch_ack_mask == vk.ctx_alloc )
    {
        vk.global_epoch++;
        vk.epoch_ack_mask = 0;
    }

    vk_command_destroy( ctx );
    vk_sync_destroy( ctx );
    vk_swapchain_destroy( ctx );
    vk_surface_destroy( ctx );

    vk_ctx_free( ctx_id );
    vk.epoch_ack_mask &= ~( 1u << ctx_id );   /* remove stale bit if epoch did not reset above */
    memset( ctx, 0, sizeof( *ctx ) );

    LOG_INFO( "context_destroy: complete (ctx %d)", ctx_id );
}

static bool
vk_context_resize( i32 ctx_id, i32 width, i32 height )
{
    vk_context_t* ctx = vk_ctx_get( ctx_id );
    if ( !ctx )
        return false;
    if ( width <= 0 || height <= 0 )
        return false;

    /* Skip if the swapchain already matches -- suppresses the WM_SIZE fired during
       CreateWindowExW, which arrives after context_create and has the same dimensions. */
    if ( (u32)width  == ctx->swapchain_extent.width &&
         (u32)height == ctx->swapchain_extent.height )
        return true;

    ctx->width          = width;
    ctx->height         = height;
    ctx->resize_pending = true;

    /* Actual swapchain recreation happens at the top of the next frame_begin,
       between frames, never mid-recording. */
    return true;
}

/*============================================================================================*/
// clang-format on