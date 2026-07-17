#ifndef LAUNCH_TOOL_H
#define LAUNCH_TOOL_H
/*==============================================================================================

    tools/launch_tool/launch_tool.h -- ORB launcher types and shared state.

    The launcher is the engine's project manager / command hub: the one gui()-driven tool.
    It lists projects, invokes the command-line tools (build_tool, ship_tool, hosts) and
    shows their output.  It never re-implements tool logic -- every action spawns a command
    line, exactly what a user would type by hand.

==============================================================================================*/

#include "orb.h"
#include "runtime_service/gui/gui.h"

/*==============================================================================================
    Limits
==============================================================================================*/

#define LAUNCH_MAX_PROJECTS 64
#define LAUNCH_PATH_MAX     260
#define LAUNCH_LOG_SIZE     ( 64 * 1024 )

/*==============================================================================================
    Types
==============================================================================================*/

/* One known project: a directory whose orb.targets declares 'engine'. */
typedef struct launch_project_s
{
    char name[ 64 ];                 // display name (directory basename)
    char path[ LAUNCH_PATH_MAX ];    // absolute project root
    bool present;                    // path existed on disk at last scan

} launch_project_t;

typedef struct launch_state_s
{
    char             engine_root[ LAUNCH_PATH_MAX ];    // sys_root_dir, resolved once at boot
    bool             build_tool_present;                // <root>/bin/build_tool.exe exists

    launch_project_t projects[ LAUNCH_MAX_PROJECTS ];
    u32              project_count;
    i32              selected;                          // index into projects; -1 = the engine itself

    char             log[ LAUNCH_LOG_SIZE ];            // captured output of the last command
    char             log_title[ 128 ];                  // what produced the log (command label)
    bool             log_valid;
    int              last_exit_code;

} launch_state_t;

/*==============================================================================================
    Units (implemented in launch_ui.c)
==============================================================================================*/

void launch_ui_init( void );
void launch_ui_frame( gui_vp_t vp );

/*============================================================================================*/
#endif    // LAUNCH_TOOL_H
