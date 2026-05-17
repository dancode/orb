/*==============================================================================================

    runtime/services/rhi/rhi_api.c — RHI API struct wiring + lifecycle.

    Included LAST by rhi.c. By this point all vk_*.c files have defined their
    static functions in the same translation unit, so this file can:
        - Orchestrate the init/shutdown ordering across subsystems
        - Assign vk_* functions to the rhi_api_t slots
        - Provide the mod_desc_t descriptor for mod_static_load

==============================================================================================*/

/*==============================================================================================
    rhi_init / rhi_shutdown — orchestrate the subsystem create/destroy order
==============================================================================================*/

static bool
rhi_init( void* native_window_handle )
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

    /* Order matters: instance → surface → device (needs surface for present
       support check) → swapchain → sync → command pool. */

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
rhi_shutdown( void )
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
rhi_resize( i32 width, i32 height )
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

/*==============================================================================================
    API Struct
==============================================================================================*/

const rhi_api_t g_rhi_api_struct = {
    /* Lifecycle */
    .init            = rhi_init,
    .shutdown        = rhi_shutdown,
    .resize          = rhi_resize,

    /* Frame */
    .frame_begin     = vk_frame_begin,
    .frame_end       = vk_frame_end,

    /* Commands */
    .cmd_clear_color = vk_cmd_clear_color,
};

/*==============================================================================================
    Module lifecycle  (called by the module system at mod_init_all time)
==============================================================================================*/

static bool
rhi_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( raw_state );
    UNUSED( get_api );
    /* Real device init happens in rhi_api()->init() once the host calls it with
       a window handle. Nothing to do here. */
    return true;
}

static void
rhi_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
    /* Defensive: if the host forgot to call shutdown(), do it now. */
    if ( g_vk.initialized )
        rhi_shutdown();
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
rhi_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = 0, /* singleton lives in vk_state.c's g_vk */
        .func_api_size = sizeof( rhi_api_t ),
        .dep_count     = 2,
        .deps          = { "sys", "app" },
        .func_api      = &g_rhi_api_struct,
        .init          = rhi_mod_init,
        .exit          = rhi_mod_exit,
        .reload        = NULL,
    };
    return &api;
}

/*============================================================================================*/
