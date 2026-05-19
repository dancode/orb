/*==============================================================================================

    tool_main.c : Short-lived CLI dispatcher

    TODO: An incomplete working prototype, but the idea is:

    Context (specific to this host)
    -------------------------------

        - parsed command-line flags         (which tool to run, input/output paths)
        - headless engine state             (no window, no GPU, no audio device)
        - exit code                         (tool result reported back to the shell)

    Behavior
    --------

        - Loads ONLY the modules a particular tool needs.
          reflection_gen needs nothing but core;
          asset_bake might need render (for texture compression).
        - Returns the tool's exit code from main().

    Usage examples
    --------------

        tool reflection_gen   foo.h
        tool asset_bake       --in assets/ --out built/
        tool list_modules

==============================================================================================*/

#include <stdio.h>    // printf, fprintf
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod.h"
#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"

/*==============================================================================================
    Parsed CLI context
==============================================================================================*/

typedef struct tool_ctx_s
{
    const char* tool_name; /* e.g. "reflection_gen", "asset_bake" */
    int         argc;
    char**      argv;
    int         exit_code;

} tool_ctx_t;

/*==============================================================================================
    Tool dispatchers
    Each is a thin function that loads only the modules the tool actually needs,
    runs the tool, and returns an exit code.
==============================================================================================*/

static int
run_reflection_gen( tool_ctx_t* ctx )
{
    UNUSED( ctx );
    /* reflection_gen needs only core — no render, no audio, no physics. */
    if ( !mod_init_all() )
        return 1;

    core()->log( "reflection_gen: would parse %s and emit reflection tables",
                     ctx->argc > 2 ? ctx->argv[ 2 ] : "<missing input>" );
    return 0;
}

static int
run_asset_bake( tool_ctx_t* ctx )
{
    UNUSED( ctx );
    /* asset_bake needs render for texture compression utilities. */
    if ( !mod_load( render ) )
        return 1;
    if ( !mod_init_all() )
        return 1;

    // if ( !MOD_HOST_FETCH_API( render_api_t, render ) )
    //    return 1;
    //
    // core()->log( "asset_bake: starting (target frames-rendered API works: %d)", render()->frame_count() );
    // 
    // /* Pretend we baked a few assets — drive the API once to prove it's wired. */
    // render()->begin_frame();
    // render()->draw( 0.f );
    // render()->end_frame();
    // 
    // core()->log( "asset_bake: done (%d 'frames' processed)", render()->frame_count() );
    UNUSED( ctx );
    return 0;
}

static int
run_list_modules( tool_ctx_t* ctx )
{
    UNUSED( ctx );
    if ( !mod_init_all() )
        return 1;
    mod_list_all();
    return 0;
}

/*============================================================================================*/

const char* reflection_gen[] = { "", "reflection_gen" };
const char* asset_bakep[]    = { "", "asset_bake" };
const char* list_modulesp[]  = { "", "list_modules" };

int
main( int argc, char** argv )
{
    if ( argc == 1 ) 
    {
        // default to list_modules if no args given, for easy testing
        argc = 2;
        argv = ( char** )list_modulesp;
    }
    if ( argc < 2 )
    {
        fprintf( stderr, "usage: tool <reflection_gen|asset_bake|list_modules> [args]\n" );
        return 2;
    }

    UNUSED( argc );
    UNUSED( argv );

    puts( "=== tool_host ===" );

    tool_ctx_t ctx = {
        .tool_name = argv[ 1 ],
        .argc      = argc,
        .argv      = argv,
        .exit_code = 0,
    };

    mod_system_init();
    if ( !mod_static_load( "core", core_get_mod_desc() ) )
        return 1;

    /* Dispatch — each tool function loads its own additional modules. */

    if ( strcmp( ctx.tool_name, "reflection_gen" ) == 0 )
        ctx.exit_code = run_reflection_gen( &ctx );
    else if ( strcmp( ctx.tool_name, "asset_bake" ) == 0 )
        ctx.exit_code = run_asset_bake( &ctx );
    else if ( strcmp( ctx.tool_name, "list_modules" ) == 0 )
        ctx.exit_code = run_list_modules( &ctx );
    else
    {
        fprintf( stderr, "tool: unknown tool '%s'\n", ctx.tool_name );
        ctx.exit_code = 2;
    }

    mod_system_exit();
    return 0;
}

/*============================================================================================*/