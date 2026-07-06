/*==============================================================================================

    sandbox/gui_editor/ed_shell.c -- Editor chrome: main menu bar, toolbar, dockspace with the
    traditional panel layout, and layout save/load/reset.

    Build order each frame: menu bar + toolbar first (popup frame-ordering), then the top band
    they occupy is published via dockspace_inset so the dock tree tiles the panels below them.
    The default layout is carved once with the dock_split remainder idiom; its serialized blob
    is captured immediately so "Reset Layout" can restore it later.  Save/Load round-trip the
    blob through a text file next to the working directory.

    Included by sb_gui_editor.c (unity build).

==============================================================================================*/
// clang-format off

#define ED_LAYOUT_FILE  "sb_gui_editor_layout.txt"
#define ED_LAYOUT_MAX   4096

static char s_default_layout[ ED_LAYOUT_MAX ];  /* the freshly-built default tree, for Reset */
static bool s_have_default;
static bool s_built;                            /* default layout carved */
static bool s_do_save, s_do_load, s_do_reset;   /* menu requests, applied at the next safe point */
static bool s_rounded_theme;                    /* Window > Rounded Theme toggle state */
static bool s_do_theme;                         /* deferred: theme_set clears the style stacks,
                                                   so apply it between builds, not mid-menu */

/*==============================================================================================
    Layout persistence
==============================================================================================*/

static void
ed_layout_save_file( void )
{
    char buf[ ED_LAYOUT_MAX ];
    u32  n = gui()->dock_save( 0, buf, sizeof( buf ) );
    if ( n == 0 || n >= sizeof( buf ) )
    {
        ed_logf( ED_LOG_ERROR, "Layout save failed (%u bytes)", n );
        return;
    }
    FILE* f = fopen( ED_LAYOUT_FILE, "wb" );
    if ( !f )
    {
        ed_logf( ED_LOG_ERROR, "Cannot write " ED_LAYOUT_FILE );
        return;
    }
    fwrite( buf, 1, n, f );
    fclose( f );
    ed_logf( ED_LOG_INFO, "Layout saved to " ED_LAYOUT_FILE " (%u bytes)", n );
}

static void
ed_layout_load_file( void )
{
    FILE* f = fopen( ED_LAYOUT_FILE, "rb" );
    if ( !f )
    {
        ed_logf( ED_LOG_WARN, "No " ED_LAYOUT_FILE " to load" );
        return;
    }
    char buf[ ED_LAYOUT_MAX ];
    u32  n = (u32)fread( buf, 1, sizeof( buf ) - 1, f );
    fclose( f );
    buf[ n ] = '\0';

    if ( gui()->dock_load( 0, buf ) )
        ed_logf( ED_LOG_INFO, "Layout loaded from " ED_LAYOUT_FILE );
    else
        ed_logf( ED_LOG_ERROR, "Layout load failed (bad blob?)" );
}

/*==============================================================================================
    Menu bar + toolbar
==============================================================================================*/

static void
ed_menu_bar( void )
{
    if ( !gui()->main_menu_bar_begin() )
        return;

    bool clicked = false;

    if ( gui()->menu_begin( "File" ) )
    {
        if ( gui()->menu_item( "Save Layout",  NULL, &clicked ) ) s_do_save  = true;
        if ( gui()->menu_item( "Load Layout",  NULL, &clicked ) ) s_do_load  = true;
        if ( gui()->menu_item( "Reset Layout", NULL, &clicked ) ) s_do_reset = true;
        gui()->separator();
        if ( gui()->menu_item( "Exit", NULL, &clicked ) )
            g_ed.request_quit = true;
        gui()->menu_end();
    }

    if ( gui()->menu_begin( "Edit" ) )
    {
        if ( gui()->menu_item( "Duplicate Selected", NULL, &clicked ) )
            g_ed.selected = ed_entity_duplicate( g_ed.selected );
        if ( gui()->menu_item( "Delete Selected", NULL, &clicked ) )
            ed_entity_delete( g_ed.selected );
        gui()->menu_end();
    }

    if ( gui()->menu_begin( "Entity" ) )
    {
        if ( gui()->menu_item( "Add Mesh",   NULL, &clicked ) ) g_ed.selected = ed_entity_add( ED_KIND_MESH );
        if ( gui()->menu_item( "Add Light",  NULL, &clicked ) ) g_ed.selected = ed_entity_add( ED_KIND_LIGHT );
        if ( gui()->menu_item( "Add Camera", NULL, &clicked ) ) g_ed.selected = ed_entity_add( ED_KIND_CAMERA );
        gui()->menu_end();
    }

    if ( gui()->menu_begin( "Window" ) )
    {
        gui()->menu_item( "Scene",     NULL, &g_ed.show_viewport  );
        gui()->menu_item( "Hierarchy", NULL, &g_ed.show_hierarchy );
        gui()->menu_item( "Inspector", NULL, &g_ed.show_inspector );
        gui()->menu_item( "Console",   NULL, &g_ed.show_console   );
        gui()->menu_item( "Assets",    NULL, &g_ed.show_assets    );
        gui()->separator();
        if ( gui()->menu_item( "Rounded Theme", NULL, &s_rounded_theme ) )
            s_do_theme = true;
        gui()->menu_end();
    }

    if ( gui()->menu_begin( "Help" ) )
    {
        if ( gui()->menu_item( "About", NULL, &clicked ) )
            ed_logf( ED_LOG_INFO, "sb_gui_editor -- ORB gui editor-shell sandbox" );
        gui()->menu_end();
    }

    gui()->main_menu_bar_end();
}

/* Pinned strip below the menu bar: play-mode transport + a status readout. */
static void
ed_toolbar( f32 y, f32 h )
{
    gui()->window_set_next_pos ( 0.0f, y, GUI_COND_ALWAYS );
    gui()->window_set_next_size( (f32)g_ed.disp_w, h, GUI_COND_ALWAYS );

    if ( gui()->window_begin( "##Toolbar", GUI_WIN_NOTITLEBAR | GUI_WIN_NOMOVE | GUI_WIN_NORESIZE
                                         | GUI_WIN_NOCOLLAPSE | GUI_WIN_NOSCROLL | GUI_WIN_NO_DETACH ) )
    {
        gui()->bar();

        gui()->disabled_begin( g_ed.mode == ED_MODE_PLAY );
        if ( gui()->button( "Play" ) )  ed_play();
        gui()->disabled_end();

        gui()->disabled_begin( g_ed.mode != ED_MODE_PLAY );
        if ( gui()->button( "Pause" ) ) ed_pause();
        gui()->disabled_end();

        gui()->disabled_begin( g_ed.mode == ED_MODE_EDIT );
        if ( gui()->button( "Stop" ) )  ed_stop();
        gui()->disabled_end();

        /* Live-viewport toggle (UE "Realtime"): on = scene pass every frame; off = the scene
           re-renders only on interaction/change (see the scheduling note in the host loop). */
        gui()->checkbox( "Realtime", &g_ed.realtime );

        static const char* mode_names[] = { "Edit", "Playing", "Paused" };
        gui()->textf( "  %s   t=%.1fs", mode_names[ g_ed.mode ], g_ed.sim_time );

        i32 count = 0;
        for ( i32 i = 0; i < ED_MAX_ENTITIES; i++ )
            if ( g_ed.entities[ i ].used )
                count++;
        gui()->textf( "  |  %d entities", count );
    }
    gui()->window_end();
}

/*==============================================================================================
    Shell build -- one call per frame from the host's emit block.
==============================================================================================*/

void
ed_shell_build( void )
{
    /* Deferred layout requests from LAST frame's menu clicks -- applied here, at the top of the
       build before the dockspace and any docked window (the safe point dock_load requires). */
    /* Seed the menu toggle from whatever theme the host started with (-theme arg). */
    static bool s_theme_synced;
    if ( !s_theme_synced )
    {
        const char* t = gui()->theme_get();
        s_rounded_theme = ( t && strcmp( t, "rounded" ) == 0 );
        s_theme_synced  = true;
    }

    if ( s_do_theme )
    {
        gui()->theme_set( s_rounded_theme ? "rounded" : "dark" );
        ed_logf( ED_LOG_INFO, "Theme: %s", gui()->theme_get() );
        s_do_theme = false;
    }
    if ( s_do_save )  { ed_layout_save_file(); s_do_save = false; }
    if ( s_do_load )  { ed_layout_load_file(); s_do_load = false; }
    if ( s_do_reset )
    {
        if ( s_have_default && gui()->dock_load( 0, s_default_layout ) )
            ed_logf( ED_LOG_INFO, "Layout reset to default" );
        s_do_reset = false;
    }

    /* Chrome: the native frame shell is auto-emitted by the boot path (first in this context's
       build), so only its caption band height is needed here -- 0 with OS chrome.  Menu bar
       next (it insets below the caption itself; menus own the popup ordering), then the toolbar
       stacked below both.  The dock area reserves only the menu + toolbar band -- the dock tree
       adds the caption inset on its own. */
    f32 caption_h = gui()->viewport_caption_h( 0 );

    ed_menu_bar();

    f32 menu_h    = gui()->calc_row( gui()->text_h( "A" ) ) + gui()->style_get()->widget_gap;
    f32 toolbar_h = menu_h + 6.0f;
    ed_toolbar( caption_h + menu_h, toolbar_h );

    gui()->dockspace_inset( 0, menu_h + toolbar_h );
    gui_dock_id_t root = gui()->dockspace_over_viewport( 0, GUI_DOCKSPACE_NONE );

    /* Carve the traditional editor layout once: left rail, right inspector, bottom strip,
       central scene view -- the dock_split shrinking-remainder idiom. */
    if ( !s_built && root != GUI_DOCK_NONE )
    {
        gui_dock_id_t left   = gui()->dock_split( root, GUI_DIR_LEFT,  0.18f, &root );
        gui_dock_id_t right  = gui()->dock_split( root, GUI_DIR_RIGHT, 0.28f, &root );
        gui_dock_id_t bottom = gui()->dock_split( root, GUI_DIR_DOWN,  0.30f, &root );

        gui()->dock_window( "Hierarchy", left   );
        gui()->dock_window( "Inspector", right  );
        gui()->dock_window( "Console",   bottom );
        gui()->dock_window( "Assets",    bottom );   /* tab beside Console */
        gui()->dock_window( "Scene",     root   );   /* central remainder */
        s_built = true;

        /* Capture the default tree for Reset Layout. */
        u32 n = gui()->dock_save( 0, s_default_layout, sizeof( s_default_layout ) );
        s_have_default = ( n > 0 && n < sizeof( s_default_layout ) );
    }

    /* The panels. */
    if ( g_ed.show_viewport  ) ed_viewport_panel();
    if ( g_ed.show_hierarchy ) ed_hierarchy_panel();
    if ( g_ed.show_inspector ) ed_inspector_panel();
    if ( g_ed.show_console   ) ed_console_panel();
    if ( g_ed.show_assets    ) ed_assets_panel();
}

// clang-format on
/*============================================================================================*/
