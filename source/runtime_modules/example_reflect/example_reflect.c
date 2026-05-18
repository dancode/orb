/*==============================================================================================

    example_reflect.c - Demo module that exercises the rs_ reflection system.

    The module ships no reflection plumbing of its own. Generated code
    (example_reflect.generated.c) defines example_reflect_rs_register(); we hand the
    function pointer to the mod system via the descriptor's rs_register slot, and the
    host's load callback invokes it. Same path for static and dynamic builds.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod_export.h"

#include "runtime_modules/example_reflect/example_reflect.h"
#include "example_reflect.generated.h"
#include <string.h>

// RS_MODULE( example_reflect )

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    ex_entity_t demo;
    ex_vec3_t   demo_velocity;
    ex_vec3_t   demo_slot[ 2 ];

} example_reflect_state_t;

static example_reflect_state_t* s = NULL;

/*==============================================================================================
    Demo instance setup
==============================================================================================*/

static void
populate_demo( example_reflect_state_t* st )
{
    memset( &st->demo, 0, sizeof st->demo );

    st->demo.id     = 7;
    st->demo.facing = EX_FACING_EAST;
    st->demo.caps   = EX_CAPS_MOVE | EX_CAPS_RENDER | EX_CAPS_SCRIPT;

    strncpy( st->demo.name, "demo_entity", sizeof st->demo.name - 1 );

    st->demo.transform.position = ( ex_vec3_t ){ 1.0f, 2.0f, 3.0f };
    st->demo.transform.rotation = ( ex_vec3_t ){ 0.0f, 0.0f, 0.0f };
    st->demo.transform.scale    = ( ex_vec3_t ){ 1.0f, 1.0f, 1.0f };
    st->demo.health             = 42.5f;

    st->demo_velocity           = ( ex_vec3_t ){ 10.0f, 0.0f, -5.0f };
    st->demo.velocity           = &st->demo_velocity;
    st->demo.label              = "demo-label";

    st->demo_slot[ 0 ]          = ( ex_vec3_t ){ 0.5f, 0.5f, 0.5f };
    st->demo_slot[ 1 ]          = ( ex_vec3_t ){ 0.0f, 1.0f, 0.0f };
    st->demo.slots[ 0 ]         = &st->demo_slot[ 0 ];
    st->demo.slots[ 1 ]         = &st->demo_slot[ 1 ];
    st->demo.slots[ 2 ]         = NULL;
    st->demo.slots[ 3 ]         = NULL;
}

/*==============================================================================================
    API implementations
==============================================================================================*/

RS_API() 
const ex_entity_t*
api_demo_entity( void )
{
    return s ? &s->demo : NULL;
}

// const example_reflect_api_t g_example_reflect_api_struct = {
//     .demo_entity = api_demo_entity,
// };

/*==============================================================================================
    Lifecycle callbacks
==============================================================================================*/

bool
example_reflect_mod_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s = ( example_reflect_state_t* )raw_state;
    populate_demo( s );
    return true;
}

bool
example_reflect_mod_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s = ( example_reflect_state_t* )raw_state;
    return true;
}

void
example_reflect_mod_exit( void* raw_state )
{
    UNUSED( raw_state );
}

RS_API() int 
this_is_a_function()
{
    return 123;
}


/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
example_reflect_get_mod_desc( void )
{
    static mod_desc_t api = {
        .version       = 1,
        .state_size    = sizeof( example_reflect_state_t ),
        .func_api_size = sizeof( example_reflect_api_t ),
        .func_api      = &g_example_reflect_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = example_reflect_mod_init,
        .reload        = example_reflect_mod_reload,
        .exit          = example_reflect_mod_exit,
        .rs_register   = MOD_REFLECT_FUNC( example_reflect ),
    };
    return &api;
}

/*==============================================================================================
    DLL export
==============================================================================================*/

MOD_DEFINE_EXPORTS( example_reflect )

/*============================================================================================*/
