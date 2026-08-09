/*==============================================================================================

    sandbox/gui/sb_gui_style/sb_gui_style.c -- Style + look customization bench (root unity TU).

    The demo you open to make the engine look like your game: find a look (Style Editor), see it
    on real widgets (Look Gallery), keep it as C source (Style Export), and pick the face it is
    set in (Font Tool).  Grew out of the font bench -- a font IS half a look, so the two belong
    in one place -- and absorbed the Style Editor that used to sit inside sb_gui, which is the
    ImGui-demo replication and no home for a style tool.

    This file owns the registry, the menu bar, and main(); the window bodies live in the st_*.c
    files unity-included below, one per window, so each reads as a worked example of one part of
    the style API.  See st.h for the window contract.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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

#include "st.h"

// clang-format off

#if OS_WINDOWS
    #define PATH_SEP "\\"
#else
    #define PATH_SEP "/"
#endif

/*============================================================================================*/
/* Shared helper -- used by every window file included below.                                  */
/*============================================================================================*/

/* Open a bench window: seed its first-appearance size, then begin it CLOSEABLE (the titlebar X
   hides it; the registry re-syncs the menu check).  Position is deliberately NOT seeded -- the
   window takes the engine default spawn, which hangs below the main menu bar. */
static bool
st_begin( const char* title, f32 w, f32 h )
{
    gui()->window_set_next_size( w, h, GUI_COND_ONCE );
    return gui()->window_begin( title, GUI_WIN_CLOSEABLE );
}

/*============================================================================================*/
/* Window bodies -- one file each, unity-included so the build config stays one unit.          */
/*============================================================================================*/

#include "st_editor.c"
#include "st_gallery.c"
#include "st_export.c"
#include "st_font.c"

/*============================================================================================*/
/* Registry -- the menu bar iterates this table in order.                                      */
/*============================================================================================*/

static st_window_t s_windows[] =
{
    /* menu item       window title     description                                            fn                 open */
    { "Style Editor",  "Style Editor",  "seeds + ramp, the colour grid, every metric var",     st_editor_window,  true  },
    { "Look Gallery",  "Look Gallery",  "the widget vocabulary under the live style",          st_gallery_window, true  },
    { "Style Export",  "Style Export",  "emit the live style as a theme entry or setup code",  st_export_window,  false },
    { "Font Tool",     "Font Tool",     "find / bake / preview / export the face",             st_font_window,    false },
};

#define ST_WINDOW_COUNT ( (i32)( sizeof( s_windows ) / sizeof( s_windows[ 0 ] ) ) )

/* Show / hide one window, keeping gui's internal CLOSEABLE latch in sync (a window the user
   X-closed stays latched shut inside gui until window_set_open re-opens it). */
static void
st_set_open( st_window_t* w, bool open )
{
    w->open = open;
    if ( open )
        gui()->window_set_open( w->title, true );
}

/*============================================================================================*/
/* Frame                                                                                       */
/*============================================================================================*/

static void
st_frame( void )
{
    /* --- Menu bar: the window list, plus the theme switch that belongs at the top level ----- */
    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "Windows" ) )
        {
            for ( i32 i = 0; i < ST_WINDOW_COUNT; ++i )
            {
                st_window_t* w    = &s_windows[ i ];
                bool         open = w->open;
                if ( gui()->menu_item( w->name, NULL, &open ) )
                    st_set_open( w, open );
                gui()->set_item_tooltip( w->desc );
            }
            gui()->menu_end();
        }

        /* A theme switch is the coarsest look knob there is -- one click from anywhere, without
           opening the editor.  The editor's own combo stays the fine-grained door. */
        if ( gui()->menu_begin( "Theme" ) )
        {
            const char* active = gui()->theme_get();
            u32         count;
            const gui_theme_t* themes = gui()->theme_list( &count );

            for ( u32 i = 0; i < count; ++i )
            {
                bool on = ( active && strcmp( active, themes[ i ].name ) == 0 );
                if ( gui()->menu_item( themes[ i ].name, NULL, &on ) )
                    gui()->theme_set( themes[ i ].name );
            }
            gui()->separator();
            if ( gui()->menu_item( "Reset to theme", NULL, NULL ) )
                gui()->theme_reset();
            gui()->menu_end();
        }

        gui()->main_menu_bar_end();
    }

    /* --- Open windows.  A window the user X-closed reports closed through window_is_open, which
           is what keeps the menu check honest without the window body knowing about the menu. --- */
    for ( i32 i = 0; i < ST_WINDOW_COUNT; ++i )
    {
        st_window_t* w = &s_windows[ i ];
        if ( !w->open )
            continue;

        w->fn();

        if ( !gui()->window_is_open( w->title ) )
            w->open = false;
    }
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
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
        fprintf( stderr, "[sb_gui_style] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    /* Headless emit: `sb_gui_style -emit [theme]` writes the named built-in theme (default: the
       one gui boots with) through the exporter and exits, no window opened.  The emitter's own
       regression test -- the output is C, so the build is the assertion. */
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-emit" ) != 0 )
            continue;

        if ( i + 1 < argc )
            gui()->theme_set( argv[ i + 1 ] );

        int written = st_export_emit_files();
        mod_system_exit();
        return written ? 0 : 1;
    }

    int  ret_code    = 1;
    bool draw_inited = false;

    /* gui owns the main window + render context (boot path); see sb_gui for the full rationale. */
    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title     = "sb_gui_style",
        .w         = 1280, .h = 800,
        .os_chrome = true,
        .font      = GUI_FONT_CASCADIA_MONO_16,
        .clock     = sys_tick_seconds,
        .sleep     = sys_sleep_milliseconds,
        .wait      = sys_wait_for_os_events_ms,
        .clear     = { 0.15f, 0.15f, 0.20f, 1.00f },
        .debug     = true,
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_style] gui->boot failed\n" );
        goto shutdown;
    }

    if ( !draw()->init() )
    {
        fprintf( stderr, "[sb_gui_style] draw->init failed\n" );
        goto shutdown;
    }
    draw_inited = true;

    /* dev_font drives the Font Tool's local scan + quick stb bake; font_tool.exe (spawned) drives
       the final one. */
    dev_font_init( NULL );

    gui()->set_retained_skip( true );

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            st_frame();
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        gui()->boot_pace ( 4, 16 );
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
