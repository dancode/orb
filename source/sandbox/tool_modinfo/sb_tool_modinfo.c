/*==============================================================================================

    sandbox/tool_modinfo/sb_tool_modinfo.c — Module descriptor inspector.

    A standalone tool: one-shot, no main loop, no runtime_host, no module
    system, no core, no app. It validates the third host shape — a tool that
    uses only the substrate (sys + host_common) and does one focused thing.

    What it does
    ------------
    Given `-module <name>`, computes <exe_dir>/<name>.dll, loads it directly
    via sys_library_load, resolves the get_mod_desc export, and prints every
    field of the returned mod_desc_t descriptor. Then unloads and exits.

    What it proves architecturally
    ------------------------------
    - tools do not need the runtime loop
    - tools do not need mod_system_init()
    - sys is genuinely usable by itself
    - host_common is shape-agnostic (works for runtime hosts, sandbox hosts,
      and tools alike)

    If any of these were untrue, this tool could not be written cleanly.

    Usage
    -----
        sandbox_tool_modinfo -module example
        sandbox_tool_modinfo -module render

    Only meaningful in dynamic builds (modules exist as DLLs). In monolithic
    builds there are no .dll files to inspect.

==============================================================================================*/
#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/mod/mod_export.h" /* mod_desc_t — type only; no symbols pulled in */

#include "host/common/host_common.h"

/*==============================================================================================
    Constants
==============================================================================================*/

#define TOOL_PATH_MAX 260

/*==============================================================================================
    Helpers
==============================================================================================*/

static void
print_usage( const char* argv0 )
{
    fprintf( stderr, "Usage: %s -module <name>\n", argv0 );
    fprintf( stderr, "\n" );
    fprintf( stderr, "  Loads <exe_dir>/<name>.dll and prints its mod_desc_t descriptor.\n" );
    fprintf( stderr, "  Examples:\n" );
    fprintf( stderr, "    %s -module example\n", argv0 );
    fprintf( stderr, "    %s -module render\n", argv0 );
}

static void
print_descriptor( const char* mod_name, const mod_desc_t* api )
{
    const int slot_count = api->func_api_size / ( int )sizeof( void* );

    int       populated  = 0;
    if ( api->func_api && slot_count > 0 )
    {
        const void* const* slots = ( const void* const* )api->func_api;
        for ( int i = 0; i < slot_count; ++i )
            if ( slots[ i ] )
                ++populated;
    }

    printf( "\n" );
    printf( "  module name      : %s\n", mod_name );
    printf( "  api version      : %d\n", api->version );
    printf( "  state size       : %d bytes%s\n", api->state_size, api->state_size == 0 ? "  (stateless)" : "" );
    printf( "  func_api_size    : %d bytes  (%d function pointer slots)\n", api->func_api_size, slot_count );
    printf( "  func_api slots   : %d / %d populated\n", populated, slot_count );
    printf( "  init()           : %s\n", api->init ? "present" : "(none)" );
    printf( "  exit()           : %s\n", api->exit ? "present" : "(none)" );
    printf( "  reload()         : %s\n", api->reload ? "present" : "(none)  -- not hot-reloadable" );
    printf( "  dep_count        : %d\n", api->dep_count );

    if ( api->dep_count > 0 )
    {
        printf( "  deps             :\n" );
        for ( int i = 0; i < api->dep_count; ++i )
            printf( "                     [%d] %s\n", i, api->deps[ i ] ? api->deps[ i ] : "(null)" );
    }
    printf( "\n" );
}

/*==============================================================================================
    Entry point
==============================================================================================*/

int
main( int argc, char** argv )
{
    if ( argc == 1 )
    {
        argc = 3;
        argv = ( char*[] ){ argv[ 0 ], "-module", "example" };
    }

    launch_params_t params;
    host_args_parse( argc, argv, &params );

    if ( params.module_override[ 0 ] == '\0' )
    {
        print_usage( argv[ 0 ] );
        return 1;
    }

    const char* mod_name = params.module_override;

    /*--------------------------------------------------
        Compute <exe_dir>/<name>.dll
    --------------------------------------------------*/

    char exe_dir[ TOOL_PATH_MAX ];
    sys_exe_dir( exe_dir, ( int )sizeof( exe_dir ) );

    char dll_path[ TOOL_PATH_MAX ];
    snprintf( dll_path, sizeof( dll_path ), "%s/%s.dll", exe_dir, mod_name );

    printf( "[modinfo] inspecting: %s\n", dll_path );

    /*--------------------------------------------------
        Load the DLL and resolve get_mod_desc
    --------------------------------------------------*/

    lib_handle_t lib = sys_library_load( dll_path );
    if ( !lib )
    {
        fprintf( stderr, "[modinfo] error: failed to load DLL (does it exist? wrong arch?)\n" );
        return 1;
    }

    typedef mod_desc_t* ( *get_mod_desc_fn )( void );
    get_mod_desc_fn get_mod_desc = ( get_mod_desc_fn )sys_library_get_symbol( lib, "get_mod_desc" );
    if ( !get_mod_desc )
    {
        fprintf( stderr, "[modinfo] error: '%s' is missing the 'get_mod_desc' export\n", mod_name );
        sys_library_unload( lib );
        return 1;
    }

    mod_desc_t* api = get_mod_desc();
    if ( !api )
    {
        fprintf( stderr, "[modinfo] error: get_mod_desc() returned NULL\n" );
        sys_library_unload( lib );
        return 1;
    }

    /*--------------------------------------------------
        Dump the descriptor
    --------------------------------------------------*/

    print_descriptor( mod_name, api );

    /*--------------------------------------------------
        Clean shutdown
    --------------------------------------------------*/

    sys_library_unload( lib );
    return 0;
}

/*============================================================================================*/