/*==============================================================================================

    example_gen.c - Demo generated module scaffolding


==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "runtime_modules/example_gen/example_gen.h"
#include "example_gen.generated.h"

RS_MODULE( example_gen )

/*==============================================================================================
    API implementations
==============================================================================================*/

typedef struct example_gen_state_s
{
    int the_state_variable;

} example_gen_state_t;

static example_gen_state_t* s = NULL;

bool
example_gen_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    printf( "\n[example_gen] init\n" );
    return true;
}

bool
example_gen_mod_reload( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    printf( "\n[example_gen] reload\n" );
    return true;
}

void
example_gen_mod_exit( void* state )
{
    UNUSED( state );
    printf( "\n[example_gen] exit\n" );
}

/*==============================================================================================
    API implementations
==============================================================================================*/
RS_API() void test_function_one( void ) { return; }

RS_API() int test_function_two( void ) { return 99; }

mod_desc_t*
example_gen_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( example_gen_state_t ),
        .func_api      = MOD_API_FUNC( example_gen ),
        .func_api_size = sizeof( example_gen_api_t ),
        .deps          = {},
        .dep_count     = 0,
        .init          = example_gen_mod_init,
        .reload        = example_gen_mod_reload,
        .exit          = example_gen_mod_exit,
        .rs_register   = MOD_REFLECT_FUNC( example_gen ),
    };
    return &api;
}

/*==============================================================================================
    DLL export
==============================================================================================*/

MOD_DEFINE_EXPORTS( example_gen )

/*============================================================================================*/
