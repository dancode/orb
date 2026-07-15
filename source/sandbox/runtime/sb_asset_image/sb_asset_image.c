/*==============================================================================================

    sandbox/runtime/sb_asset_image/sb_asset_image.c -- asset service image loader proof (Phase 3).

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
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "engine/fs/fs_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/asset/asset_host.h"
#include "runtime_service/asset/loaders/asset_image.h"
#include "engine/pack/pack_host.h"   /* build a pack.zip for the "served from a .zip" mode */

// clang-format off

/* Virtual path of the image to display.  gui_issue.png sits at the repo root; the sandbox runs
   with the repo root as its working directory, and we mount "" -> "" (CWD) below. */
#define IMAGE_VPATH   "gui_issue.png"
#define IMAGE_TEX     "gui_issue.tex"   /* cooked twin: asset_tool cook gui_issue.png gui_issue.tex */
#define PACK_ZIP      "sb_asset_pack.zip"
#define PACK_COOK     "sb_asset_cooked.zip"   /* asset_tool-produced bundle for "pack" mode */

/* Phase 5 mode: pack the loose PNG into a .zip so the asset is served from a bundle instead of
   loose files.  Reads the loose image through sys, deflates it into a heap zip, writes it out.
   Returns false (and packs nothing) if the source image is not next to the executable's CWD. */
static bool
build_png_zip( const char* zip_path, const char* src_png )
{
    sys_file_data_t fd = sys_file_read_entire( src_png );
    if ( !fd.ok )
        return false;

    pack_zip_writer_t* zw = pack_zip_writer_begin();
    bool ok = zw && pack_zip_writer_add( zw, src_png, fd.data, fd.size, PACK_LEVEL_DEFAULT );

    void* buf = NULL;
    u32   sz  = 0;
    if ( zw )
        ok = pack_zip_writer_end( zw, &buf, &sz ) && ok;
    if ( ok )
        ok = sys_file_write_entire( zip_path, buf, sz );

    free( buf );                 // writer_end handed us ownership of the heap block
    sys_file_free( &fd );
    return ok;
}

int
main( int argc, char** argv )
{
    /* Optional numeric arg = auto-quit after N rendered frames (headless smoke test); 0/absent
       runs interactively until ESC or the close button.  Backing-store arg (pick one):
         "zip"  -- pack the source PNG into a bundle in-process and serve from it (Phase 5).
         "tex"  -- load the cooked .tex twin from loose files, zero decode (Cook-C).
         "pack" -- load the cooked .tex from an asset_tool-produced .zip pack (Cook-D). */
    int         max_frames = 0;
    bool        use_zip    = false;
    bool        use_tex    = false;
    bool        use_pack   = false;
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "zip" ) == 0 )
            use_zip = true;
        else if ( strcmp( argv[ i ], "tex" ) == 0 )
            use_tex = true;
        else if ( strcmp( argv[ i ], "pack" ) == 0 )
            use_pack = true;
        else
            max_frames = atoi( argv[ i ] );
    }

    /* "tex" mode acquires the cooked .tex twin instead of the source PNG: the loader uploads its
       pre-decoded RGBA8 payload with zero decode (Cook-C).  Cook it first with
       `asset_tool cook gui_issue.png gui_issue.tex`.
       "pack" mode (Cook-D) also acquires the .tex, but from an asset_tool-produced .zip bundle
       (see the mount block below). */
    const char* image_vpath = ( use_tex || use_pack ) ? IMAGE_TEX : IMAGE_VPATH;

    /* ---- Modules ---- */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( pack );
    mod_static( fs );
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
    /* Loose mode: serve the working directory verbatim (vpath == real path under CWD).
       Zip mode (Phase 5): pack the PNG into a bundle and mount that -- the asset service reads
       through core/fs, so acquire() is identical; only the backing store changes. */

    if ( use_zip )
    {
        if ( !build_png_zip( PACK_ZIP, IMAGE_VPATH ) )
        {
            fprintf( stderr, "[sb_asset_image] could not build %s from '%s' -- run from the repo root\n",
                     PACK_ZIP, IMAGE_VPATH );
            draw()->shutdown();
            rhi()->context_destroy( ctx );
            app()->window_close( win );
            rhi()->shutdown();
            mod_system_exit();
            return 1;
        }
        fs()->mount( "", PACK_ZIP, 0 );
        printf( "[sb_asset_image] serving from bundle %s\n", PACK_ZIP );
    }
    else if ( use_pack )
    {
        /* Cook-D: serve the cooked .tex out of an asset_tool-produced pack.  The bundle mounts at
           priority 0; CWD mounts loose at priority 10, so a loose file of the same vpath would
           shadow the bundled one (loose-over-bundle, proven generically in fs_zip_test).  Build
           the pack first:  asset_tool -src <srctree> -dst cooked  &&  asset_tool pack cooked <zip>
           where <srctree> holds gui_issue.png, so the pack carries gui_issue.tex. */
        fs()->mount( "", PACK_COOK, 0 );    // bundle (low priority)
        fs()->mount( "", "", 10 );          // loose CWD (high priority) -- would override
        printf( "[sb_asset_image] serving from asset_tool pack %s (loose CWD overrides)\n", PACK_COOK );
    }
    else
    {
        fs()->mount( "", "", 0 );
    }

    asset_id_t     id  = asset()->acquire( image_vpath );
    asset_image_t* img = ( asset_image_t* )asset()->get( id );
    if ( !img )
    {
        fprintf( stderr, "[sb_asset_image] could not load '%s' (state=%d) -- run from the repo root\n",
                 image_vpath, asset()->state( id ) );
        asset()->release( id );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }
    printf( "[sb_asset_image] loaded '%s' (%s) -> tex_index=%u  %ux%u  (asset id {%u,%u})\n",
            image_vpath, ( use_tex || use_pack ) ? "cooked .tex, zero decode" : "source decode",
            img->tex_index, img->width, img->height, id.index, id.generation );
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
    if ( use_zip )
        sys_file_delete( PACK_ZIP );  // scratch bundle built at startup
    return 0;
}

/*============================================================================================*/
// clang-format on
