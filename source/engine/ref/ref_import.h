/*==============================================================================================

    engine/ref/ref_import.h - Registration vtable for generated reflection code.

    Include this in generated <module>.generated.h/.c files and in any code that
    implements an ref_register callback.  Do NOT include ref_api.h from generated
    files — it pulls in mod gateway machinery that registration code does not need.

    Include chain:  ref_import.h -> ref.h
    For DLL modules calling the runtime API, include ref_api.h instead.
    For host executables and test sandboxes, include ref_host.h.

==============================================================================================*/
#ifndef REF_IMPORT_H
#define REF_IMPORT_H

#include "engine/ref/ref.h"

// clang-format off
/*==============================================================================================
    Modifier Shorthand

    Assign ref_mods_t enum values from ref.h directly to field.mods.
    REF_NO_MODS is a zero-value alias for REF_MODS_VALUE (bare T field).

    Examples:
        field.mods = REF_MODS_PTR;        // T*
        field.mods = REF_MODS_ARRAY;      // T[N]
        field.mods = REF_MODS_PTR_ARRAY;  // T*[N]
==============================================================================================*/

#define REF_NO_MODS  ( (uint16_t)REF_MODS_VALUE )

/*==============================================================================================
    Codegen Helpers

    Used by generated <name>.generated.c files and hand-rolled ref_register implementations
    to build ref_type_t / ref_field_t initializers.
==============================================================================================*/

#define REF_OFFSETOF( T, m )    ( (uint16_t)offsetof( T, m ) )
#define REF_SIZEOF( T )         ( (uint16_t)sizeof( T ) )
#define REF_ALIGNOF( T )        ( (uint8_t)_Alignof( T ) )
#define REF_FIELD_SIZE( T, m )  ( (uint16_t)sizeof( ((T*)0)->m ) )
#define REF_ARRAY_COUNT( a )    ( (uint16_t)( sizeof( a ) / sizeof( (a)[0] ) ) )

/*==============================================================================================
    Registration API

    Vtable passed to generated <name>_ref_register() functions.  DLL modules call through
    this instead of calling ref_register_type etc. directly (those access g_ref in the host).
==============================================================================================*/

typedef struct ref_reg_api_s
{
    ref_name_t          ( *intern                  )( const char* );
    uint16_t            ( *ref_register_type       )( const ref_type_t*, const ref_field_t*, uint16_t );
    uint16_t            ( *ref_register_enum       )( const ref_type_t*, const ref_enum_t*,  uint16_t );
    uint16_t            ( *ref_register_bitset     )( const ref_type_t*, const ref_enum_t*,  uint16_t );
    uint16_t            ( *ref_register_function   )( const ref_type_t*, const ref_field_t*, uint16_t );
    bool                ( *ref_type_add_attr       )( uint16_t type_id,  const ref_attrib_t* );
    bool                ( *ref_field_add_attr      )( uint16_t field_id, const ref_attrib_t* );
    const ref_type_t*   ( *ref_get_type            )( uint16_t type_id );

} ref_reg_api_t;

// clang-format on
/*============================================================================================*/
#endif    // REF_IMPORT_H
