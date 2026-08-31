/*==============================================================================================

    runtime_service/gui/gui_names.c -- GUI_NAMES translation unit: the shared name pool.

    Storage for the one gui_str_pool_t every resource registry's lookup-key string interns
    into (gui_names.h).  Depends on nothing but the generic pool primitive, so it sits at the
    bottom of the stack next to gui_rect.c / gui_log.c and is reached the same way -- through
    the public gui.h chain -- rather than by each consumer including it directly.

==============================================================================================*/

#include "orb.h"

#include "runtime_service/gui/gui_str_pool.h"
#include "runtime_service/gui/gui_names.h"

static gui_str_pool_t s_names;

u32
gui_names_intern( const char* s )
{
    return gui_str_pool_intern( &s_names, s );
}

const char*
gui_names_cstr( u32 off )
{
    return gui_str_pool_cstr( &s_names, off );
}

void
gui_names_reset( void )
{
    gui_str_pool_reset( &s_names );
}

/*============================================================================================*/
