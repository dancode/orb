#ifndef EDITOR_EXAMPLE_H
#define EDITOR_EXAMPLE_H
/*==============================================================================================

    editor_modules/editor_example/editor_example.h — Example editor module API.

    Stub module demonstrating the editor module pattern.
    Hot-reloadable in dynamic builds; statically linked in monolithic builds.

==============================================================================================*/

#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct editor_example_api_s
{
    void ( *placeholder )( void );

} editor_example_api_t;

#if defined( BUILD_STATIC ) || defined( EDITOR_EXAMPLE_STATIC )
MOD_GATEWAY_STATIC( editor_example_api_t, editor_example )
mod_desc_t* editor_example_get_mod_desc( void );
#else
MOD_GATEWAY_DYNAMIC( editor_example_api_t, editor_example )
#endif

/*============================================================================================*/
#endif    // EDITOR_EXAMPLE_H
