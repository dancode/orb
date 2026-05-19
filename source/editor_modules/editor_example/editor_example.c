/*==============================================================================================

    editor_example.c : example editor module.

    Stub module demonstrating the editor module pattern.
    Hot-reloadable in dynamic builds; statically linked in monolithic builds.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod_export.h"
#include "editor_modules/editor_example/editor_example_api.h"

/*==============================================================================================
    Persistent state
==============================================================================================*/

typedef struct
{
    int init;

} editor_example_state_t;

static editor_example_state_t* s_state = NULL;

/*==============================================================================================
    API implementations
==============================================================================================*/

static void
editor_example_placeholder( void )
{
}

/*==============================================================================================
    Public API struct
==============================================================================================*/

const editor_example_api_t g_editor_example_api = {
    .placeholder = editor_example_placeholder,
};

/*==============================================================================================
    Lifecycle callbacks
==============================================================================================*/

static bool
editor_example_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s_state       = ( editor_example_state_t* )raw_state;
    s_state->init = 1;
    return true;
}

static bool
editor_example_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s_state = ( editor_example_state_t* )raw_state;
    return true;
}

static void
editor_example_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
editor_example_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( editor_example_state_t ),
        .func_api_size = sizeof( editor_example_api_t ),
        .dep_count     = 0,
        .deps          = { NULL },
        .func_api      = &g_editor_example_api,
        .init          = editor_example_init,
        .exit          = editor_example_exit,
        .reload        = editor_example_reload,
    };
    return &desc;
}

void*
editor_example_get_api( void )
{
    return ( void* )&g_editor_example_api;
}

/*==============================================================================================
    DLL exports — only present in dynamic builds
==============================================================================================*/

MOD_DEFINE_EXPORTS( editor_example )

/*============================================================================================*/
