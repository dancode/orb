#ifndef PHYSICS_API_H
#define PHYSICS_API_H
/*==============================================================================================

    runtime_modules/physics/physics_api.h — physics module API struct and gateway macro.

==============================================================================================*/

#include "runtime_modules/physics/physics.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
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

#if defined( BUILD_STATIC ) || defined( PHYSICS_STATIC )
    #define MOD_USE_PHYSICS    /* static build */
    #define MOD_FETCH_PHYSICS  true
#else
    #define MOD_USE_PHYSICS    MOD_DEFINE_API_PTR( physics_api_t, physics )
    #define MOD_FETCH_PHYSICS  MOD_FETCH_API( physics_api_t, physics )
#endif

// clang-format on
/*============================================================================================*/
#endif    // PHYSICS_API_H
