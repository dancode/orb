/*==============================================================================================

    sandbox/asset/sb_asset_image.c -- asset service image loader proof (Phase 3).

    Boots sys + ref + app + core + rhi + draw + asset through the module system, opens a window
    and an RHI context, then:
      - mounts the current directory on core/fs,
      - acquire()s a PNG by virtual path -- the asset service's built-in "image" type decodes it
        (stb_image) and uploads a bindless RHI texture behind the id,
      - reads the resulting asset_image_t (bindless slot + dimensions) with get(),
      - draws it every frame as a single centered, aspect-fit textured quad via draw()->image.

    The whole point: pixels on screen come from an ACQUIRED ASSET ID, not a hand-built texture.
    ESC or the window close button quits; release() then unloads the texture.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/asset/asset_host.h"
#include "runtime_service/asset/loaders/asset_image.h"

// clang-format off

/* Virtual path of the image to display.  gui_issue.png sits at the repo root; the sandbox runs
   with the repo root as its working directory, and we mount "" -> "" (CWD) below. */
#define IMAGE_VPATH   "gui_issue.png"

int
main( int argc, char** argv )
{
    /* Optional first arg = auto-quit after N rendered frames (headless smoke test); 0/absent
       runs interactively until ESC or the close button. */
    int max_frames = ( argc > 1 ) ? atoi( argv[ 1 ] ) : 0;

    /* ---- Modules ---- */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( draw );
    mod_static( asset );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_asset_image] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    /* ---- RHI + window ---- */
    if ( !rhi()->init() )
    {
        mod_system_exit();
        return 1;
    }

    win_id_t win = app()->window_open( "sb_asset_image", 0, 0, 1280, 720, APP_WIN_DEFAULT );
    if ( win == APP_WIN_INVALID )
    {
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    i32 ctx = rhi()->context_open( win );
    if ( ctx == RHI_CTX_INVALID )
    {
        app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_asset_image] draw->init failed\n" );
        rhi()->context_destroy( ctx );
        app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    /* ---- Acquire the image asset ---- */
    /* Serve the working directory verbatim (vpath == real path under CWD). */
    core()->fs_mount( "", "", 0 );

    asset_id_t     id  = asset()->acquire( IMAGE_VPATH );
    asset_image_t* img = ( asset_image_t* )asset()->get( id );
    if ( !img )
    {
        fprintf( stderr, "[sb_asset_image] could not load '%s' (state=%d) -- run from the repo root\n",
                 IMAGE_VPATH, asset()->state( id ) );
        asset()->release( id );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }
    printf( "[sb_asset_image] loaded '%s' -> tex_index=%u  %ux%u  (asset id {%u,%u})\n",
            IMAGE_VPATH, img->tex_index, img->width, img->height, id.index, id.generation );
    printf( "[sb_asset_image] running -- edit/re-save the image to hot-reload; ESC to quit\n" );
    fflush( stdout );

    const u32 samp   = draw()->sampler_linear();
    int       frames = 0;

    /* ---- Render loop ---- */
    while ( app()->pump_events() )
    {
        app_event_t ev;
        while ( app()->next_event( &ev ) )
        {
            rhi()->event( &ev );
            if ( ev.type == APP_EV_WIN_CLOSE )
                goto shutdown;
        }
        if ( app()->key_pressed( APP_KEY_ESCAPE ) )
            goto shutdown;

        if ( app()->window_is_minimized( win ) )
            continue;

        /* Hot-reload poll: if the source file changed on disk, refresh() re-runs the loader in
           place behind the same id -- but that frees the old resource, so re-get the pointer.
           (A transient bad/incomplete save can leave the asset FAILED; img goes NULL and we
           just skip drawing until the next good save.) */
        if ( asset()->refresh() > 0 )
        {
            img = ( asset_image_t* )asset()->get( id );
            if ( img )
                printf( "[sb_asset_image] hot-reloaded -> tex_index=%u  %ux%u\n",
                        img->tex_index, img->width, img->height );
            else
                printf( "[sb_asset_image] hot-reload left asset FAILED (state=%d) -- skipping draw\n",
                        asset()->state( id ) );
            fflush( stdout );
        }

        i32 w = 0, h = 0;
        app()->window_get_size( win, &w, &h );

        rhi_cmd_t cmd = rhi()->frame_begin( ctx );
        if ( rhi_cmd_valid( cmd ) )
        {
            /* begin_pass clears, binds the bindless set, sets viewport/scissor, and installs a
               pixel-space ortho matrix -- so image() coordinates below read as pixels. */
            draw()->begin_pass( cmd, w, h, ( const f32[ 4 ] ){ 0.06f, 0.06f, 0.09f, 1.0f } );

            if ( img )
            {
                /* Centered, aspect-fit into 90% of the window. */
                f32 avail_w = (f32)w * 0.9f;
                f32 avail_h = (f32)h * 0.9f;
                f32 scale   = avail_w / (f32)img->width;
                if ( (f32)img->height * scale > avail_h )
                    scale = avail_h / (f32)img->height;

                f32 qw = (f32)img->width  * scale;
                f32 qh = (f32)img->height * scale;

                draw()->image( (f32)w * 0.5f, (f32)h * 0.5f, qw, qh,
                               img->tex_index, samp, ( const f32[ 4 ] ){ 1, 1, 1, 1 } );
            }

            draw()->end_pass();
            rhi()->frame_end( ctx );

            if ( max_frames && ++frames >= max_frames )
            {
                printf( "[sb_asset_image] rendered %d frames -- exiting (headless smoke)\n", frames );
                goto shutdown;
            }
        }
    }

shutdown:
    asset()->release( id );          // unloads the texture (unregister + destroy)
    rhi()->context_destroy( ctx );
    draw()->shutdown();
    rhi()->shutdown();
    app()->window_close( win );
    mod_system_exit();
    return 0;
}

/*============================================================================================*/
// clang-format on
