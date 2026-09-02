/*==============================================================================================

    sandbox/runtime/sb_asset_image/sb_asset_image.c -- asset service image loader proof.

    Boots sys + ref + app + core + rhi + draw + asset through the module system, opens a
    window and an RHI context, then:
      - mounts a content tree (or trees) on core/fs,
      - acquire()s an image by NAME -- marked with RID() so the build resolves it against
        content/ and lists it in this executable's resource manifest -- and the asset
        service's built-in "image" type finds the file by trying its extensions in
        preference order (.tex, then .png ...) against the mounts, decodes if needed, and
        uploads a bindless RHI texture behind the id,
      - reads the resulting asset_image_t (bindless slot + dimensions) with get(),
      - draws it every frame as a single centered, aspect-fit textured quad via draw()->image.

    The whole point: pixels on screen come from an ACQUIRED ASSET named without an extension
    or a root.  Which tree served the bytes is the mounts' business; the asset log line
    "loaded '...' as image" says which file won.  ESC or the window close button quits;
    release() then unloads the texture.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/res/res.h"
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

/* The image: content/sandbox/asset/image.png, named without its extension or root.  The
   sandbox runs with the repo root as its working directory. */
#define IMAGE_NAME    "sandbox/asset/image"
#define IMAGE_SRC     "content/sandbox/asset/image.png"

/* Trees the modes mount.  build/content is the cooked mirror of content/: same names, cooked
   extensions; a mount above content/ so the image type's preferred .tex wins there. */
#define CONTENT_DIR   "content"
#define COOKED_DIR    "build/content"
#define COOKED_TEX    "build/content/sandbox/asset/image.tex"
#define COOK_TREE     "build/content_cooked"       /* asset_tool tree cook of content/, for "pack" */
#define PACK_ZIP      "build/sb_asset_pack.zip"     /* the loose png zipped in-process, for "zip"  */
#define PACK_COOK     "build/sb_asset_cooked.zip"   /* asset_tool-produced bundle, for "pack"      */

/* Make every directory along `path` (forward slashes), so a cook has somewhere to write. */
static void
dir_make_deep( const char* path )
{
    char buf[ 512 ];
    snprintf( buf, sizeof( buf ), "%s", path );
    for ( char* p = buf + 1; *p; ++p )
    {
        if ( *p == '/' )
        {
            *p = '\0';
            sys_dir_make( buf );
            *p = '/';
        }
    }
    sys_dir_make( buf );
}

/* Spawn bin/asset_tool.exe with `args`; false if it could not run or failed. */
static bool
run_asset_tool( const char* args )
{
    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    char cmd[ 1536 ];
    snprintf( cmd, sizeof( cmd ), "\"%s\\asset_tool.exe\" %s", exe_dir, args );

    sys_process_result_t res;
    if ( !sys_process_run( cmd, NULL, &res ) || res.exit_code != 0 )
    {
        fprintf( stderr, "[sb_asset_image] asset_tool failed: %s\n", cmd );
        return false;
    }
    return true;
}

/* "zip" mode: pack the loose PNG into a .zip under its content-relative name, so the asset is
   served from a bundle instead of loose files.  Reads the image through sys, deflates it into
   a heap zip, writes it out.  Returns false (and packs nothing) if the source is missing. */
static bool
build_png_zip( const char* zip_path, const char* src_png, const char* entry )
{
    sys_file_data_t fd = sys_file_read_entire( src_png );
    if ( !fd.ok )
        return false;

    pack_zip_writer_t* zw = pack_zip_writer_begin();
    bool ok = zw && pack_zip_writer_add( zw, entry, fd.data, fd.size, PACK_LEVEL_DEFAULT );

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
         "tex"  -- cook the PNG to a .tex in the cooked mirror (build/content) and mount that
                   above content/: the image type prefers .tex, so it loads with zero decode.
         "zip"  -- pack the source PNG into a bundle in-process and serve from it.
         "pack" -- asset_tool tree-cooks content/ and bundles it; serve the .tex from the pack
                   with loose content/ mounted above it (which only has the .png). */
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

    /* ---- Mount the tree(s) the image will be found in ---- */
    /* Every mode serves the same name; only the mounts differ.  The asset service never learns
       which: it asks fs for the name plus each extension the image type accepts. */

    bool mounted = true;
    if ( use_tex )
    {
        dir_make_deep( "build/content/sandbox/asset" );
        mounted = run_asset_tool( "cook " IMAGE_SRC " " COOKED_TEX );
        fs()->mount( "", COOKED_DIR, 10 );     // cooked mirror above ...
        fs()->mount( "", CONTENT_DIR, 0 );     // ... the source tree
        printf( "[sb_asset_image] cooked mirror %s over %s/\n", COOKED_DIR, CONTENT_DIR );
    }
    else if ( use_zip )
    {
        dir_make_deep( "build" );
        mounted = build_png_zip( PACK_ZIP, IMAGE_SRC, IMAGE_NAME ".png" );
        fs()->mount( "", PACK_ZIP, 0 );
        printf( "[sb_asset_image] serving from bundle %s\n", PACK_ZIP );
    }
    else if ( use_pack )
    {
        dir_make_deep( COOK_TREE );
        mounted = run_asset_tool( "-src " CONTENT_DIR " -dst " COOK_TREE )
               && run_asset_tool( "pack " COOK_TREE " " PACK_COOK );
        fs()->mount( "", PACK_COOK, 0 );       // bundle holds the .tex ...
        fs()->mount( "", CONTENT_DIR, 10 );    // ... loose content above it holds only the .png
        printf( "[sb_asset_image] serving from asset_tool pack %s (loose %s/ mounted above)\n",
                PACK_COOK, CONTENT_DIR );
    }
    else
    {
        fs()->mount( "", CONTENT_DIR, 0 );
    }

    if ( !mounted )
    {
        fprintf( stderr, "[sb_asset_image] could not prepare the backing store -- run from the repo root\n" );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }

    /* ---- Acquire the image asset by name ---- */
    /* The literal is spelled at the marker: RID() takes no macro or variable, so the build can
       harvest it (IMAGE_NAME above is only for messages and the zip entry). */
    aid_t          id  = asset()->acquire( RID( "sandbox/asset/image" ), ASSET_TYPE_IMAGE );
    asset_image_t* img = ( asset_image_t* )asset()->get( id );
    if ( !img )
    {
        fprintf( stderr, "[sb_asset_image] could not load '%s' (state=%d) -- run from the repo root\n",
                 IMAGE_NAME, asset()->state( id ) );
        asset()->release( id );
        draw()->shutdown();
        rhi()->context_destroy( ctx );
        app()->window_close( win );
        rhi()->shutdown();
        mod_system_exit();
        return 1;
    }
    printf( "[sb_asset_image] loaded '%s' -> tex_index=%u  %ux%u  (asset id {%u,%u})\n",
            asset()->name( id ), img->tex_index, img->width, img->height, id.index, id.generation );
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

        /* Hot-reload poll: if the file that loaded changed on disk, refresh() re-runs the
           loader in place behind the same id -- but that frees the old resource, so re-get the
           pointer.  (A transient bad/incomplete save can leave the asset FAILED; img goes NULL
           and we just skip drawing until the next good save.) */
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
