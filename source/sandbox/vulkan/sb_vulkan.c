/*==============================================================================================

    sandbox/vulkan/sb_vulkan.c -- Vulkan RHI bring-up test.

    Loads sys + app + rhi + draw (static), opens a window, then exercises the pipeline.
    Currently uses sb_vk_boot to render a hardcoded triangle until the window is closed
    or ESC is pressed.  No runtime host, no module hot-reload.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/imgui/imgui_host.h"
#include "sb_vulkan_boot.h"

// clang-format off
/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    /* Load modules. */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( draw ); 
    mod_static( imgui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_vulkan] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    assert( sys() );
    assert( ref() );
    assert( app() );
    assert( core() );
    assert( rhi() );
    assert( draw() );
    assert( imgui() );

    core()->log_set_min_level( LOG_LEVEL_TRACE );
    core_log_fn( LOG_LEVEL_DEBUG, "sb_vulkan", "debug log: modules loaded successfully" );

    LOG_LINE();

    /* ------------------------------------------------------------------------------ */
    /* Setup Window + RHI */

    /* Open window. */
    win_id_t win = app()->window_open( "sb_vulkan", 0, 0, 1280, 720, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID ) {
         mod_system_exit();
         return 1;
    }

    /* Global RHI init (instance + device). */
    if ( !rhi()->init() ) {
         app()->window_close( win );
         mod_system_exit();
         return 1;
    }

    /* Per-window render context. */
    void* hwnd = app()->window_handle( win );
    i32  ctx   = rhi()->context_create( win, hwnd, 1280, 720 );
    if ( ctx == RHI_CTX_INVALID ) {
         rhi()->shutdown();
         app()->window_close( win );
         mod_system_exit();
         return 1;
    }

    /* ------------------------------------------------------------------------------ */
    /* Setup Resources */

    const bool b_use_boot = false;  // skip bootstrap triangle pipeline

    /* Initialize draw GPU resources (buffers + pipelines) now that the device is live. */
    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_vulkan] draw->init failed\n" );
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }

    sb_vk_boot_t boot = { 0 };
    if ( b_use_boot )
    {
        if ( !sb_vk_boot_create( &boot ) )
        {
            draw()->shutdown();
            rhi()->context_destroy( ctx );
            rhi()->shutdown();
            app()->window_close( win );
            mod_system_exit();
            return 1;
        }
    }

    /* Initialize imgui GPU resources and load TrueType font atlas. */
    if ( !imgui()->init() )
    {
        fprintf( stderr, "[sb_vulkan] imgui->init failed\n" );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        rhi()->shutdown();
        app()->window_close( win );
        mod_system_exit();
        return 1;
    }
    
    imgui()->load_font( "fonts/jetbrains_regular_24.orb_font" );
    // imgui()->load_font( "fonts/jetbrains_bold_24.orb_font" );
    
    imgui()->print_mem_stats();

    /* ------------------------------------------------------------------------------ */
    /* Start render loop. */

    printf( "[sb_vulkan] running -- ESC or close window to quit\n" );

    /* Track window size for viewport/scissor updates. */
    i32 win_w = 1280;
    i32 win_h = 720;

    /* Main loop. */
    while ( app()->pump_events() )
    {
        /* Drain the app event ring once: imgui consumes the input events it cares
           about (text + scroll); the host handles the rest (resize). */
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            if ( imgui()->event( &ev ) )
                continue;

            switch ( ev.type )
            {
                case APP_EV_WIN_RESIZE:
                    win_w = ev.data.win_resize.w;
                    win_h = ev.data.win_resize.h;
                    rhi()->context_resize( ctx, win_w, win_h );
                    break;

                default:
                    break;
            }
        }

        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            break;

        /* ------------------------------------------------------------------------------ */
        /* imgui testing keys */


        if ( app()->key_pressed( APP_KEY_NP_DOT ) )
        {            
            static int bmp_scale = 1;
            bmp_scale = bmp_scale == 1 ? 2 : ( bmp_scale == 2 ? 3 : 1 );
            {                
                imgui()->set_bmp_scale( bmp_scale );
            }
        }

        static u8 font_select = 1;

        if ( app()->key_pressed( APP_KEY_NP_ADD ) )
        {            
            font_select = ( font_select + 1 ) % IMGUI_FONT_BITMAP_MAX;
            imgui()->set_font( (imgui_font_t)font_select );
        }
        if ( app()->key_pressed( APP_KEY_NP_SUB ) )
        {
            font_select = ( font_select - 1 ) % IMGUI_FONT_BITMAP_MAX;
            imgui()->set_font( ( imgui_font_t )font_select );
        }
        
        /* ------------------------------------------------------------------------------ */
        /* Render frame -- skip entirely while minimized to avoid 0x0 swapchain churn. */

        if ( !app()->window_is_minimized( win ))
        {
            /* valid command recording handle on success */
            rhi_cmd_t cmd = rhi()->frame_begin( ctx );
            if ( rhi_cmd_valid( cmd ) )
            {
                if ( b_use_boot )
                {
                    sb_vk_boot_render( &boot, cmd, win_w, win_h );
                }
                else
                {
                    /* No scene this frame -- clear the swapchain so imgui's LOAD pass
                       composites over a fresh background instead of last frame's pixels
                       (otherwise dragging a window smears: hall-of-mirrors). */
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );
                }

                if ( 0 )
                {
                    /* 2D draw pass -- positions in pixel space (0,0 = top-left). */
                    const f32 bg[ 4 ] = { 0.08f, 0.08f, 0.12f, 1.0f };
                    draw()->begin_pass( cmd, win_w, win_h, bg );

                    const f32 white[ 4 ] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    draw()->rect( win_w * 0.5f, win_h * 0.5f, 200.0f, 100.0f, white );

                    const f32 red[ 4 ] = { 0.9f, 0.2f, 0.2f, 1.0f };
                    draw()->circle( 200.0f, 200.0f, 80.0f, 32, red );

                    const f32 blue[ 4 ] = { 0.2f, 0.4f, 0.9f, 1.0f };
                    draw()->rect( win_w - 160.0f, win_h - 80.0f, 120.0f, 60.0f, blue );

                    draw()->end_pass();
                }

                static bool show_ui = true;

                if ( app()->key_pressed( APP_KEY_MINUS ) ) { show_ui = false; }
                if ( app()->key_pressed( APP_KEY_EQUAL ) ) { show_ui = true; }

                /* Drag-mode test: 1 = title bar only, 2 = whole window, 3 = fixed. */
                if ( imgui() )
                {
                    if ( app()->key_pressed( APP_KEY_1 ) ) imgui()->set_window_drag( IMGUI_WIN_DRAG_TITLEBAR );
                    if ( app()->key_pressed( APP_KEY_2 ) ) imgui()->set_window_drag( IMGUI_WIN_DRAG_BODY );
                    if ( app()->key_pressed( APP_KEY_3 ) ) imgui()->set_window_drag( IMGUI_WIN_DRAG_NONE );
                }

                // imgui_win_flags_t flags = { 0 };
                // flags.no_titlebar = true;
                // UNUSED( flags );

                if ( imgui() && show_ui )
                {
                    imgui()->new_frame( win_w, win_h, 4 );

                    imgui()->begin_window( "Debug", 10, 10, 640, 640 );
                    if ( imgui()->button( "Reload$" ) )
                    {
                        // empty
                    }
                    if ( imgui()->button( "JsjperR1234q" ) )
                    {
                        // empty
                    }
                    imgui()->text( "this is some text" );
                    static float scale = 1.0f;
                    if ( imgui()->slider_float( "Scale", &scale, 1.0f, 3.0f ) )
                    {
                        // empty
                    }
                    imgui()->text( "here we go..." );
                    imgui()->textf( "formatted number: %.2f", 123.456f );

                    imgui()->text( "this is some text" );
                    imgui()->text( "THIS is more text" );
                    imgui()->text( "the last line!" );
                    imgui()->text( "abcdefghijklmnopqrstuvwxyz" );
                    imgui()->text( "ABCDEFGHIJKLMNOPQRSTUVWXYZ" );
                    imgui()->text( "`!@$%^&*&()_+~<>,./?'\\"";:[{--}]" );

                    for ( int i = 0 ; i < 32; i++ )
                    {
                        imgui()->textf( "text widdget %d", i );
                    }

                    static bool checked = false;
                    imgui()->checkbox( "Checkbox", &checked );

                    static char buffer[ 32 ] = { 0 };
                    imgui()->input_text( "Input Text", buffer, sizeof( buffer ));


                    imgui()->end_window();

                    if ( 1 ) {
                        /* Second, overlapping window -- click either to bring it to the
                           front (z-order); drag to reposition. */
                        imgui()->begin_window( "Inspector", 360, 240, 360, 280 );
                        imgui()->text( "Second window." );
                        imgui()->textf( "drag mode keys: 1 title  2 body  3 none" );
                        static bool toggle = false;
                        imgui()->checkbox( "Overlap toggle", &toggle );
                        imgui()->end_window();
                    }
                    imgui()->render( cmd, win_w, win_h );    // opens LOAD pass on swapchain, flushes, closes pass
                }

                /* Only end a frame we actually began.  frame_begin returns an invalid handle
                   without recording (minimized, swapchain out-of-date) -- calling frame_end then
                   would record into a command buffer that was never begun. */
                rhi()->frame_end( ctx );
            }
        }

        /* ------------------------------------------------------------------------------ */

        sys_sleep_milliseconds( 4 );
    }

    /* Shutdown -- drain GPU first, then free all GPU resources, then device, then window.
       context_destroy calls vk_device_wait_idle before tearing down sync/swapchain; after
       it returns the GPU is idle and pipeline/buffer destroy calls are safe. */

    rhi()->context_destroy( ctx ); // finish rendering and free swapchain + sync objects (first)

    imgui()->shutdown();

    if ( b_use_boot )
    {
        sb_vk_boot_destroy( &boot );    // destroy boot resources
    }

    draw()->shutdown();                 // destroy draw resources

    rhi()->shutdown();                  // destroy device and instance (last)
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
