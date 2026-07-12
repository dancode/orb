/*==============================================================================================

    sandbox/gui/sb_gui_dock/sb_gui_dock.c -- docking lifecycle test bed.

    Exercises the dock system END TO END, including the part sb_gui_example got wrong: turning
    docking OFF again.  A viewport becomes dock-capable on its first dockspace_over_viewport()
    call and STAYS capable until dock_clear() -- the tree is not tied to the host emitting the
    dockspace.  The Dock menu drives the viewport through all three lifecycle states:

      LIVE   -- dockspace_over_viewport runs every frame, tree laid out + interactive.  Normal.
      STALE  -- the host stopped emitting the dockspace but never cleared the tree (the
                sb_gui_example demo-switch bug, reproduced on purpose).  Docked windows stay
                pinned to their LAST-laid-out node rects (resize the OS window: they no longer
                re-tile), the splitters are gone, and every title drag still offers dock chips
                into the invisible tree.
      CLEAN  -- dock_clear() freed the tree: ex-docked windows return to free-floating at their
                last rect, drags offer no chips, the viewport is truly out of the docking
                business.  The proper OFF.

    The correct teardown a host must perform when switching a viewport out of docking is thus
    BOTH halves: stop calling dockspace_over_viewport AND call dock_clear (at a safe point --
    top of the build, before any docked window's window_begin; same rule as dock_load).  The
    "Dock Lab" window reports each window's docked state live so the difference is visible.

    Menu actions all set request flags; the requests are processed at the top of the NEXT build
    -- the safe-point pattern for every tree-mutating verb (clear / load / rebuild).

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
    Lifecycle state -- what the host believes about viewport 0's docking
==============================================================================================*/

static bool s_emit_dock   = true;    // host calls dockspace_over_viewport this frame
static bool s_tree_live   = false;   // host-side mirror: a tree exists (emitted since last clear)

/* Menu requests -- processed at the top of the next build (the safe point for tree mutation). */
static bool s_req_build   = true;    // clear + carve the default layout (initial state)
static bool s_req_clear   = false;   // proper teardown: dock_clear + stop emitting
static bool s_req_save    = false;
static bool s_req_restore = false;

/* Layout persistence blob -- round-trips in RAM (a real app would write it to a file). */
static char s_layout[ 2048 ];
static bool s_have_layout = false;

/*==============================================================================================
    Menu -- the lifecycle controls
==============================================================================================*/

static void
show_menu( void )
{
    if ( !gui()->main_menu_bar_begin() )
        return;

    if ( gui()->menu_begin( "Dock" ) )
    {
        /* Unchecking this WITHOUT clearing is the bug repro: the tree stays live (STALE). */
        gui()->menu_item( "Emit dockspace", NULL, &s_emit_dock );

        if ( gui()->menu_item( "Build default layout", NULL, NULL ) ) s_req_build = true;
        if ( gui()->menu_item( "Clear dock state",     NULL, NULL ) ) s_req_clear = true;
        gui()->separator();
        if ( gui()->menu_item( "Save layout",          NULL, NULL ) ) s_req_save    = true;
        if ( gui()->menu_item( "Restore layout",       NULL, NULL ) ) s_req_restore = true;
        gui()->menu_end();
    }

    gui()->main_menu_bar_end();
}

/*==============================================================================================
    Panels -- always emitted; each frame they either dock (tree has their tab) or free-float
==============================================================================================*/

static void
show_panels( void )
{
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
        gui()->text( "Lifecycle controls live in the Dock menu." );
    }
    gui()->window_end();

    /* A FREE (undocked) window to exercise the drag-to-dock gesture: drag its title bar over
       any pane to see the 5-way overlay, then drop on center (tab in) or a side (split).
       In the STALE state this drag still offers chips -- that is the bug on display. */
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
    Dock Lab -- live state readout, so LIVE / STALE / CLEAN are observably different
==============================================================================================*/

static void
show_dock_lab( void )
{
    static const char* k_panels[] = { "Scene Tree", "Inspector", "Console", "Assets", "Viewport", "Palette" };

    gui()->window_set_next_pos ( 930, 320, GUI_COND_ONCE );
    gui()->window_set_next_size( 330, 360, GUI_COND_ONCE );
    if ( gui()->window_begin( "Dock Lab", GUI_WIN_NONE ) )
    {
        gui()->stack();

        const char* mode = s_emit_dock ? "LIVE"
                         : s_tree_live ? "STALE  (bug repro)"
                                       : "CLEAN";
        gui()->textf( "State: %s", mode );
        gui()->textf( "emit dockspace: %s   tree live: %s",
                      s_emit_dock ? "yes" : "no", s_tree_live ? "yes" : "no" );
        gui()->separator();

        for ( u32 i = 0; i < ARRAY_COUNT( k_panels ); ++i )
            gui()->textf( "%-11s %s", k_panels[ i ],
                          gui()->window_is_docked( k_panels[ i ] ) ? "docked" : "floating" );
        gui()->separator();

        if ( s_emit_dock )
        {
            gui()->text_wrapped( "Normal docking.  Now uncheck Dock > Emit dockspace: the host "
                                 "stops laying the tree out but never frees it -- watch what "
                                 "the leftover state does." );
        }
        else if ( s_tree_live )
        {
            gui()->text_wrapped( "The dockspace is gone but its tree was never cleared -- the "
                                 "sb_gui_example demo-switch bug.  The panels above still read "
                                 "'docked': they render pinned to their last node rects (resize "
                                 "the OS window -- they no longer re-tile), the splitters are "
                                 "dead, and dragging any title bar still offers dock chips into "
                                 "the invisible tree.  Dock > Clear dock state is the fix." );
        }
        else
        {
            gui()->text_wrapped( "dock_clear() freed the tree: every panel reads 'floating', "
                                 "drags offer no chips, the viewport is out of the docking "
                                 "business.  Dock > Build default layout starts over; Emit "
                                 "dockspace alone gives an empty dockspace to hand-build." );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Build -- one frame of UI, requests first (the safe point), then chrome, then windows
==============================================================================================*/

static void
build_frame( gui_vp_t vp )
{
    /* --- Safe point: top of the build, before the dockspace and any docked window_begin. ---
       Every tree mutation lands here, one frame after its menu click. */
    if ( s_req_save )
    {
        gui()->dock_save( vp, s_layout, sizeof( s_layout ) );
        s_have_layout = true;
        s_req_save    = false;
    }
    if ( s_req_restore )
    {
        /* dock_load frees + rebuilds the tree; restoring implies we want it laid out again. */
        if ( s_have_layout && gui()->dock_load( vp, s_layout ) )
        {
            s_emit_dock = true;
            s_tree_live = true;
        }
        s_req_restore = false;
    }
    if ( s_req_clear )
    {
        /* THE proper teardown -- both halves: free the tree AND stop emitting the dockspace. */
        gui()->dock_clear( vp );
        s_emit_dock = false;
        s_tree_live = false;
        s_req_clear = false;
    }
    if ( s_req_build )
    {
        /* Rebuild = clear first (a second build over a live tree would split it further),
           then carve fresh below once the dockspace hands us the new bare root. */
        gui()->dock_clear( vp );
        s_emit_dock = true;
    }

    /* Menu first (popup frame-ordering), then publish the band it occupies so the dock tree
       tiles below it.  The boot shell's caption band is inset by the dock tree on its own. */
    show_menu();
    gui()->dockspace_inset( vp, gui()->main_menu_bar_h() );

    if ( s_emit_dock )
    {
        gui_dock_id_t root = gui()->dockspace_over_viewport( vp, GUI_DOCKSPACE_NONE );
        if ( root != GUI_DOCK_NONE )
        {
            s_tree_live = true;
            if ( s_req_build )
            {
                /* Left rail, right inspector, bottom strip, central viewport -- the DockBuilder
                   idiom of splitting the shrinking remainder. */
                gui_dock_id_t left   = gui()->dock_split( root, GUI_DIR_LEFT,  0.22f, &root );
                gui_dock_id_t right  = gui()->dock_split( root, GUI_DIR_RIGHT, 0.28f, &root );
                gui_dock_id_t bottom = gui()->dock_split( root, GUI_DIR_DOWN,  0.30f, &root );
                gui()->dock_window( "Scene Tree", left   );
                gui()->dock_window( "Inspector",  right  );
                gui()->dock_window( "Console",    bottom );
                gui()->dock_window( "Assets",     bottom );   /* tab alongside Console */
                gui()->dock_window( "Viewport",   root   );   /* central remainder */
                s_req_build = false;
            }
        }
    }

    show_panels();
    show_dock_lab();
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
            build_frame( vp0 );
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
