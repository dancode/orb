#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod_host.h"


#include "runtime_modules/example_gen/example_gen.h"
MOD_DEFINE_API_PTR( example_gen_api_t, example_gen );


/*============================================================================================*/
int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    mod_system_init();

    // dev_hot_init( NULL, NULL );

    if ( !mod_static_load( "sys", sys_get_mod_desc() ) )
        goto shutdown;

    if ( !mod_load( example_gen ) )
    {
        fprintf( stderr, "load example_gen: %s\n", mod_last_error() );
        goto shutdown;
    }

    if ( mod_init_all() == false )
    {
        fprintf( stderr, "fatal: %s\n", mod_last_error() );
        goto shutdown;
    }

    mod_list_all();

    MOD_HOST_FETCH_API( example_gen_api_t, example_gen );
    example_gen_api()->test_function_one();
    example_gen_api()->test_function_two();


shutdown:

    fprintf( stderr, "%s\n", mod_last_error() );
    mod_system_exit();

    return 0;
}

/*============================================================================================*/