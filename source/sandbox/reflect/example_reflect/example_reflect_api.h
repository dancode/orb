#ifndef EXAMPLE_REFLECT_API_H
#define EXAMPLE_REFLECT_API_H
/*==============================================================================================

    sandbox/reflect/example_reflect/example_reflect_api.h — example_reflect API struct and gateway.

==============================================================================================*/

#include "sandbox/reflect/example_reflect/example_reflect.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct example_reflect_api_s
{
    /* Returns a populated demo entity (pointer is stable for the module's lifetime). */
    const ex_entity_t* ( *demo_entity )( void );

    /* Returns a populated demo NPC whose on_damage field holds a live callback pointer. */
    const ex_npc_t* ( *demo_npc )( void );

} example_reflect_api_t;

#if defined( BUILD_STATIC ) || defined( EXAMPLE_REFLECT_STATIC )
MOD_GATEWAY_STATIC( example_reflect_api_t, example_reflect )
#else
MOD_GATEWAY_DYNAMIC( example_reflect_api_t, example_reflect )
#endif

#if defined( BUILD_STATIC ) || defined( EXAMPLE_REFLECT_STATIC )
    #define MOD_USE_EXAMPLE_REFLECT    /* static build */
    #define MOD_FETCH_EXAMPLE_REFLECT  true
#else
    #define MOD_USE_EXAMPLE_REFLECT    MOD_DEFINE_API_PTR( example_reflect_api_t, example_reflect )
    #define MOD_FETCH_EXAMPLE_REFLECT  MOD_FETCH_API( example_reflect_api_t, example_reflect )
#endif

// clang-format on
/*============================================================================================*/
#endif    // EXAMPLE_REFLECT_API_H
