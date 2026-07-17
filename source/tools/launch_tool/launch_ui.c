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

#define LAUNCH_CREATE_POPUP "Create Project"   /* shared id: popup_open + popup_modal_begin */

/* Deferred open request: popup_open records at the CURRENT popup nesting depth, so calling it
   from inside a menu dropdown (itself a popup) would nest the modal under the menu -- which then
   closes and takes the modal with it.  The menu item sets this flag; the frame opens the popup at
   top level once the menu bar has ended. */
static bool s_req_create_popup = false;

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
    Per-project actions -- all against the selected project's path
==============================================================================================*/

/* Run the engine's build_tool inside a project directory: build_tool reads the CWD's
   orb.targets, so the working dir IS the project selector.  Captured (fast, want the log). */
static void
launch_project_build_tool( launch_project_t* prj, const char* label, const char* args )
{
    char cmd[ LAUNCH_PATH_MAX * 2 ];
    snprintf( cmd, sizeof( cmd ), "\"%s/bin/build_tool.exe\" %s", s_launch.engine_root, args );

    char full_label[ 160 ];
    snprintf( full_label, sizeof( full_label ), "%s: %s", prj->name, label );
    launch_run_capture( full_label, cmd, prj->path );
}

/* Launch an engine host on a project: host <exe> -project "<abs path>".  Spawned (long-lived,
   owns its own window); the host resolves assets relative to its own exe, so the engine root
   is the working dir.  Guarded on the project DLL: a host given an unbuilt project loads no
   game module and exits at once (a console flash), so we report "Build first" instead. */
static void
launch_project_host( launch_project_t* prj, const char* host_exe, const char* label )
{
    char dll[ LAUNCH_PATH_MAX + 96 ];
    snprintf( dll, sizeof( dll ), "%s/bin/%s.dll", prj->path, prj->name );
    if ( !sys_file_exists( dll ) )
    {
        snprintf( s_launch.log_title, sizeof( s_launch.log_title ), "%s: %s", prj->name, label );
        snprintf( s_launch.log, sizeof( s_launch.log ),
                  "[launch] %s.dll not built yet -- run Build first, then %s.\n", prj->name, label );
        s_launch.log_valid      = true;
        s_launch.last_exit_code = 0;
        s_show_output           = true;
        return;
    }

    char cmd[ LAUNCH_PATH_MAX * 2 ];
    snprintf( cmd, sizeof( cmd ), "\"%s/bin/%s.exe\" -project \"%s\"",
              s_launch.engine_root, host_exe, prj->path );

    char full_label[ 160 ];
    snprintf( full_label, sizeof( full_label ), "%s: %s", prj->name, label );
    launch_spawn( full_label, cmd, s_launch.engine_root );
}

/* Open the project folder in the OS file browser (Windows Explorer). */
static void
launch_project_open_folder( launch_project_t* prj )
{
    char cmd[ LAUNCH_PATH_MAX + 32 ];
    snprintf( cmd, sizeof( cmd ), "explorer \"%s\"", prj->path );
    launch_spawn( "open folder", cmd, prj->path );
}

/*==============================================================================================
    Create Project -- wraps build_tool -create <name> -type project
==============================================================================================*/

/* Client-side twin of build_tool's create_valid_name: a project name is a C identifier
   (letters, digits, underscore; not starting with a digit).  Pre-validating gates the Create
   button so an obviously-bad name never reaches a spawn; build_tool re-checks authoritatively. */
static bool
launch_name_valid( const char* s )
{
    if ( !s || !s[ 0 ] )                     return false;
    if ( s[ 0 ] >= '0' && s[ 0 ] <= '9' )    return false;
    for ( const char* p = s; *p; ++p )
    {
        bool ok = ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ||
                  ( *p >= '0' && *p <= '9' ) || ( *p == '_' );
        if ( !ok ) return false;
    }
    return true;
}

/* Build the create command into `out` (same string the dialog previews and runs), so the
   preview can never drift from what actually executes.  -dir is omitted when `dir` is empty:
   build_tool then defaults the project directory to the name, relative to the engine root. */
static void
launch_create_command( char* out, u32 out_sz, const char* name, const char* dir )
{
    int n = snprintf( out, out_sz, "\"%s/bin/build_tool.exe\" -create %s -type project",
                      s_launch.engine_root, name );
    if ( n > 0 && ( u32 )n < out_sz && dir && dir[ 0 ] )
        snprintf( out + n, out_sz - ( u32 )n, " -dir \"%s\"", dir );
}

/* Run the create (from the engine root -- cmd_create_project requires it) and, on success,
   reload the registry (build_tool self-registered the project) and select the new entry. */
static void
launch_run_create( const char* name, const char* dir )
{
    char cmd[ LAUNCH_PATH_MAX * 2 ];
    launch_create_command( cmd, sizeof( cmd ), name, dir );

    char label[ 128 ];
    snprintf( label, sizeof( label ), "create project %s", name );
    launch_run_capture( label, cmd, s_launch.engine_root );

    if ( s_launch.last_exit_code != 0 )
        return;

    launch_registry_load();
    for ( u32 i = 0; i < s_launch.project_count; ++i )
        if ( strcmp( s_launch.projects[ i ].name, name ) == 0 )
            s_launch.selected = ( i32 )i;
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
        if ( gui()->menu_item( "New Project...", NULL, NULL ) )
            s_req_create_popup = true;    /* opened at top level after the menu bar (see frame) */
        gui()->separator();
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

        /* Actions for the selected project.  Present-guarded: a missing project can still be
           opened in a file browser (to find where it went) but cannot be built or run. */
        if ( s_launch.selected >= 0 && s_launch.selected < ( i32 )s_launch.project_count )
        {
            launch_project_t* prj = &s_launch.projects[ s_launch.selected ];

            gui()->separator();
            gui()->textf( "Actions -- %s", prj->name );

            if ( !prj->present )
            {
                gui()->text( "(project folder is missing)" );
                if ( gui()->button( "Open Folder" ) )
                    launch_project_open_folder( prj );
            }
            else
            {
                if ( gui()->button( "Open in Editor" ) )
                    launch_project_host( prj, "host_editor", "editor" );
                gui()->same_line( 0.0f );
                if ( gui()->button( "Play" ) )
                    launch_project_host( prj, "host_game", "play" );

                if ( gui()->button( "Build" ) )
                    launch_project_build_tool( prj, "build", "-config Debug" );
                gui()->same_line( 0.0f );
                if ( gui()->button( "Generate" ) )
                    launch_project_build_tool( prj, "gen", "-gen" );
                gui()->same_line( 0.0f );
                if ( gui()->button( "Doctor" ) )
                    launch_project_build_tool( prj, "doctor", "-doctor" );

                if ( gui()->button( "Open Folder" ) )
                    launch_project_open_folder( prj );
            }
        }

        /* Import: paste a project root (a dir holding orb.targets) and Add. */
        gui()->separator();
        gui()->text_wrapped( "Add an existing project by its root folder\n(the one holding orb.targets):" );    
        // gui()->text( "Add an existing project by its root folder\n(the one holding orb.targets):" );
        gui()->text( "    e.g.  F:\\games\\my_project" );
        gui()->input_text_with_hint( "##import_path", "project folder path...",
                                     s_import_path, sizeof( s_import_path ) );
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
    Create Project dialog -- modal; emitted every frame, visible only while open
==============================================================================================*/

static void
launch_show_create_dialog( void )
{
    static char name[ 64 ];
    static char dir [ LAUNCH_PATH_MAX ];

    if ( !gui()->popup_modal_begin( LAUNCH_CREATE_POPUP, "Create Project", GUI_WIN_NONE ) )
        return;

    gui()->stack();
    gui()->text( "A standalone project built on this engine (its own orb.targets)." );
    gui()->separator();

    gui()->field_label_left( 90.0f );
    gui()->input_text( "Name", name, sizeof( name ) );
    gui()->input_text_with_hint( "Directory", "defaults to the name", dir, sizeof( dir ) );

    const bool valid = launch_name_valid( name );
    if ( name[ 0 ] && !valid )
        gui()->text( "Name must be letters, digits, or underscore; not start with a digit." );

    /* Preview the exact command that will run -- the launcher wraps the CLI, it does not hide it. */
    gui()->separator();
    if ( valid )
    {
        char preview[ LAUNCH_PATH_MAX * 2 ];
        launch_create_command( preview, sizeof( preview ), name, dir );
        gui()->text_wrapped( preview );
    }
    else
    {
        gui()->text( "Enter a valid project name to continue." );
    }
    gui()->separator();

    gui()->disabled_begin( !valid );
    if ( gui()->button( "Create" ) )
    {
        launch_run_create( name, dir );
        if ( s_launch.last_exit_code == 0 )
        {
            name[ 0 ] = '\0';
            dir [ 0 ] = '\0';
            gui()->popup_close_current();
        }
        /* On failure the dialog stays open; the Output pane shows why. */
    }
    gui()->disabled_end();

    gui()->same_line( 0.0f );
    if ( gui()->button( "Cancel" ) )
    {
        name[ 0 ] = '\0';
        dir [ 0 ] = '\0';
        gui()->popup_close_current();
    }

    gui()->popup_end();
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

    /* Honor a deferred create request at top-level depth (0), now the menu bar has ended --
       so the modal registers as a root popup, not nested under the just-closed menu. */
    if ( s_req_create_popup )
    {
        gui()->popup_open( LAUNCH_CREATE_POPUP );
        s_req_create_popup = false;
    }

    s_main_vp = vp;

    if ( s_show_engine )   launch_show_engine_pane();
    if ( s_show_projects ) launch_show_projects_pane();
    if ( s_show_output )   launch_show_output_pane();

    /* Modal: emitted unconditionally, self-gated on its open state (menu opens it). */
    launch_show_create_dialog();
}

/*============================================================================================*/
// clang-format on
