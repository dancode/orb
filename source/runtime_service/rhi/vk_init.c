/*==============================================================================================

    runtime/services/rhi/vk_init.c -- Vulkan backend init / shutdown / resize.

    Orchestrates the subsystem create/destroy order. Included by rhi.c after all
    other vk_*.c files so vk_instance_create, vk_device_create, etc. are visible.

==============================================================================================*/

static bool
vk_init( void* native_window_handle )
{
    if ( g_vk.initialized )
    {
        printf( "[rhi] init called but already initialized\n" );
        return true;
    }

    if ( !native_window_handle )
    {
        printf( "[rhi] init: native_window_handle is NULL\n" );
        return false;
    }

    g_vk.native_window = native_window_handle;
    g_vk.current_frame = 0;
    g_vk.width         = 0;
    g_vk.height        = 0;

    printf( "[rhi] init begin (Vulkan placeholder backend)\n" );

    /* Order matters: instance -> surface -> device (needs surface for present
       support check) -> swapchain -> sync -> command pool. */

    if ( !vk_instance_create() )   goto fail_after_nothing;
    if ( !vk_surface_create() )    goto fail_after_instance;
    if ( !vk_device_create() )     goto fail_after_surface;
    if ( !vk_swapchain_create() )  goto fail_after_device;
    if ( !vk_sync_create() )       goto fail_after_swapchain;
    if ( !vk_command_create() )    goto fail_after_sync;

    g_vk.initialized = true;
    printf( "[rhi] init complete\n" );
    return true;

    /* Tear down only what was successfully created, in reverse order. */
fail_after_sync:       vk_sync_destroy();
fail_after_swapchain:  vk_swapchain_destroy();
fail_after_device:     vk_device_destroy();
fail_after_surface:    vk_surface_destroy();
fail_after_instance:   vk_instance_destroy();
fail_after_nothing:

    g_vk.native_window = NULL;
    printf( "[rhi] init failed\n" );
    return false;
}

static void
vk_shutdown( void )
{
    if ( !g_vk.initialized )
        return;

    printf( "[rhi] shutdown begin\n" );

    /* TODO: vkDeviceWaitIdle once device is real, before destroying anything. */

    vk_command_destroy();
    vk_sync_destroy();
    vk_swapchain_destroy();
    vk_device_destroy();
    vk_surface_destroy();
    vk_instance_destroy();

    g_vk.initialized = false;
    g_vk.native_window = NULL;
    printf( "[rhi] shutdown complete\n" );
}

static bool
vk_resize( i32 width, i32 height )
{
    if ( !g_vk.initialized )
        return false;

    if ( width <= 0 || height <= 0 )
        return false;

    g_vk.width          = width;
    g_vk.height         = height;
    g_vk.resize_pending = true;

    /* Actual recreation happens on the next frame_begin, between frames,
       never mid-recording. */
    return true;
}

/*============================================================================================*/
