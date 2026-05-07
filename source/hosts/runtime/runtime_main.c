/*==============================================================================================

    runtime_main.c

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/mod/mod_api.h"

#include "runtime_modules/example/example_api.h"

#define SYS_STATIC
#define CORE_STATIC


// TODO: define these in build step if its an exe-linked TU
struct mod_api_s* sys_get_mod_api( void );
struct mod_api_s* core_get_mod_api( void );

MOD_DEFINE_API_PTR( example_api_t, example );

/*============================================================================================*/
int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    
    mod_system_init();
    mod_static_load( "sys", sys_get_mod_api() );
    mod_static_load( "core", core_get_mod_api());
    if ( !mod_load( example ) )
    {
        return 1;
    }

    mod_system_exit();

    return 0;
}

/*============================================================================================*/