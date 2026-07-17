/*==============================================================================================

    tools/ship_tool/ship_tool.c -- Ship-pipeline batch tool.

    Thin CLI front-end over the dev_ship library (developer/dev_ship): runs the
    build -> cook -> stage -> package -> deploy pipeline that turns a project into a
    standalone, non-development release layout.  This is the batch-job heart of shipping --
    the editor's Deploy window spawns this exe with the same arguments a build script
    would use.

    Usage:
        ship_tool <project> [-config <Debug|Release>] [-out <dir>] [-modular]
                            [-deploy <dir>] [-only <build|cook|stage|package|deploy>]
                            [-skip-build] [-pdb] [-clean]

        <project>     project name; the monolithic ship exe target is <project>_ship
                      (see the PATTERN block in orb.targets)
        -config       build configuration (default: Release)
        -out          staging root (default: build/ship/<project>)
        -modular      ship host_game.exe + module DLLs instead of the monolithic
                      single exe (the "final correct" default)
        -deploy       destination directory to mirror the staged build into; without
                      it the deploy stage is a no-op (the staged dir is the deliverable)
        -only         run a single pipeline stage instead of the full sequence
        -skip-build   stage prebuilt bin/ output; do not invoke build_tool
        -pdb          include .pdb files in the staged layout
        -clean        delete staged files before staging

    Run from the engine root (the directory holding orb.targets).

    Link deps: dev_ship (pipeline), sys (validation, process run inside dev_ship)

==============================================================================================*/
// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "developer/dev_ship/dev_ship.h"

/*==============================================================================================
    Usage
==============================================================================================*/

static int
usage( void )
{
    fprintf( stderr,
             "usage: ship_tool <project> [-config <Debug|Release>] [-out <dir>] [-modular]\n"
             "                 [-deploy <dir>] [-only <build|cook|stage|package|deploy>]\n"
             "                 [-skip-build] [-pdb] [-clean]\n" );
    return 1;
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    if ( argc < 2 )
        return usage();

    dev_ship_desc_t desc =
    {
        .project  = argv[ 1 ],
        .config   = "Release",
        .root_dir = ".",        /* run from the engine root; dev_ship validates */
        .out_dir  = NULL,
    };

    dev_ship_stage_t only      = DEV_SHIP_STAGE_COUNT;    /* COUNT = run the full pipeline */
    bool             have_only = false;

    for ( int i = 2; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-config" ) == 0 && i + 1 < argc )
            desc.config = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-out" ) == 0 && i + 1 < argc )
            desc.out_dir = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-deploy" ) == 0 && i + 1 < argc )
            desc.deploy_dir = argv[ ++i ];
        else if ( strcmp( argv[ i ], "-only" ) == 0 && i + 1 < argc )
        {
            const char* name = argv[ ++i ];
            for ( int s = 0; s < DEV_SHIP_STAGE_COUNT; ++s )
                if ( strcmp( name, dev_ship_stage_name( (dev_ship_stage_t)s ) ) == 0 )
                {
                    only      = (dev_ship_stage_t)s;
                    have_only = true;
                    break;
                }
            if ( !have_only )
            {
                fprintf( stderr, "ship_tool: unknown stage '%s'\n", name );
                return usage();
            }
        }
        else if ( strcmp( argv[ i ], "-modular" ) == 0 )
            desc.flags |= DEV_SHIP_MODULAR;
        else if ( strcmp( argv[ i ], "-skip-build" ) == 0 )
            desc.flags |= DEV_SHIP_SKIP_BUILD;
        else if ( strcmp( argv[ i ], "-pdb" ) == 0 )
            desc.flags |= DEV_SHIP_PDB;
        else if ( strcmp( argv[ i ], "-clean" ) == 0 )
            desc.flags |= DEV_SHIP_CLEAN;
        else
        {
            fprintf( stderr, "ship_tool: unknown option '%s'\n", argv[ i ] );
            return usage();
        }
    }

    printf( "ship_tool: %s (%s)%s%s\n", desc.project, desc.config,
            have_only ? " stage: " : "",
            have_only ? dev_ship_stage_name( only ) : "" );

    bool ok = have_only ? dev_ship_run_stage( &desc, only )
                        : dev_ship_run( &desc );
    if ( !ok )
    {
        fprintf( stderr, "ship_tool: %s\n", dev_ship_last_error() );
        return 1;
    }

    printf( "ship_tool: done\n" );
    return 0;
}
