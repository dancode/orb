/*==============================================================================================

    editor.c : editor framework static service.

    The bare-bones editor shell: main menu bar (File / Window), a dockspace over the main
    gui viewport, and the first official editor windows --

        Viewport      the scene viewport (editor_service/viewport): the game's offscreen
                      render target in a docked panel; the play-in-editor surface
        Game          session controls over the game framework runner: Play / Stop /
                      Pause / Step + state readout
        Frame Stats   run clock readout (floating, off by default)

    Window visibility lives in the Window menu -- one source of truth, no close boxes yet.
    The panel set is deliberately flat for now; it becomes a registry when panels grow
    past a handful (per the north star: registry + selection + undo land with the world).

    Consumes: gui (static gateway via GUI_STATIC), render (shared pointer the runtime
    host populates), game (pointer owned HERE -- fetched in init, host uses it too),
    run_host direct calls (same exe).

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#define LOG_CH "editor"

#include "engine/mod/mod_export.h"
#include "engine/core/core_api.h"
#include "runtime_service/gui/gui_api.h"
#include "runtime_modules/render/render_api.h"
#include "game/game_api.h"
#include "runtime/run_api.h"
#include "runtime/run_host.h"
#include "editor_service/viewport/viewport.h"
#include "editor/editor_api.h"

/* The exe-wide game api pointer lives here: the editor service is game()'s dep-ordered
   consumer (fetched in init below); the host's session calls read the same global. */
MOD_USE_GAME;
MOD_USE_RUN;

/*==============================================================================================
    Shell state -- statics, matching the gui service precedent (state_size 0): the editor
    is a static service, never hot-reloaded, so module-system state buys nothing yet.
==============================================================================================*/

static bool s_show_viewport = true;
static bool s_show_game     = true;
static bool s_show_stats    = false;
static bool s_dock_built    = false;     /* default layout carved once             */
static char s_project[ 64 ];             /* bound project display name; "" = none  */

/*==============================================================================================
    API implementations
==============================================================================================*/

static void
editor_set_project_impl( const char* name )
{
    if ( name && name[ 0 ] )
        snprintf( s_project, sizeof( s_project ), "%s", name );
    else
        s_project[ 0 ] = '\0';
}

static void
editor_update_impl( f32 dt )
{
    UNUSED( dt );

    /* Live = a session is submitting scene draws this frame (playing OR paused -- the
       runner keeps frame/draw running while paused).  Hidden viewport gates the flip
       too: nothing samples the target, so let its buffers rest. */
    bool live = game() && game()->state() != GAME_STOPPED;
    ed_viewport_update( live && s_show_viewport );
}

static i32
editor_view_ctx_impl( void )
{
    return s_show_viewport ? ed_viewport_render_ctx() : -1;
}

static void
editor_view_size_impl( i32* w, i32* h )
{
    ed_viewport_surface( w, h );
}

/*==============================================================================================
    Windows
==============================================================================================*/

/* Session controls -- the host's policy surface, unchanged behavior: Play/Stop restart
   the session, Pause freezes the sim while the scene keeps drawing, Step advances one
   sim tick while paused. */
static void
editor_game_window( void )
{
    if ( gui()->window_begin( "Game", GUI_WIN_NONE ) )
    {
        gui()->stack();

        if ( s_project[ 0 ] && game() )
        {
            i32 st = game()->state();

            gui()->textf( "project: %s", s_project );

            if ( gui()->button( st == GAME_STOPPED ? "Play" : "Stop" ) )
            {
                if ( st == GAME_STOPPED )
                    game()->play();
                else
                    game()->stop();
            }

            if ( st != GAME_STOPPED )
            {
                if ( gui()->button( st == GAME_PAUSED ? "Resume" : "Pause" ) )
                    game()->pause( st != GAME_PAUSED );

                if ( st == GAME_PAUSED && gui()->button( "Step" ) )
                    game()->step();
            }

            gui()->textf( "state  %s", st == GAME_PLAYING ? "PLAYING"
                                     : st == GAME_PAUSED  ? "PAUSED"
                                                          : "stopped" );
        }
        else
        {
            gui()->textf( "no project (-project <dir>)" );
        }
    }
    gui()->window_end();
}

static void
editor_stats_window( void )
{
    gui()->window_set_next_pos ( 420.0f, 120.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 300.0f, 160.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Frame Stats", GUI_WIN_NONE ) )
    {
        gui()->stack();
        const run_clock_t* clk = run()->clock();
        gui()->textf( "frame  %llu", ( unsigned long long )clk->frame_number );
        gui()->textf( "time   %.1fs", clk->app_time );
        gui()->textf( "dt     %.2f ms", clk->dt * 1000.0f );
    }
    gui()->window_end();
}

/*==============================================================================================
    Shell build -- one call per frame from the host's on_gui.
==============================================================================================*/

static void
editor_build_gui_impl( f32 dt )
{
    UNUSED( dt );

    /* Menu bar (it insets below the borderless caption itself), then the dock area
       reserves the menu band -- the dock tree adds the caption inset on its own. */
    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "File" ) )
        {
            bool quit = false;
            if ( gui()->menu_item( "Quit", NULL, &quit ) )
                run_host_quit();
            gui()->menu_end();
        }
        if ( gui()->menu_begin( "Window" ) )
        {
            gui()->menu_item( "Viewport",    NULL, &s_show_viewport );
            gui()->menu_item( "Game",        NULL, &s_show_game     );
            gui()->menu_item( "Frame Stats", NULL, &s_show_stats    );
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

    gui_vp_t vp = run_host_vp();
    gui()->dockspace_inset( vp, gui()->main_menu_bar_h() );
    gui_dock_id_t root = gui()->dockspace_over_viewport( vp, GUI_DOCKSPACE_NONE );

    /* Carve the default layout once: session controls left, scene viewport center. */
    if ( !s_dock_built && root != GUI_DOCK_NONE )
    {
        gui_dock_id_t left = gui()->dock_split( root, GUI_DIR_LEFT, 0.22f, &root );

        gui()->dock_window( "Game",     left );
        gui()->dock_window( "Viewport", root );
        s_dock_built = true;
    }

    if ( s_show_viewport ) ed_viewport_panel();
    if ( s_show_game     ) editor_game_window();
    if ( s_show_stats    ) editor_stats_window();
}

/*==============================================================================================
    Public API struct
==============================================================================================*/

static void
editor_shutdown_impl( void )
{
    ed_viewport_shutdown();
}

const editor_api_t g_editor_api_struct = {
    .set_project = editor_set_project_impl,
    .update      = editor_update_impl,
    .build_gui   = editor_build_gui_impl,
    .view_ctx    = editor_view_ctx_impl,
    .view_size   = editor_view_size_impl,
    .shutdown    = editor_shutdown_impl,
};

/*==============================================================================================
    Module lifecycle + descriptor
==============================================================================================*/

static bool
editor_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );

    if ( !MOD_FETCH_GAME )
        return false;

    /* render() is the runtime host's pointer, populated after mod_init_all -- not
       fetched here (init order); the viewport lib NULL-guards until it lands. */
    return true;
}

static bool
editor_mod_reload( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );

    if ( !MOD_FETCH_GAME )
        return false;
    return true;
}

static void
editor_mod_exit( void* state )
{
    UNUSED( state );
    ed_viewport_shutdown();
}

static mod_desc_t s_editor_mod_desc = {
    .version       = 1,
    .state_size    = 0,
    .func_api_size = sizeof( editor_api_t ),
    .func_api      = &g_editor_api_struct,
    .dep_count     = 4,
    .deps          = { "core", "gui", "render", "game" },
    .init          = editor_mod_init,
    .reload        = editor_mod_reload,
    .exit          = editor_mod_exit,
};

mod_desc_t*
editor_get_mod_desc( void )
{
    return &s_editor_mod_desc;
}

/*============================================================================================*/
