/*==============================================================================================

    sandbox/gui/sb_gui_dock/sb_gui_dock.c -- docking lifecycle test bed.

    Exercises the dock system end to end, centered on the rule that a dockspace is EMIT-GATED
    like every other immediate-mode element: windows only exist on frames their code path runs,
    and they survive not being emitted -- the dock tree behaves identically.  The Dock menu
    swaps between two whole UI modes:

      DOCK VIEW  -- dockspace_over_viewport runs every frame; the panels tile into the tree.
      SWAP VIEW  -- the dock code path does not execute at all; a "Swap View" window takes half
                    the screen instead.  The tree goes DORMANT: retained but inert.  The panels
                    are still emitted by the host every frame (their code path still runs!) but
                    render nothing -- a window tabbed in a dormant tree suppresses, inactive-tab
                    style, instead of drawing pinned to rects that no longer lay out.  Title
                    drags offer no dock chips.  Swapping back revives the layout exactly as it
                    was, splitters, tabs and all.

    So the state ladder is: EMITTED (dockspace ran this frame) / DORMANT (tree retained, inert)
    / CLEARED (dock_clear destroyed it -- the only way state dies; panels free-float after).
    The "Dock Lab" window reports each window's docked state live so all three are observable.

    Menu actions set request flags processed at the top of the NEXT build -- the safe-point
    pattern for every tree-mutating verb (clear / load / rebuild).

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
    Lifecycle state
==============================================================================================*/

static bool s_swap_view   = false;   // true = the dock code path does not run; swap window instead
static bool s_tree_live   = false;   // host-side mirror: a tree exists (emitted since last clear)

/* Menu requests -- processed at the top of the next build (the safe point for tree mutation). */
static bool s_req_build   = true;    // clear + carve the default layout (initial state)
static bool s_req_clear   = false;   // destroy the tree (dock_clear)
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
        /* The swap: checking this stops the dockspace code path entirely (tree goes dormant);
           unchecking re-enters it (tree revives).  No teardown call either way -- that is the point. */
        gui()->menu_item( "Swap view (dock dormant)", NULL, &s_swap_view );

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
    Panels -- ALWAYS emitted, both view modes; that is the suppression test.  Each frame a panel
    either tiles into the emitted tree, suppresses (dormant tree holds its tab), or free-floats
    (no tree membership).
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

    /* DOCK_MAXIMIZE: while docked, the node's tab strip offers a maximize button (double-click
       the strip's empty band works too) that pins this pane over the WHOLE dockspace -- the
       other panels suppress while covered.  The Dock Lab readout shows them flip state. */
    if ( gui()->window_begin( "Viewport", GUI_WIN_DOCK_MAXIMIZE ) )
    {
        gui()->stack();
        gui()->text( "Central viewport panel." );
        gui()->separator();
        gui()->text( "Drag the gutters between regions to resize." );
        gui()->text( "Click the Console / Assets tabs to switch." );
        gui()->text( "Drag a tab OUT to pop it into a floater." );
        gui()->text( "Drag the Palette window onto a pane to dock." );
        gui()->separator();
        gui()->text( "Maximize (strip button / double-click) fills" );
        gui()->text( "the dockspace; the other panels suppress." );
        gui()->separator();
        gui()->text( "Lifecycle controls live in the Dock menu." );

        bool maxed = gui()->window_is_dock_maximized( "Viewport" );
        if ( gui()->button( maxed ? "Restore dock view" : "Fullscreen this pane" ) )
            gui()->dock_window_maximize( "Viewport", !maxed );
    }
    gui()->window_end();

    /* A FREE (undocked) window to exercise the drag-to-dock gesture: drag its title bar over
       any pane to see the 5-way overlay, then drop on center (tab in) or a side (split).
       In swap view the same drag offers NO chips -- the dormant tree accepts no drops. */
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
    Swap view -- the "other UI mode": one window claiming half the screen, dock path not run
==============================================================================================*/

static void
show_swap_view( i32 vp, f32 top )
{
    i32 win_w = 0, win_h = 0;
    app()->window_get_size( ( i32 )vp, &win_w, &win_h );

    static int clicks = 0;

    gui()->window_set_next_pos ( 0.0f, top, GUI_COND_ALWAYS );
    gui()->window_set_next_size( ( f32 )win_w * 0.5f, ( f32 )win_h - top, GUI_COND_ALWAYS );
    if ( gui()->window_begin( "Swap View", GUI_WIN_NOMOVE | GUI_WIN_NORESIZE | GUI_WIN_NOCOLLAPSE ) )
    {
        gui()->stack();
        gui()->text( "This mode does not run the dock code path AT ALL." );
        gui()->separator();
        gui()->text_wrapped( "The five panels are still emitted every frame -- their code path "
                             "runs -- but the tree that owns them is dormant, so they render "
                             "nothing.  The Palette and Dock Lab windows have no tab in the tree "
                             "and float as normal.  Try dragging a title bar: no dock chips." );
        gui()->separator();
        gui()->text_wrapped( "Now swap back (Dock > Swap view): the layout returns exactly as "
                             "you left it -- splits, tabs, active tab, splitter positions." );
        gui()->separator();
        if ( gui()->button( "A live widget, to prove this path is real" ) ) ++clicks;
        gui()->textf( "Clicks: %d", clicks );
    }
    gui()->window_end();
}

/*==============================================================================================
    Dock Lab -- live state readout, so EMITTED / DORMANT / CLEARED are observably different
==============================================================================================*/

static void
show_dock_lab( void )
{
    static const char* k_panels[] = { "Scene Tree", "Inspector", "Console", "Assets", "Viewport", "Palette" };

    gui()->window_set_next_pos ( 930, 320, GUI_COND_ONCE );
    gui()->window_set_next_size( 330, 370, GUI_COND_ONCE );
    if ( gui()->window_begin( "Dock Lab", GUI_WIN_NONE ) )
    {
        gui()->stack();

        const bool emitted = !s_swap_view && s_tree_live;
        const char* mode = emitted     ? "EMITTED"
                         : s_tree_live ? "DORMANT  (retained, inert)"
                                       : "CLEARED  (no tree)";
        gui()->textf( "Dockspace: %s", mode );
        gui()->separator();

        for ( u32 i = 0; i < ARRAY_COUNT( k_panels ); ++i )
        {
            const bool docked = gui()->window_is_docked( k_panels[ i ] );
            gui()->textf( "%-11s %s", k_panels[ i ],
                          !docked ? "floating" : emitted ? "docked" : "docked (suppressed)" );
        }
        gui()->separator();

        if ( emitted )
        {
            gui()->text_wrapped( "Normal docking.  Dock > Swap view stops the dock code path "
                                 "cold -- no teardown call -- and everything above should flip "
                                 "to 'docked (suppressed)' while the tree waits, intact." );
        }
        else if ( s_tree_live )
        {
            gui()->text_wrapped( "The dock code path is not executing, and the tree is parked: "
                                 "suppressed panels emit but render nothing, drags offer no "
                                 "chips, window_is_docked stays true.  Swap back and the layout "
                                 "revives exactly as it was.  This used to be the stale-state "
                                 "bug; dormancy is now the designed behavior." );
        }
        else
        {
            gui()->text_wrapped( "dock_clear destroyed the tree -- the only operation that "
                                 "does.  Every panel free-floats (membership gone for good). "
                                 "Build default layout carves it fresh." );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Build -- one frame of UI, requests first (the safe point), then chrome, then windows
==============================================================================================*/

static void
build_frame( i32 vp )
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
        /* dock_load frees + rebuilds the tree; jump to dock view so the result is visible. */
        if ( s_have_layout && gui()->dock_load( vp, s_layout ) )
        {
            s_swap_view = false;
            s_tree_live = true;
        }
        s_req_restore = false;
    }
    if ( s_req_clear )
    {
        /* Destroy the tree.  Note: if the dock view stays active, next frame's
           dockspace_over_viewport re-creates a bare root (an empty dockspace) -- clearing does
           not turn the dockspace off, it only discards the layout. */
        gui()->dock_clear( vp );
        s_tree_live = false;
        s_req_clear = false;
    }
    if ( s_req_build )
    {
        /* Rebuild = clear first (a second build over a live tree would split it further),
           then carve fresh below once the dockspace hands us the new bare root. */
        gui()->dock_clear( vp );
        s_swap_view = false;
    }

    /* Menu first (popup frame-ordering), then publish the band it occupies so the dock tree
       tiles below it.  The boot shell's caption band is inset by the dock tree on its own. */
    show_menu();
    gui()->dockspace_inset( vp, gui()->main_menu_bar_h() );

    if ( !s_swap_view )
    {
        /* DOCK VIEW: the dock code path.  Not running this block is the whole swap test. */
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
    else
    {
        /* SWAP VIEW: a different UI mode entirely; the dormant tree just waits. */
        show_swap_view( vp, gui()->viewport_caption_h( vp ) + gui()->main_menu_bar_h() );
    }

    show_panels();      /* both modes -- suppression under a dormant tree is the test */
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
    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui dock",
        .w     = 1280, .h = 720,
        .font  = GUI_FONT_JETBRAINS,
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
    while ( gui()->boot_poll( &dt ) )
    {
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin();
            build_frame( vp0 );
            gui()->ctx_end();
        }
        gui()->frame_end();

        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();
        gui()->boot_pace ( 4, 16 );
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
