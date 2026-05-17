#ifndef EDITOR_H
#define EDITOR_H
/*==============================================================================================

    editor/editor.h — Editor host library API.

    Static library that provides the editor framework on top of the engine and runtime.
    Always statically linked into the host executable; never hot-reloaded.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"

/*==============================================================================================
    API struct
==============================================================================================*/

typedef struct editor_api_s
{
    void ( *placeholder )( void );

} editor_api_t;

#if defined( BUILD_STATIC ) || defined( EDITOR_STATIC )
MOD_GATEWAY_STATIC( editor_api_t, editor )
#else
MOD_GATEWAY_DYNAMIC( editor_api_t, editor )
#endif

/*============================================================================================*/
#endif    // EDITOR_H
