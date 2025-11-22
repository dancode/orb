/*==============================================================================================

    engine_main.c

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"

#if PLATFORM_WINDOWS
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <unistd.h>
#    include <libgen.h>
#endif

#include "core/core.h"
#include "core/module_system.h"

/*============================================================================================*/

int  intern_test( void );                        // ... temporary code ....
void reflection_test( void );                    // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/

static void
test( int argc, char** argv )
{
    core_api_init();    // <-- required to debug natvis and must go first

    mem_test();    // <-- test memory system

    intern_test();        // <-- test string interning system
    reflection_test();    // <-- test reflection system


    /**************************************************************/

    cvar_system_init();
    test_core_cvar( argc, argv );    // <-- test cvar system
    cvar_system_exit();

    /**************************************************************/

    core_api_exit();
}

/*============================================================================================*/

static void
main_set_module_base_path()
{
#if PLATFORM_WINDOWS

    module_set_base_path( "" );

#else    // PLATFORM_LINUX

    char exe_path[ 256 ];
    readlink( "/proc/self/exe", exe_path, sizeof( exe_path ) );
    char* last_slash = strrchr( exe_path, '/' );
    if ( last_slash )
    {
        *last_slash = '\0';
    }
    char base_path[ 256 ];
    snprintf( base_path, sizeof( base_path ), "%s/../lib/", exe_path );
    module_set_base_path( base_path );

#endif
}

/*============================================================================================*/

int
main( int argc, char** argv )
{
    test( argc, argv );

    core_init();

    /**************************************************************/
    /* setup module base path -- different per platform */
    /* this setup code is designed to allow Google jules to find library files */
    
    main_set_module_base_path();

    // sid_t game_module_name = sid_intern_cstr( "sample_game" );
    // ( void )game_module_name;

    const char* mod_name = "sample_game";
    char        path[ 256 ];
    
    snprintf( path, sizeof( path ), "%s%s%s%s", module_get_base_path(), LIB_PREFIX, mod_name, LIB_EXT );

    /**************************************************************/
    /* new module laoding code path */



    /**************************************************************/

    struct module_t* game = module_load( mod_name, path );
    if ( game == NULL )
    {
        return 1;
    }

    for ( int i = 0; i < 3; i++ )
    {
        module_call_tick( game );
    }

    module_reload( game );

    for ( int i = 0; i < 2; i++ )
    {
        module_call_tick( game );
    }

    module_unload( game );

    core_exit();
    return 0;
}

/*============================================================================================*/