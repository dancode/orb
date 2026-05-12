/*==============================================================================================

    host/common/host_common.c : Pre-runtime host setup implementation.

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "host/common/host_common.h"

/*==============================================================================================
    API
==============================================================================================*/

void
host_args_parse( int argc, char** argv, launch_params_t* out )
{
    memset( out, 0, sizeof( *out ) );

    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-module" ) == 0 && i + 1 < argc )
        {
            snprintf( out->module_override, sizeof( out->module_override ), "%s", argv[ ++i ] );
        }
        else if ( strcmp( argv[ i ], "-project" ) == 0 && i + 1 < argc )
        {
            snprintf( out->project_path, sizeof( out->project_path ), "%s", argv[ ++i ] );
        }
        else if ( strcmp( argv[ i ], "-dev" ) == 0 )
        {
            out->dev_mode = true;
        }
    }
}

/*============================================================================================*/