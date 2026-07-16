/*==============================================================================================

    tool_main.c : Short-lived CLI dispatcher

    Context (specific to this host)
    -------------------------------

        - parsed command-line flags         (which tool to run, input/output paths)
        - headless engine state             (no window, no GPU, no audio device)
        - exit code                         (tool result reported back to the shell)

    Behavior
    --------

        - Boots through run_host_main headless (RUN_LOOP_NONE): the engine floor loads,
          then the caller dispatches its subcommand and shuts down explicitly.
        - Loads ONLY the modules a particular tool needs.
          reflection_gen needs nothing but the engine floor;
          asset_bake also needs render (for texture compression).
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
#define LOG_CH "tool"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"
#include "runtime_modules/render/render_api.h"

#include "runtime/run_host.h"

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

    Each runs AFTER run_host_main has already loaded and inited whatever modules the
    dispatch table below decided it needs -- no mod_load/mod_init_all calls here.
==============================================================================================*/

static int
run_reflection_gen( tool_ctx_t* ctx )
{
    LOG_INFO( "reflection_gen: would parse %s and emit reflection tables",
              ctx->argc > 2 ? ctx->argv[ 2 ] : "<missing input>" );
    return 0;
}

static int
run_asset_bake( tool_ctx_t* ctx )
{
    UNUSED( ctx );
    /* asset_bake needs render for texture compression utilities (added to the module
       list below).  Wiring the actual bake work is a follow-up -- this proves the boot
       path with render live. */
    return 0;
}

static int
run_list_modules( tool_ctx_t* ctx )
{
    UNUSED( ctx );
    mod_list_all();
    return 0;
}

/*============================================================================================*/

const char* reflection_genp[] = { "", "reflection_gen" };
const char* asset_bakep[]     = { "", "asset_bake" };
const char* list_modulesp[]   = { "", "list_modules" };

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

    puts( "=== tool_host ===" );

    tool_ctx_t ctx = {
        .tool_name = argv[ 1 ],
        .argc      = argc,
        .argv      = argv,
        .exit_code = 0,
    };

    bool unknown = strcmp( ctx.tool_name, "reflection_gen" ) != 0 &&
                   strcmp( ctx.tool_name, "asset_bake"     ) != 0 &&
                   strcmp( ctx.tool_name, "list_modules"   ) != 0;
    if ( unknown )
    {
        fprintf( stderr, "tool: unknown tool '%s'\n", ctx.tool_name );
        return 2;
    }

    /* Module list is decided by which subcommand was asked for -- reflection_gen and
       list_modules need nothing past the engine floor; asset_bake also needs render. */
    static const run_module_entry_t k_modules_render[] = { RUN_MODULE( render ), { 0 } };
    static const run_module_entry_t k_modules_none[]   = { { 0 } };

    const bool needs_render = strcmp( ctx.tool_name, "asset_bake" ) == 0;

    const run_host_desc_t desc = {
        .name      = "host_tool",
        .flags     = 0,               /* headless, one-shot -- no window, no hot-reload */
        .loop_mode = RUN_LOOP_NONE,   /* boot, return, caller dispatches, then shuts down */
        .modules   = needs_render ? k_modules_render : k_modules_none,
    };

    if ( run_host_main( &desc, argc, argv ) != 0 )
        return 1;

    if ( strcmp( ctx.tool_name, "reflection_gen" ) == 0 )
        ctx.exit_code = run_reflection_gen( &ctx );
    else if ( strcmp( ctx.tool_name, "asset_bake" ) == 0 )
        ctx.exit_code = run_asset_bake( &ctx );
    else /* "list_modules" */
        ctx.exit_code = run_list_modules( &ctx );

    run_host_shutdown();
    return ctx.exit_code;
}

/*============================================================================================*/
