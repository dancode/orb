/*==============================================================================================

    sandbox/gui/gui_font/sb_gui_font.c -- Font testing sandbox.

    A dedicated bench for finding, baking, previewing, and exporting fonts -- split out of
    sb_gui so font work has a home of its own.

    Two bakers are exercised side by side (the engine ships both):
      * quick stb bake  -- dev_font_get(): rasterized at runtime with stb_truetype into
                           assets/font_cache/.  Instant, disposable, "I want an stb font".
      * final orb bake  -- font_tool.exe: FreeType quality, written to assets/font/.  This is
                           the shippable asset, invoked here as a child process ("I want an orb font").

    Search scope is a checkbox: local only (assets/font_source/) or Windows too (the OS font
    registry, resolving "Cascadia Mono" -> C:\Windows\Fonts\CascadiaMono.ttf without copying it in).

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/draw/draw_host.h"
#include "runtime_service/gui/gui_host.h"
#include "developer/dev_font/dev_font.h"

// clang-format off

#if OS_WINDOWS
    #define PATH_SEP "\\"
#else
    #define PATH_SEP "/"
#endif

/*============================================================================================*/
/* Font tool state                                                                             */
/*============================================================================================*/

#define FT_LIST_MAX 512
#define FT_NAME_MAX 128

typedef struct
{
    /* Picker listing: font_source/ files first (indices 0..local_count), then installed faces
       (indices local_count..count, sorted) when allow_windows is on. */
    char names[ FT_LIST_MAX ][ FT_NAME_MAX ];
    int  count;
    int  local_count;        /* number of leading entries that are project (font_source) fonts */
    int  sel;
    bool scanned;
    bool list_has_windows;   /* scope the current listing was built with (detects toggle) */
    bool list_has_local;     /* ditto, for the font_source/ toggle */

    /* Request the user is working on. */
    char request[ FT_NAME_MAX ];   /* font name or filename to resolve */
    bool allow_windows;            /* false = local only; true = also the OS font registry */
    bool allow_local;              /* false = skip assets/font_source/ in the picker listing */
    i32  size_px;

    /* Live stb preview. */
    u32  preview_id;
    bool preview_ready;
    char preview_name[ FT_NAME_MAX ];
    i32  preview_size;
    char sample_text[ 256 ];

    /* Status lines for the two actions. */
    char bake_status  [ 256 ];
    bool bake_ok;
    char export_status[ 512 ];
    bool export_ok;
    char export_path  [ 512 ];   /* just the .orb_font path parsed out of export_status, for copy-to-clipboard */

    /* Atlas preview toggle (see sb_gui.c's Font Browser -- same pattern). */
    bool show_atlas;
    bool atlas_2x;

} font_tool_t;

static font_tool_t s_ft;

/*============================================================================================*/
/* Resolution honoring the local/Windows scope checkbox.                                       */
/*                                                                                             */
/* dev_font_resolve() always walks font_source -> system dir -> OS registry.  When the user    */
/* asks for local only we must NOT fall through to Windows, so we probe assets/font_source/     */
/* ourselves and stop there.  With the box ticked we defer to the full resolver.               */
/*============================================================================================*/

static bool
ft_resolve( const char* request, bool allow_windows, char* out, int out_size )
{
    if ( allow_windows )
        return dev_font_resolve( request, out, out_size );

    /* Local only: exact name in font_source/, then the common face extensions. */
    char src[ 512 ];
    if ( !dev_font_source_dir( src, sizeof( src ) ) )
        return false;

    static const char* ext[] = { "", ".ttf", ".otf", ".ttc" };
    for ( int i = 0; i < 4; ++i )
    {
        snprintf( out, (size_t)out_size, "%s" PATH_SEP "%s%s", src, request, ext[ i ] );
        if ( sys_file_time( out ) > 0 )
            return true;
    }
    return false;
}

/*============================================================================================*/
/* Scan assets/font_source/ for the local picker.                                              */
/*============================================================================================*/

/* Append one name to the list (both scans share this).  Returns false once the list is full so
   the enumerations stop early. */
static bool
ft_add( const char* name )
{
    if ( s_ft.count >= FT_LIST_MAX )
        return false;
    snprintf( s_ft.names[ s_ft.count++ ], FT_NAME_MAX, "%s", name );
    return s_ft.count < FT_LIST_MAX;
}

static bool
ft_local_cb( const char* filename, const char* full_path, void* ud )
{
    UNUSED( full_path );
    UNUSED( ud );
    return ft_add( filename );
}

static bool
ft_windows_cb( const char* name, const char* full_path, void* ud )
{
    UNUSED( full_path );
    UNUSED( ud );
    return ft_add( name );   /* friendly name, e.g. "Cascadia Mono Regular" */
}

/* Case-insensitive name compare for sorting the Windows portion of the list. */
static int
ft_name_cmp( const void* a, const void* b )
{
    const char* x = (const char*)a;
    const char* y = (const char*)b;
    for ( ; *x && *y; ++x, ++y )
    {
        char cx = ( *x >= 'A' && *x <= 'Z' ) ? (char)( *x - 'A' + 'a' ) : *x;
        char cy = ( *y >= 'A' && *y <= 'Z' ) ? (char)( *y - 'A' + 'a' ) : *y;
        if ( cx != cy ) return (unsigned char)cx - (unsigned char)cy;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

static void
ft_scan( void )
{
    s_ft.count = 0;

    /* Local .ttf/.otf/.ttc under assets/font_source/ (listed first, by filename), when the scope
       box is ticked. */
    if ( s_ft.allow_local )
    {
        char src[ 512 ];
        if ( dev_font_source_dir( src, sizeof( src ) ) )
        {
            sys_file_glob( src, "*.ttf", ft_local_cb, NULL );
            sys_file_glob( src, "*.otf", ft_local_cb, NULL );
            sys_file_glob( src, "*.ttc", ft_local_cb, NULL );
        }
    }
    s_ft.local_count = s_ft.count;   /* everything before here is a project font */

    /* Installed fonts by friendly name, only when the scope box is ticked.  These arrive as two
       per-hive alphabetical runs; sort the whole appended block into one A-Z list so a face is
       easy to find (and does not appear to be missing just because it sits in another run). */
    if ( s_ft.allow_windows )
    {
        sys_font_enumerate( ft_windows_cb, NULL );
        if ( s_ft.count > s_ft.local_count )
            qsort( s_ft.names[ s_ft.local_count ], (size_t)( s_ft.count - s_ft.local_count ),
                   FT_NAME_MAX, ft_name_cmp );
    }

    s_ft.list_has_windows = s_ft.allow_windows;
    s_ft.list_has_local   = s_ft.allow_local;
    s_ft.scanned          = true;
    if ( s_ft.sel >= s_ft.count )
        s_ft.sel = 0;
}

/*============================================================================================*/
/* Action: quick stb bake + live preview (dev_font_get).                                       */
/*============================================================================================*/

static void
ft_bake_preview( void )
{
    if ( !s_ft.request[ 0 ] )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "Enter a font name or file." );
        s_ft.bake_ok = false;
        return;
    }

    /* Honor the scope checkbox before handing off to the baker: resolve to an absolute path so
       dev_font_get uses it verbatim (a path with a separator bypasses its internal search). */
    char abs[ 512 ];
    if ( !ft_resolve( s_ft.request, s_ft.allow_windows, abs, sizeof( abs ) ) )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ),
                  s_ft.allow_windows ? "Not found: %s"
                                     : "Not in font_source/: %s  (tick Windows to search installed fonts)",
                  s_ft.request );
        s_ft.bake_ok = false;
        return;
    }

    char path[ 512 ];
    if ( !dev_font_get( abs, s_ft.size_px, path, sizeof( path ) ) )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "Bake error: %s", dev_font_last_error() );
        s_ft.bake_ok = false;
        return;
    }

    if ( !s_ft.preview_ready )
    {
        u32 id = gui()->font_load( path );
        if ( !id )
        {
            snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "font_load failed for %s", path );
            s_ft.bake_ok = false;
            return;
        }
        s_ft.preview_id    = id;
        s_ft.preview_ready = true;
    }
    else
    {
        gui()->font_load_into( s_ft.preview_id, path );
    }

    snprintf( s_ft.preview_name, sizeof( s_ft.preview_name ), "%s", s_ft.request );
    s_ft.preview_size = s_ft.size_px;
    snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ),
              "stb baked: %s at %d px -> %s", s_ft.request, s_ft.size_px, path );
    s_ft.bake_ok = true;
}

/*============================================================================================*/
/* Action: final FreeType bake via font_tool.exe (writes assets/font/).                        */
/*                                                                                             */
/* font_tool lives next to us in bin/; it resolves the same inputs we do (shared dev_font) and  */
/* defaults its output to assets/font/<name>_<size>px.orb_font, creating the dir if needed.     */
/*============================================================================================*/

static void
ft_export_final( void )
{
    if ( !s_ft.request[ 0 ] )
    {
        snprintf( s_ft.export_status, sizeof( s_ft.export_status ), "Enter a font name or file." );
        s_ft.export_ok = false;
        return;
    }

    char exe_dir[ 512 ];
    sys_exe_dir( exe_dir, sizeof( exe_dir ) );

    char cmd[ 1024 ];
    snprintf( cmd, sizeof( cmd ), "\"%s" PATH_SEP "font_tool.exe\" \"%s\" %d",
              exe_dir, s_ft.request, s_ft.size_px );

    char                 out[ 1024 ] = { 0 };
    sys_process_result_t res         = { 0 };
    bool launched = sys_process_run_capture( cmd, NULL, out, sizeof( out ), NULL, &res );

    if ( !launched || !res.started )
    {
        snprintf( s_ft.export_status, sizeof( s_ft.export_status ),
                  "Could not launch font_tool.exe (expected in %s)", exe_dir );
        s_ft.export_ok = false;
        return;
    }

    /* font_tool prints its output path on success and an "error:" line on failure; surface its
       last line of output alongside the exit code so the bench shows what the tool reported. */
    const char* tail = out;
    for ( const char* p = out; *p; ++p )
        if ( ( *p == '\n' || *p == '\r' ) && p[ 1 ] && p[ 1 ] != '\n' && p[ 1 ] != '\r' )
            tail = p + 1;

    s_ft.export_path[ 0 ] = '\0';

    if ( res.exit_code == 0 )
    {
        snprintf( s_ft.export_status, sizeof( s_ft.export_status ), "font_tool ok: %s", tail );

        /* Pull the path back out of font_tool's own "... -> 'path'" trailer (see font_tool.c's
           final printf) so the copy button doesn't need to re-derive the output path itself. */
        const char* arrow = strstr( tail, " -> '" );
        if ( arrow )
        {
            const char* path_start = arrow + 5;
            const char* quote_end  = strrchr( path_start, '\'' );
            if ( quote_end && quote_end > path_start )
                snprintf( s_ft.export_path, sizeof( s_ft.export_path ), "%.*s",
                          (int)( quote_end - path_start ), path_start );
        }
    }
    else
    {
        snprintf( s_ft.export_status, sizeof( s_ft.export_status ),
                  "font_tool failed (exit %d): %s", res.exit_code, tail );
    }
    s_ft.export_ok = ( res.exit_code == 0 );
}

/*============================================================================================*/
/* Window                                                                                      */
/*============================================================================================*/

static void
show_font_tool( void )
{
    /* Lazy init on first show. */
    if ( !s_ft.scanned )
    {
        s_ft.size_px       = 16;
        s_ft.allow_windows = true;
        s_ft.allow_local   = true;
        snprintf( s_ft.sample_text, sizeof( s_ft.sample_text ),
                  "The quick brown fox jumps over the lazy dog." );
        ft_scan();
        if ( s_ft.count > 0 )
            snprintf( s_ft.request, sizeof( s_ft.request ), "%s", s_ft.names[ 0 ] );
    }

    gui()->window_set_next_size( 560.0f, 560.0f, GUI_COND_ONCE );
    if ( !gui()->window_begin( "Font Tool", GUI_WIN_NONE ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* --- Source --------------------------------------------------------------- */
    gui()->separator_text( "Source" );

    gui()->text( "Font name or file" );
    gui()->input_text( "##request", s_ft.request, sizeof( s_ft.request ) );

    gui()->checkbox( "Include internal fonts", &s_ft.allow_local );
    gui()->same_line( -1 );
    gui()->help_marker( "On:  list assets/font_source/ (the project's shipped fonts) in the picker.\n"
                        "Off: skip them -- handy for browsing/testing the Windows registry alone." );

    gui()->checkbox( "Include Windows fonts", &s_ft.allow_windows );
    gui()->same_line( -1 );
    gui()->help_marker( "Off: list/search assets/font_source/ only.\n"
                        "On:  also list installed fonts by friendly name from the OS font registry\n"
                        "     (e.g. \"Cascadia Mono\" -> C:\\Windows\\Fonts\\CascadiaMono.ttf)." );

    /* Either checkbox governs the picker's scope -- rebuild the list when either flips. */
    if ( s_ft.list_has_windows != s_ft.allow_windows || s_ft.list_has_local != s_ft.allow_local )
        ft_scan();

    /* Local picker: choosing a file copies its name into the request field above. */
    const char* combo_label = ( s_ft.count > 0 ) ? s_ft.names[ s_ft.sel ] : "(no local fonts)";
    
    if ( gui()->combo_begin( "##local", combo_label, GUI_COMBO_NONE ) )
    {
        gui()->scale_push( GUI_SCALE_DENSE );
        for ( int i = 0; i < s_ft.count; i++ )
        {
            /* Project (font_source) fonts lead the list in a distinct tint; installed Windows
               faces follow in default color. */
            bool project = ( i < s_ft.local_count );
            if ( project )
                gui()->push_style_color( GUI_COL_TEXT, GUI_COLOR( 0x7C, 0xD9, 0x92, 0xFF ) );

            bool sel = ( i == s_ft.sel );
            if ( gui()->selectable( s_ft.names[ i ], &sel ) )
            {
                s_ft.sel = i;
                snprintf( s_ft.request, sizeof( s_ft.request ), "%s", s_ft.names[ i ] );
            }

            if ( project )
                gui()->pop_style_color( 1 );
        }
        gui()->scale_pop();
        gui()->combo_end();
    }
    

    // gui()->same_line( -1 );
    if ( gui()->small_button( "Refresh" ) )
        ft_scan();

    gui()->slider_int( "Size (px)", &s_ft.size_px, 6, 72 );

    /* --- Actions -------------------------------------------------------------- */
    gui()->separator_text( "Bake" );

    /* Two half-width buttons at normal height.  next_item_fit(1.0) makes each button fill its
       0.5 cell (a plain button would shrink to its label); button_fill is NOT used here -- it
       sizes its HEIGHT to the view bottom, which feeds back into the scroll extent when content
       follows it (status + preview below), growing the region without bound every scrolled frame. */
    gui()->row2( 0.5f, 0.5f );
    // if ( gui()->button_fill( "Bake & Preview (stb)" ) )
    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "Bake & Preview (stb)" ) )
        ft_bake_preview();

    // if ( gui()->button_fill( "Export final (font_tool)" ) )
    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "Export final (font_tool)" ) )
        ft_export_final();

    gui()->stack();
    if ( s_ft.bake_status[ 0 ] )
    {
        if ( s_ft.bake_ok ) gui()->text_disabled( s_ft.bake_status );
        else                gui()->text_colored( GUI_COLOR( 0xFF, 0x60, 0x60, 0xFF ), s_ft.bake_status );
    }
    if ( s_ft.export_status[ 0 ] )
    {
        if ( s_ft.export_ok ) gui()->text_wrapped( s_ft.export_status );
        else                  gui()->text_colored( GUI_COLOR( 0xFF, 0x60, 0x60, 0xFF ), s_ft.export_status );
    }
    if ( s_ft.export_path[ 0 ] && gui()->button( "Copy Path" ) )
        app()->clipboard_set( s_ft.export_path );

    /* --- Preview -------------------------------------------------------------- */
    if ( s_ft.preview_ready )
    {
        gui()->separator_text( "Preview" );
        gui()->input_text_with_hint( "##sample", "Custom preview text...",
                                     s_ft.sample_text, sizeof( s_ft.sample_text ) );
        gui()->new_line( -1.0f );

        /* NOTE -- the renderer resolves glyphs from ONE global active font at tessellation time,
           so push_font here recolors the WHOLE frame's text, not just these lines.  For a font
           bench that is the point (you see the face applied everywhere); a truly isolated preview
           would need a separate texture path the single-global-font model does not offer. */
        gui()->push_font( s_ft.preview_id );
        gui()->stack();
        if ( s_ft.sample_text[ 0 ] )
            gui()->text( s_ft.sample_text );
        gui()->text( "ABCDEFGHIJKLMNOPQRSTUVWXYZ" );
        gui()->text( "abcdefghijklmnopqrstuvwxyz" );
        gui()->text( "0123456789  !@#$%^&*()-+=[]{};:" );
        gui()->pop_font();

        gui()->separator_text( "Apply" );
        gui()->textf( "Live: %s  %d px", s_ft.preview_name, s_ft.preview_size );
        if ( gui()->button( "Use as UI font" ) )
            gui()->font_use( s_ft.preview_id );
    }

    /* --- Atlas ------------------------------------------------------------------ */
    /* Independent of the bake/preview flow above -- shows the CURRENTLY ACTIVE font's atlas
       (whatever the app booted with, or last font_use'd via "Use as UI font"), so the controls are
       there from the first frame instead of only appearing after a bake.  NOTE -- the atlas is an
       R8_UNORM coverage texture, but image_texture samples it via the RGBA path (texel.rgb as
       color): only the red channel carries data, so glyph ink renders red-on-black rather than
       white-on-black.  Fine for judging packing (ink vs gap is still obvious); a true grayscale
       view would need a dedicated coverage-sampling draw path. */
    gui()->separator_text( "Atlas" );
    if ( gui()->button( "Show Atlas" ) )
        s_ft.show_atlas = !s_ft.show_atlas;
    gui()->same_line( -1 );
    gui()->checkbox( "2x", &s_ft.atlas_2x );

    if ( s_ft.show_atlas )
    {
        u32 active_id = gui()->font_active_id();
        u32 atlas_idx = gui()->font_atlas_idx( active_id );
        if ( atlas_idx )
        {
            gui_vec2_t asz = gui()->font_atlas_size( active_id );

            gui()->textf( "Active font #%u -- %.0f x %.0f px  (bindless #%u)",
                          active_id, asz.x, asz.y, atlas_idx );

            /* Native resolution (or 2x via the checkbox) -- no fit-to-window scaling, so packing/
               coverage reads exactly as baked. */
            f32 scale = s_ft.atlas_2x ? 2.0f : 1.0f;
            gui()->image_texture( atlas_idx, asz.x * scale, asz.y * scale, 0 );
        }
        else
        {
            gui()->text_disabled( "No active font atlas." );
        }
    }

    gui()->window_end();
}

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
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_font] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int  ret_code    = 1;
    bool draw_inited = false;

    /* gui owns the main window + render context (boot path); see sb_gui for the full rationale. */
    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "sb_gui_font",
        .w         = 1100, .h = 720,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO_16, // GUI_FONT_CASCADIA_MONO_16
        .caps      = &( gui_forward_caps_t ){ .keyboard_nav = true, .tables = false, .docking = false },
        .clock     = sys_tick_seconds,
        .sleep     = sys_sleep_milliseconds,
        .wait      = sys_wait_for_os_events_ms,
        .clear     = { 0.15f, 0.15f, 0.20f, 1.00f },
        .debug     = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_font] gui->boot failed\n" );
        goto shutdown;
    }

    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_gui_font] draw->init failed\n" );
        goto shutdown;
    }
    draw_inited = true;

    /* dev_font drives the local scan + quick stb bake; font_tool.exe (spawned) drives the final one. */
    dev_font_init( NULL );

    gui()->set_retained_skip( true );

    f32 dt = 0.0f;
    while ( gui()->frame_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            show_font_tool();
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->present_begin( NULL );
        gui()->present_end();

        gui()->frame_pace( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();
    if ( draw_inited ) draw()->shutdown();
    rhi()->shutdown();
    dev_font_shutdown();
    mod_system_exit();
    return ret_code;
}

// clang-format on
