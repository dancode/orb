/*==============================================================================================

    editor_api.c -- editor service wiring.
    Implements the editor_api_t vtable struct and the mod_desc_t lifecycle descriptor.

    The bare-bones editor shell: main menu bar (File / Window), a dockspace over the main
    gui viewport, and the first official editor windows --

        Viewport      the scene viewport (editor_service/viewport): the game's offscreen
                      render target in a docked panel; the play-in-editor surface
        Game          session controls over the game framework runner: Play / Stop /
                      Pause / Step + state readout, plus Play Standalone (spawns the
                      project under host_game.exe as its own process)
        Frame Stats   run clock readout (floating, off by default)
        Deploy        ship-pipeline front-end (File > Deploy...): configures and spawns
                      ship_tool detached, the batch-job heart -- no pipeline logic here

    Window visibility lives in the s_show_* flags: the Window menu toggles them and each
    window's close (X) button clears them back (CLOSEABLE + window_is_open sync, the
    pipeline-dashboard pattern), so menu checks and close boxes never disagree.
    The panel set is deliberately flat for now; it becomes a registry when panels grow
    past a handful (per the north star: registry + selection + undo land with the world).

    Consumes: gui (static gateway via GUI_STATIC), render (shared pointer the runtime
    host populates), game (pointer owned HERE -- fetched in init, host uses it too),
    run_host direct calls (same exe).

==============================================================================================*/

/*==============================================================================================
    Cached API pointers
==============================================================================================*/

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
static bool s_show_deploy   = false;
static bool s_dock_built    = false;     /* default layout carved once             */
static char s_project[ 64 ];             /* bound project display name; "" = none  */
static char s_project_dir[ 260 ];        /* dir holding <name>.dll; "" = exe dir   */

/* Deploy window options -- mirror ship_tool's CLI; the window is a front-end that spawns
   the tool, never a second implementation of the pipeline. */
static i32  s_ship_config  = 0;          /* index into s_ship_configs               */
static bool s_ship_modular = false;      /* host_game.exe + DLLs instead of mono exe */
static bool s_ship_pdb     = false;
static bool s_ship_clean   = true;       /* fresh stage by default: no stale-mix     */
static char s_ship_deploy_dir[ 260 ];    /* optional publish destination; "" = none  */

static const char* s_ship_configs[] = { "Release", "Debug" };

/*==============================================================================================
    API implementations
==============================================================================================*/

static bool
editor_project_bind( const char* name, const char* dir )
{
    if ( !name || !name[ 0 ] || !game() || !game()->project_bind( name ) )
        return false;

    snprintf( s_project, sizeof( s_project ), "%s", name );
    snprintf( s_project_dir, sizeof( s_project_dir ), "%s", dir ? dir : "" );
    LOG_INFO( "project '%s' bound -- use Play to start it", s_project );
    return true;
}

static void
editor_update( f32 dt )
{
    /* The one per-frame runner call -- a no-op while GAME_STOPPED.  View rebuilt every
       frame: render_ctx is the SCENE VIEWPORT's target, not a window swapchain -- the
       play-in-editor seam run_project.h names.  -1 = headless (viewport hidden or not
       yet sized); the project skips its draws but the sim still ticks. */
    if ( game() )
    {
        /* gui_vp -1: the editor never forwards on_hud -- a project HUD over the editor's
           own chrome is wrong, and HUD-inside-the-viewport-panel needs a rect-scoped form
           of the hook (future work, GUI_STACK_PLAN inc 6 notes).  Play Standalone shows it. */
        run_view_t view = {
            .version    = RUN_VIEW_VERSION,
            .render_ctx = s_show_viewport ? ed_viewport_render_ctx() : -1,
            .gui_vp     = -1,
        };
        ed_viewport_surface( &view.surface_w, &view.surface_h );
        game()->tick( dt, &view );
    }

    /* Live = a session is submitting scene draws this frame (playing OR paused -- the
       runner keeps frame/draw running while paused).  Hidden viewport gates the flip
       too: nothing samples the target, so let its buffers rest. */
    bool live = game() && game()->state() != GAME_STOPPED;

    /* AFTER the tick filled the target's submission bucket, BEFORE the gui emit bakes
       the target's texture index (the buffer flip lives in here). */
    bool recreated = ed_viewport_update( live && s_show_viewport );

    /* Pacing gates -- a live session must tick without blocking on OS input (realtime),
       and the viewport image changes every frame so clean-frame emit skips would freeze
       it (force-redraw).  Re-derived every frame so they survive every transition --
       buttons, host quits, a project ending its own session.

       A target (re)create latches force-redraw for one frame even when stopped: the old
       textures are destruction-deferred only a couple of epochs, so a clean-frame replay
       of the retained draw list -- still carrying the old bindless index -- would sample
       a freed image once reclaim runs (VK_ERROR_DEVICE_LOST).  The forced emit re-bakes
       the panel with the new index before that window closes. */
    run_host_realtime_set( live );

    if ( gui()->force_redraw() == true && gui()->debug_hotkeys_armed() )
    {
        // debug overrdie skips this forced on.
        return;
    }
    
    gui()->set_force_redraw( live || recreated );    
}

static void
editor_shutdown( void )
{
    if ( game() && game()->state() != GAME_STOPPED )
        game()->stop();

    ed_viewport_shutdown();
}

/*==============================================================================================
    Windows
==============================================================================================*/

/* Play Standalone -- spawn the bound project under host_game.exe as its OWN process (own
   window, own console, own hot-reload watcher).  Rebuilds the launch args from the bind:
   a dll dir round-trips through -project (host_resolve_project finds <dir>/<name>.dll),
   an exe-dir project needs only -module.  Fire-and-forget: the editor keeps no handle,
   and both processes may run the same session side by side. */
static void
editor_play_standalone( void )
{
    char exe_dir[ 260 ];
    sys_exe_dir( exe_dir, sizeof( exe_dir ) );

    char cmd[ 1024 ];
    if ( s_project_dir[ 0 ] )
        snprintf( cmd, sizeof( cmd ), "\"%s\\host_game.exe\" -project \"%s\" -module %s",
                  exe_dir, s_project_dir, s_project );
    else
        snprintf( cmd, sizeof( cmd ), "\"%s\\host_game.exe\" -module %s", exe_dir, s_project );

    if ( sys_process_spawn( cmd, NULL ) )
        LOG_INFO( "standalone launched: %s", cmd );
    else
        LOG_WARN( "standalone launch FAILED: %s", cmd );
}

/* Session controls -- the host's policy surface, unchanged behavior: Play/Stop restart
   the session, Pause freezes the sim while the scene keeps drawing, Step advances one
   sim tick while paused.  Play Standalone is session-independent -- it spawns a separate
   process and never touches the in-editor state. */
static void
editor_game_window( void )
{
    /* The shell flag says show: revive the pool entry if the X button hid it earlier. */
    gui()->window_set_open( "Game", true );
    if ( gui()->window_begin( "Game", GUI_WIN_CLOSEABLE ) )
    {
        gui()->stack();

        if ( s_project[ 0 ] && game() )
        {
            i32 st = game()->state();

            ed_prop_text( "project", s_project );

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

            ed_prop_text( "state", st == GAME_PLAYING ? "PLAYING"
                                 : st == GAME_PAUSED  ? "PAUSED"
                                                      : "stopped" );

            if ( gui()->button( "Play Standalone" ) )
                editor_play_standalone();
        }
        else
        {
            gui()->textf( "no project (-project <dir>)" );
        }
    }
    gui()->window_end();

    /* The X button closed it this frame: report back so the menu toggle stays in sync. */
    if ( !gui()->window_is_open( "Game" ) )
        s_show_game = false;
}

/* Deploy -- configure and launch the ship pipeline (build -> verify -> stage -> package ->
   deploy) for the bound project.  At its heart a batch job: the window only assembles a
   ship_tool command line and spawns it detached (own console shows the pipeline log),
   exactly the Play Standalone pattern.  The editor keeps no handle and no pipeline state. */

static void
editor_ship( void )
{
    /* Run through cmd.exe with a trailing pause so the console survives after the pipeline
       finishes -- otherwise the window closes before the log can be read. */
    char cmd[ 1024 ];
    int  n = snprintf( cmd, sizeof( cmd ), "cmd.exe /c \"\"%s\\bin\\ship_tool.exe\" %s -config %s",
                       sys_root_dir(), s_project, s_ship_configs[ s_ship_config ] );

    if ( s_ship_modular ) n += snprintf( cmd + n, sizeof( cmd ) - n, " -modular" );
    if ( s_ship_pdb )     n += snprintf( cmd + n, sizeof( cmd ) - n, " -pdb" );
    if ( s_ship_clean )   n += snprintf( cmd + n, sizeof( cmd ) - n, " -clean" );
    if ( s_ship_deploy_dir[ 0 ] )
        n += snprintf( cmd + n, sizeof( cmd ) - n, " -deploy \"%s\"", s_ship_deploy_dir );

    snprintf( cmd + n, sizeof( cmd ) - n, " & pause\"" );

    if ( sys_process_spawn( cmd, sys_root_dir() ) )
        LOG_INFO( "ship launched: %s", cmd );
    else
        LOG_WARN( "ship launch FAILED: %s", cmd );
}

static void
editor_deploy_window( void )
{
    gui()->window_set_open( "Deploy", true );
    gui()->window_set_next_pos ( 460.0f, 160.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 340.0f, 240.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Deploy", GUI_WIN_CLOSEABLE ) )
    {
        gui()->stack();

        if ( s_project[ 0 ] )
        {
            /* Property rows (ed_kit): the label column names the option, the value zone
               holds the stock control -- the editor's inspector idiom. */
            ed_prop_text( "project", s_project );

            ed_prop_begin( "config" );
            gui()->combo( "##ship_config", &s_ship_config, s_ship_configs,
                          (i32)( sizeof( s_ship_configs ) / sizeof( s_ship_configs[ 0 ] ) ) );
            ed_prop_end();

            ed_prop_begin( "modular" );
            gui()->checkbox( "host + DLLs", &s_ship_modular );
            ed_prop_end();

            ed_prop_begin( "pdbs" );
            gui()->checkbox( "include", &s_ship_pdb );
            ed_prop_end();

            ed_prop_begin( "stage" );
            gui()->checkbox( "clean first", &s_ship_clean );
            ed_prop_end();

            ed_prop_begin( "deploy to" );
            gui()->input_text_with_hint( "##ship_deploy", "(optional dir)",
                                         s_ship_deploy_dir, sizeof( s_ship_deploy_dir ) );
            ed_prop_end();

            if ( gui()->button( "Ship It" ) )
                editor_ship();

            gui()->textf( "-> build\\ship\\%s", s_project );
        }
        else
        {
            gui()->textf( "no project bound" );
        }
    }
    gui()->window_end();

    if ( !gui()->window_is_open( "Deploy" ) )
        s_show_deploy = false;
}

static void
editor_stats_window( void )
{
    gui()->window_set_open( "Frame Stats", true );
    gui()->window_set_next_pos ( 420.0f, 120.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 300.0f, 160.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Frame Stats", GUI_WIN_CLOSEABLE ) )
    {
        gui()->stack();
        const run_clock_t* clk = run()->clock();
        gui()->textf( "frame  %llu", ( unsigned long long )clk->frame_number );
        gui()->textf( "time   %.1fs", clk->app_time );
        gui()->textf( "dt     %.2f ms", clk->dt * 1000.0f );
    }
    gui()->window_end();

    if ( !gui()->window_is_open( "Frame Stats" ) )
        s_show_stats = false;
}

/*==============================================================================================
    Shell build -- one call per frame from the host's on_gui.
==============================================================================================*/

static void
editor_build_gui( f32 dt )
{
    UNUSED( dt );

    /* Menu bar (it insets below the borderless caption itself), then the dock area
       reserves the menu band -- the dock tree adds the caption inset on its own. */
    if ( gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "File" ) )
        {
            bool deploy = false;
            if ( gui()->menu_item( "Deploy...", NULL, &deploy ) )
                s_show_deploy = true;

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
            gui()->menu_item( "Deploy",      NULL, &s_show_deploy   );
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

    i32 vp = run_host_vp();
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

    if ( s_show_viewport )
    {
        /* The panel lives in editor_service; the open handshake stays here with the flag. */
        gui()->window_set_open( "Viewport", true );
        ed_viewport_panel();
        if ( !gui()->window_is_open( "Viewport" ) )
            s_show_viewport = false;
    }
    if ( s_show_game     ) editor_game_window();
    if ( s_show_stats    ) editor_stats_window();
    if ( s_show_deploy   ) editor_deploy_window();

    /* F11: fullscreen the scene viewport over the dockspace (and back) -- the hotkey twin of
       the tab strip's maximize button.  A function key, so no capture fence needed: the key
       router only hands it here when nothing below claimed it. */
    if ( s_show_viewport && gui()->is_key_pressed( APP_KEY_F11 ) )
        gui()->dock_window_maximize( "Viewport", !gui()->window_is_dock_maximized( "Viewport" ) );
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const editor_api_t g_editor_api_struct = {
    .project_bind = editor_project_bind,
    .update       = editor_update,
    .build_gui    = editor_build_gui,
    .shutdown     = editor_shutdown,
};

/*==============================================================================================
    Module lifecycle + descriptor
==============================================================================================*/

static bool
editor_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );

    if ( !MOD_FETCH_GAME || !MOD_FETCH_RUN )
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

    if ( !MOD_FETCH_GAME || !MOD_FETCH_RUN )
        return false;
    return true;
}

/* Module teardown funnels through the public shutdown: stop a live session, then release
   the viewport's GPU resources.  Hosts that already called editor()->shutdown() (quit key,
   close request) make this a no-op -- both paths are idempotent. */
static void
editor_mod_exit( void* state )
{
    UNUSED( state );
    editor_shutdown();
}

static mod_desc_t s_editor_mod_desc = {
    .version       = 1,
    .state_size    = 0,
    .func_api_size = sizeof( editor_api_t ),
    .func_api      = &g_editor_api_struct,
    .dep_count     = 5,
    .deps          = { "core", "gui", "render", "game", "run" },
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
