#ifndef PHYSICS_API_H
#define PHYSICS_API_H

/*==============================================================================================

    physics_api.h : public API for physics module

==============================================================================================*/

#include "engine/mod/mod_api.h"

/*==============================================================================================
    Physics API struct
==============================================================================================*/

typedef struct physics_api_s
{
    void ( *physics_function )( void );

} physics_api_t;

#if defined( BUILD_STATIC ) || defined( PHYSICS_STATIC )
MOD_GATEWAY_STATIC( physics_api_t, physics )
#else
MOD_GATEWAY_DYNAMIC( physics_api_t, physics )
#endif

/*============================================================================================*/
#endif    // PHYSICS_API_H