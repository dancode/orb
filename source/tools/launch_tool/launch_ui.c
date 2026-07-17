/*==============================================================================================

    tools/launch_tool/launch_ui.c -- launcher UI and command invocation.

    Three panes: Engine (root readout + actions), Projects (registry list -- empty until the
    registry lands), Output (captured stdout of the last synchronous command).  Every action
    is a spawned command line against the engine's bin/ tools; nothing is re-implemented.

==============================================================================================*/

// clang-format off

static launch_state_t s_launch;

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
}

/*==============================================================================================
    Panes
==============================================================================================*/

static void
launch_show_engine_pane( void )
{
    gui()->window_set_next_pos ( 20, 20, GUI_COND_ONCE );
    gui()->window_set_next_size( 640, 300, GUI_COND_ONCE );
    if ( gui()->window_begin( "Engine", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "Root:        %s", s_launch.engine_root );
        gui()->textf( "build_tool:  %s", s_launch.build_tool_present ? "found" : "MISSING (run bootstrap_build_tool.bat)" );
        gui()->separator();

        char cmd[ LAUNCH_PATH_MAX * 2 ];

        if ( gui()->button( "Run Doctor" ) )
        {
            snprintf( cmd, sizeof( cmd ), "\"%s/bin/build_tool.exe\" -doctor", s_launch.engine_root );
            launch_run_capture( "build_tool -doctor", cmd, s_launch.engine_root );
        }
        if ( gui()->button( "Launch Editor" ) )
        {
            snprintf( cmd, sizeof( cmd ), "\"%s/bin/host_editor.exe\"", s_launch.engine_root );
            launch_spawn( "host_editor", cmd, s_launch.engine_root );
        }
    }
    gui()->window_end();
}

static void
launch_show_projects_pane( void )
{
    gui()->window_set_next_pos ( 680, 20, GUI_COND_ONCE );
    gui()->window_set_next_size( 580, 300, GUI_COND_ONCE );
    if ( gui()->window_begin( "Projects", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( s_launch.project_count == 0 )
        {
            gui()->text( "No projects registered." );
            gui()->separator();
            gui()->text_wrapped( "The project registry (machine-local list of created/imported "
                                 "projects) is the next step -- see the launcher design notes. "
                                 "Until then, create projects from the command line:" );
            gui()->text( "    bin\\build_tool.exe -create <name> -type project" );
        }
        else
        {
            for ( u32 i = 0; i < s_launch.project_count; ++i )
            {
                launch_project_t* p = &s_launch.projects[ i ];
                gui()->textf( "%s  --  %s%s", p->name, p->path, p->present ? "" : "  (missing)" );
            }
        }
    }
    gui()->window_end();
}

static void
launch_show_output_pane( void )
{
    gui()->window_set_next_pos ( 20, 340, GUI_COND_ONCE );
    gui()->window_set_next_size( 1240, 360, GUI_COND_ONCE );
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
    UNUSED( vp );
    launch_show_engine_pane();
    launch_show_projects_pane();
    launch_show_output_pane();
}

/*============================================================================================*/
// clang-format on
