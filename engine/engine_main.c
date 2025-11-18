/*==============================================================================================

    engine_main.c

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#if PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif

#include "orb.h"
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

int
main( int argc, char** argv )
{
    test( argc, argv );

    core_init();

    const char* mod_name = "sample_game";
    char path[256];

#if PLATFORM_WINDOWS
    set_module_base_path("");
#else
    char exe_path[256];
    readlink("/proc/self/exe", exe_path, sizeof(exe_path));
    char* last_slash = strrchr(exe_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    }
    char base_path[256];
    snprintf(base_path, sizeof(base_path), "%s/../lib/", exe_path);
    set_module_base_path(base_path);
#endif

    snprintf(path, sizeof(path), "%s%s%s%s", get_module_base_path(), LIB_PREFIX, mod_name, LIB_EXT);

    struct module_t* game = module_load(mod_name, path);
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