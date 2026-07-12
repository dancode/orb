#ifndef SB_REF_DLL_API_H
#define SB_REF_DLL_API_H
/*==============================================================================================

    sandbox/reflect/sb_ref_dll/sb_ref_dll_api.h -- sb_ref_dll API struct and gateway.

==============================================================================================*/

#include "sandbox/reflect/sb_ref_dll/sb_ref_dll.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct sb_ref_dll_api_s
{
    /* Returns a populated demo entity (pointer is stable for the module's lifetime). */
    const ex_entity_t* ( *demo_entity )( void );

    /* Returns a populated demo NPC whose on_damage field holds a live callback pointer. */
    const ex_npc_t* ( *demo_npc )( void );

} sb_ref_dll_api_t;

#if defined( BUILD_STATIC ) || defined( SB_REF_DLL_STATIC )
MOD_GATEWAY_STATIC( sb_ref_dll_api_t, sb_ref_dll )
    #define MOD_USE_SB_REF_DLL    /* static build */
    #define MOD_FETCH_SB_REF_DLL  true
#else
MOD_GATEWAY_DYNAMIC( sb_ref_dll_api_t, sb_ref_dll )
    #define MOD_USE_SB_REF_DLL    MOD_DEFINE_API_PTR( sb_ref_dll_api_t, sb_ref_dll )
    #define MOD_FETCH_SB_REF_DLL  MOD_FETCH_API( sb_ref_dll_api_t, sb_ref_dll )
#endif

// clang-format on
/*============================================================================================*/
#endif    // SB_REF_DLL_API_H
