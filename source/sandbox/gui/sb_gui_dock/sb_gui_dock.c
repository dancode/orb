/*==============================================================================================

    sandbox/gui/sb_gui_dock/sb_gui_dock.c -- docking test bed.

    The dock system demo, isolated in its own host: a dockspace fills the main viewport for the
    program's whole life.  It used to live in sb_gui_example's demo rotation, but the dock tree
    and its docked windows persist in gui state across demo switches -- stepping from the dock
    demo to a plain-window demo and back left stale layout behind.  Here nothing ever switches:
    the dockspace is the application.

    What it exercises (see gui_dock*.c):
      - dockspace_over_viewport() at the top of every build; splitter drag + tab interaction.
      - Programmatic layout: dock_split / dock_window build the tree once (DockBuilder idiom
        of splitting the shrinking remainder).
      - Tabs: Console / Assets share a node; drag a tab out to pop it into a floater.
      - Drag-to-dock: the free Palette window drops onto any pane (center = tab, side = split).
      - Layout persistence: dock_save / dock_load round-trip the tree in RAM.

    Host shape is the boot-tier easy-mode loop (see sb_gui_example.c for the annotated version).

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

/*==============================================================================================
    Dock scene -- one dockspace, five docked panels, one free floater
==============================================================================================*/

static void
show_dock_scene( gui_vp_t vp )
{
    /* Layout persistence: the Viewport panel's buttons only set these flags; the actual
       save/restore runs HERE, at the top of the build before the dockspace and any docked window --
       a safe point to free + rebuild the tree (dock_load) without touching a node mid-render.  A real
       app would write s_layout to a file on save and load it at startup; here it round-trips in RAM. */
    static char s_layout[ 2048 ];
    static bool s_have_layout    = false;
    static bool s_save_layout    = false;
    static bool s_restore_layout = false;
    if ( s_save_layout )
    {
        gui()->dock_save( vp, s_layout, sizeof( s_layout ) );
        s_have_layout = true;
        s_save_layout = false;
    }
    if ( s_restore_layout )
    {
        if ( s_have_layout ) gui()->dock_load( vp, s_layout );
        s_restore_layout = false;
    }

    /* Lay out + interact the dockspace every frame (must precede the docked windows' window_begin). */
    gui_dock_id_t root = gui()->dockspace_over_viewport( vp, GUI_DOCKSPACE_NONE );

    /* Build the tree once: left rail, right inspector, a bottom strip, central viewport. */
    static bool built = false;
    if ( !built && root != GUI_DOCK_NONE )
    {
        gui_dock_id_t left   = gui()->dock_split( root, GUI_DIR_LEFT,  0.22f, &root );
        gui_dock_id_t right  = gui()->dock_split( root, GUI_DIR_RIGHT, 0.28f, &root );
        gui_dock_id_t bottom = gui()->dock_split( root, GUI_DIR_DOWN,  0.30f, &root );
        gui()->dock_window( "Scene Tree", left   );
        gui()->dock_window( "Inspector",  right  );
        gui()->dock_window( "Console",    bottom );
        gui()->dock_window( "Assets",     bottom );   /* tab alongside Console */
        gui()->dock_window( "Viewport",   root   );   /* central remainder */
        built = true;
    }

    if ( gui()->window_begin( "Scene Tree", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( gui()->tree_node( "World" ) )
        {
            gui()->text( "Camera" );
            gui()->text( "Sun Light" );
            if ( gui()->tree_node( "Props" ) )
            {
                gui()->text( "Crate" );
                gui()->text( "Barrel" );
                gui()->tree_pop();
            }
            gui()->tree_pop();
        }
    }
    gui()->window_end();

    if ( gui()->window_begin( "Inspector", GUI_WIN_NONE ) )
    {
        static char name[ 32 ] = "Crate";
        static f32  pos[ 3 ]   = { 0.0f, 1.0f, 0.0f };
        static bool visible    = true;
        gui()->stack();                     /* declare the layout mode before any widget */
        gui()->field_label_left( 80.0f );
        gui()->input_text  ( "Name",     name, sizeof( name ) );
        gui()->input_float3( "Position", pos, NULL );
        gui()->checkbox    ( "Visible",  &visible );
    }
    gui()->window_end();

    if ( gui()->window_begin( "Console", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "[info] engine started" );
        gui()->text( "[info] dock layout built" );
        gui()->text( "> _" );
    }
    gui()->window_end();

    if ( gui()->window_begin( "Assets", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->bullet_text( "models/crate.obj" );
        gui()->bullet_text( "textures/wood.png" );
        gui()->bullet_text( "shaders/lit.glsl" );
    }
    gui()->window_end();

    if ( gui()->window_begin( "Viewport", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Central viewport panel." );
        gui()->separator();
        gui()->text( "Drag the gutters between regions to resize." );
        gui()->text( "Click the Console / Assets tabs to switch." );
        gui()->text( "Drag a tab OUT to pop it into a floater." );
        gui()->text( "Drag the Palette window onto a pane to dock." );
        gui()->separator();
        gui()->text( "Rearrange, Save, rearrange more, then Restore:" );
        /* Two full-width stacked rows: a stack() button fills its column, so same_line would push the
           second button off the pane's right edge. */
        if ( gui()->button( "Save Layout" ) )    s_save_layout    = true;
        if ( gui()->button( "Restore Layout" ) ) s_restore_layout = true;
    }
    gui()->window_end();

    /* A FREE (undocked) window to exercise the drag-to-dock gesture: drag its title bar over
       any pane to see the 5-way overlay, then drop on center (tab in) or a side (split). */
    gui()->window_set_next_pos ( 980, 120, GUI_COND_ONCE );
    gui()->window_set_next_size( 220, 150, GUI_COND_ONCE );
    if ( gui()->window_begin( "Palette", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "I'm a floating window." );
        gui()->separator();
        gui()->text( "Drag my title bar over a" );
        gui()->text( "pane to dock me (center =" );
        gui()->text( "tab, sides = split)." );
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

    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_dock] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* One-call setup: gui owns the main window + render context end to end (boot path). */
    gui_vp_t vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui dock",
        .w     = 1280, .h = 720,
        .font  = GUI_FONT_JETBRAINS_16,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
        .debug = true,    // gui owns the debug hotkeys + overlays (see debug_enable, gui_api.h)
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_dock] gui->boot failed\n" );
        goto shutdown;
    }

    f32 dt = 0.0f;
    while ( gui()->frame_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            show_dock_scene( vp0 );
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->present_begin( NULL );
        gui()->present_end();
        gui()->frame_pace( 4, 16 );
    }

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();  /* also tears down the boot window + context */
    rhi()->shutdown();                               /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
// clang-format on
