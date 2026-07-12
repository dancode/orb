/*==============================================================================================

    sb_gen_dll.c - Demo generated module scaffolding


==============================================================================================*/
#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "sandbox/reflect/sb_gen_dll/sb_gen_dll.h"
#include "sb_gen_dll.generated.h"

#include "engine/core/core_api.h"

REF_MODULE( sb_gen_dll )
MOD_USE_CORE;

/*==============================================================================================
    API implementations
==============================================================================================*/

typedef struct sb_gen_dll_state_s
{
    int the_state_variable;

} sb_gen_dll_state_t;

bool
sb_gen_dll_mod_init( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    printf( "\n[sb_gen_dll] init\n" );

    if ( !MOD_FETCH_CORE ) {
        return false;
    }

    sid_t sid = core()->sid_intern_cstr( "test_sid_string" );
    UNUSED( sid );

    return true;
}

bool
sb_gen_dll_mod_reload( void* state, get_api_fn get_api )
{
    UNUSED( state );
    UNUSED( get_api );
    if ( !MOD_FETCH_CORE ) {
        return false;
    }
    printf( "\n[sb_gen_dll] reload\n" );
    return true;
}

void
sb_gen_dll_mod_exit( void* state )
{
    UNUSED( state );
    printf( "\n[sb_gen_dll] exit\n" );
}

/*==============================================================================================
    API implementations
==============================================================================================*/
REF_API() void test_function_one( void ) { return; }
REF_API() int test_function_two( void ) { return 99; }

mod_desc_t*
sb_gen_dll_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( sb_gen_dll_state_t ),
        .func_api      = MOD_API_FUNC( sb_gen_dll ),
        .func_api_size = sizeof( sb_gen_dll_api_t ),
        .deps          = { "core" },
        .dep_count     = 1,
        .init          = sb_gen_dll_mod_init,
        .reload        = sb_gen_dll_mod_reload,
        .exit          = sb_gen_dll_mod_exit,
        .ref_register  = MOD_REFLECT_FUNC( sb_gen_dll ),
    };
    return &api;
}

/*==============================================================================================
    DLL export
==============================================================================================*/

MOD_DEFINE_EXPORTS( sb_gen_dll )

/*============================================================================================*/
