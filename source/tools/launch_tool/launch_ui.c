/*==============================================================================================

    tools/launch_tool/launch_ui.c -- launcher UI and command invocation.

    Menu-driven: every action lives in the main menu bar, and each pane (Engine, Projects,
    Output) is a window toggled from the Windows menu.  Visual arrangement is deliberately
    plain for now -- free windows placed below the viewport chrome (viewport_content_y).
    Every action is a spawned command line against the engine's bin/ tools; nothing is
    re-implemented.

==============================================================================================*/

// clang-format off

/* Pane visibility -- menu-toggled; all on by default until a real arrangement lands. */
static bool s_show_engine   = true;
static bool s_show_projects = true;
static bool s_show_output   = true;

static gui_vp_t s_main_vp = 0;  /* the main viewport hosting the chrome shell and free windows */

/*==============================================================================================
    Command invocation
==============================================================================================*/

/* Run a command synchronously, capturing combined stdout/stderr into the Output pane.
   Fine for fast commands (doctor, query); long builds move to async capture later. */
static void
launch_run_capture( const char* label, const char* command_line, const char* working_dir )
{
    sys_process_result_t result = { 0 };

    snprintf( s_launch.log_title, sizeof( s_launch.log_title ), "%s", label );
    s_launch.log[ 0 ]   = '\0';
    s_launch.log_valid  = true;
    s_show_output       = true;    /* an action's output must be visible */

    if ( !sys_process_run_capture( command_line, working_dir,
                                   s_launch.log, sizeof( s_launch.log ), NULL, &result ) )
    {
        snprintf( s_launch.log, sizeof( s_launch.log ), "[launch] failed to start: %s\n", command_line );
        s_launch.last_exit_code = -1;
        return;
    }
    s_launch.last_exit_code = result.exit_code;
}

/* Fire-and-forget spawn (hosts, editors); the child owns its own lifetime and console. */
static void
launch_spawn( const char* label, const char* command_line, const char* working_dir )
{
    snprintf( s_launch.log_title, sizeof( s_launch.log_title ), "%s", label );
    s_launch.log_valid = true;
    s_show_output      = true;

    if ( sys_process_spawn( command_line, working_dir ) )
        snprintf( s_launch.log, sizeof( s_launch.log ), "[launch] spawned: %s\n", command_line );
    else
        snprintf( s_launch.log, sizeof( s_launch.log ), "[launch] failed to spawn: %s\n", command_line );
    s_launch.last_exit_code = 0;
}

/*==============================================================================================
    Init -- resolve the engine root and probe the toolchain
==============================================================================================*/

void
launch_ui_init( void )
{
    snprintf( s_launch.engine_root, sizeof( s_launch.engine_root ), "%s", sys_root_dir() );
    s_launch.selected = -1;

    char probe[ LAUNCH_PATH_MAX + 32 ];
    snprintf( probe, sizeof( probe ), "%s/bin/build_tool.exe", s_launch.engine_root );
    s_launch.build_tool_present = ( sys_file_time( probe ) != 0 );

    launch_registry_load();
}

/*==============================================================================================
    Menu -- all actions live here; panes are just readouts
==============================================================================================*/

static void
launch_show_menu( gui_vp_t vp )
{
    if ( !gui()->main_menu_bar_begin() )
        return;

    char cmd[ LAUNCH_PATH_MAX * 2 ];

    if ( gui()->menu_begin( "Engine" ) )
    {
        if ( gui()->menu_item( "Run Doctor", NULL, NULL ) )
        {
            snprintf( cmd, sizeof( cmd ), "\"%s/bin/build_tool.exe\" -doctor", s_launch.engine_root );
            launch_run_capture( "build_tool -doctor", cmd, s_launch.engine_root );
        }
        if ( gui()->menu_item( "Launch Editor", NULL, NULL ) )
        {
            snprintf( cmd, sizeof( cmd ), "\"%s/bin/host_editor.exe\"", s_launch.engine_root );
            launch_spawn( "host_editor", cmd, s_launch.engine_root );
        }
        gui()->separator();
        if ( gui()->menu_item( "Exit", NULL, NULL ) )
            app()->window_request_close( ( i32 )vp );
        gui()->menu_end();
    }

    if ( gui()->menu_begin( "Projects" ) )
    {
        if ( gui()->menu_item( "Rescan", NULL, NULL ) )
            launch_registry_load();
        if ( gui()->menu_item( "Remove Missing", NULL, NULL ) )
            launch_registry_remove_missing();
        gui()->menu_end();
    }

    if ( gui()->menu_begin( "Windows" ) )
    {
        gui()->menu_item( "Engine",   NULL, &s_show_engine );
        gui()->menu_item( "Projects", NULL, &s_show_projects );
        gui()->menu_item( "Output",   NULL, &s_show_output );
        gui()->menu_end();
    }

    gui()->main_menu_bar_end();
}

/*==============================================================================================
    Panes
==============================================================================================*/

static void
launch_show_engine_pane()
{    
    i32 w, h; gui()->viewport_size( s_main_vp, &w, &h );
    f32 top = gui()->viewport_content_y( s_main_vp );
    f32 width = (f32)w / 2;
    f32 height = (f32)h / 2;

    gui()->window_set_next_pos ( 8, top + 8, GUI_COND_ONCE );
    gui()->window_set_next_size( width - 16, height - 8, GUI_COND_ONCE );
    if ( gui()->window_begin( "Engine", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "Root:        %s", s_launch.engine_root );
        gui()->textf( "build_tool:  %s", s_launch.build_tool_present ? "found" : "MISSING (run bootstrap_build_tool.bat)" );
    }
    gui()->window_end();
}

static void
launch_show_projects_pane()
{    
    i32 w, h; gui()->viewport_size( s_main_vp, &w, &h );
    f32 top = gui()->viewport_content_y( s_main_vp );
    f32 width = (f32)w / 2;
    f32 height = (f32)h / 2;

    gui()->window_set_next_pos ( width + 8, top + 8, GUI_COND_ONCE );
    gui()->window_set_next_size( width - 16, height - 8, GUI_COND_ONCE );
    /* Import row state: pasted path + last attempt's verdict. */
    static char        s_import_path[ LAUNCH_PATH_MAX ];
    static const char* s_import_status = NULL;

    if ( gui()->window_begin( "Projects", GUI_WIN_NONE ) )
    {
        gui()->stack();

        if ( s_launch.project_count == 0 )
        {
            gui()->text( "No projects registered." );
            gui()->text_wrapped( "Create one from the engine root -- it self-registers here:" );
            gui()->text( "    bin\\build_tool.exe -create <name> -type project" );
        }
        else
        {
            for ( u32 i = 0; i < s_launch.project_count; ++i )
            {
                launch_project_t* p   = &s_launch.projects[ i ];
                bool              sel = ( s_launch.selected == ( i32 )i );

                char label[ LAUNCH_PATH_MAX + 96 ];
                snprintf( label, sizeof( label ), "%s  --  %s%s",
                          p->name, p->path, p->present ? "" : "  (MISSING)" );

                gui()->push_id_int( ( i32 )i );
                if ( gui()->selectable( label, &sel ) )
                    s_launch.selected = sel ? ( i32 )i : -1;
                gui()->pop_id();
            }
        }

        /* Import: paste a project root (a dir holding orb.targets) and Add. */
        gui()->separator();
        gui()->input_text( "##import_path", s_import_path, sizeof( s_import_path ) );
        if ( gui()->button( "Add Project" ) )
        {
            bool ok         = launch_registry_add( s_import_path );
            s_import_status = ok ? "added" : "not a project (no orb.targets there)";
            if ( ok )
                s_import_path[ 0 ] = '\0';
        }
        if ( s_import_status )
            gui()->textf( "Import: %s", s_import_status );
    }
    gui()->window_end();
}

static void
launch_show_output_pane()
{
    i32 w, h; gui()->viewport_size( s_main_vp, &w, &h );
    f32 top = gui()->viewport_content_y( s_main_vp );
    f32 width = (f32)w / 2;
    f32 height = (f32)h / 2;

    gui()->window_set_next_pos ( 8, top + height + 8, GUI_COND_ONCE );
    gui()->window_set_next_size( width * 2 - 16, height - top - 16, GUI_COND_ONCE );
    if ( gui()->window_begin( "Output", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( !s_launch.log_valid )
        {
            gui()->text( "Command output appears here." );
        }
        else
        {
            gui()->textf( "$ %s    (exit %d)", s_launch.log_title, s_launch.last_exit_code );
            gui()->separator();

            /* Emit the captured buffer line by line; the window scrolls the overflow. */
            const char* p = s_launch.log;
            while ( *p )
            {
                const char* nl  = strchr( p, '\n' );
                int         len = nl ? ( int )( nl - p ) : ( int )strlen( p );
                if ( len > 0 && p[ len - 1 ] == '\r' ) --len;
                gui()->textf( "%.*s", len, p );
                if ( !nl ) break;
                p = nl + 1;
            }
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Frame
==============================================================================================*/

void
launch_ui_frame( gui_vp_t vp )
{
    /* Menu first (popup frame-ordering), then place panes below the viewport chrome --
       caption band (gui-shelled native window) + the menu bar just emitted. */
    launch_show_menu( vp );
    
    s_main_vp = vp;

    if ( s_show_engine )   launch_show_engine_pane();
    if ( s_show_projects ) launch_show_projects_pane();
    if ( s_show_output )   launch_show_output_pane();
}

/*============================================================================================*/
// clang-format on
