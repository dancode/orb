/*==============================================================================================

    editor.c : editor host library.

    Static library that bootstraps the editor framework on top of the engine and runtime.
    Placeholder — not yet implemented.

==============================================================================================*/

#include "orb.h"
#include "editor/editor.h"

/*==============================================================================================
    API implementations
==============================================================================================*/

static void
editor_placeholder( void )
{
}

/*==============================================================================================
    Public API struct
==============================================================================================*/

const editor_api_t g_editor_api_struct = {
    .placeholder = editor_placeholder,
};

// editor_api_t*
// editor_api( void )
// {
//     return ( editor_api_t* )&g_editor_api;
// }

/*============================================================================================*/
