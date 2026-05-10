/*==============================================================================================

    host/common/host_common.c : common code shared by multiple host executables (game, sandbox, etc).

==============================================================================================*/
static int host_common_var = 0;

#include "host_common.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
    #include <windows.h>
#endif

void
host_common_early_setup( void )
{
#ifdef _WIN32
    // Ensure the Windows console prints UTF-8 characters correctly
    SetConsoleOutputCP( CP_UTF8 );
    // You would also attach your SEH (Structured Exception Handling) crash dumper here
#endif
}

void
host_common_parse_args( int argc, char** argv, runtime_config_t* out_config )
{
    // 1. Hardcode absolute fallbacks (if someone just double clicks the .exe)
    out_config->project_name  = "Amberfall";
    out_config->project_dll   = "amberfall";
    out_config->window_width  = 1280;
    out_config->window_height = 720;

    // 2. Parse the command line arguments
    for ( int i = 1; i < argc; i++ )
    {
        // Target Module Pivot (For Sandboxes)
        if ( strcmp( argv[ i ], "-module" ) == 0 && i + 1 < argc )
        {
            out_config->project_dll  = argv[ i + 1 ];
            out_config->project_name = argv[ i + 1 ];    // Use module name as title
            i++;
        }
        // Resolution Overrides
        else if ( strcmp( argv[ i ], "-w" ) == 0 && i + 1 < argc )
        {
            out_config->window_width = atoi( argv[ i + 1 ] );
            i++;
        }
        else if ( strcmp( argv[ i ], "-h" ) == 0 && i + 1 < argc )
        {
            out_config->window_height = atoi( argv[ i + 1 ] );
            i++;
        }
    }
}


/*============================================================================================*/