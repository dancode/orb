/*==============================================================================================

    engine/rs/rs_import.h - Registration vtable for generated reflection code.

    Include this in generated <module>.generated.h/.c files and in any code that
    implements an rs_register callback.  Do NOT include rs_api.h from generated
    files — it pulls in mod gateway machinery that registration code does not need.

    Include chain:  rs_import.h -> rs.h
    For DLL modules calling the runtime API, include rs_api.h instead.
    For host executables and test sandboxes, include rs_host.h.

==============================================================================================*/
#ifndef RS_IMPORT_H
#define RS_IMPORT_H

#include "engine/rs/rs.h"

// clang-format off
/*==============================================================================================
    Slot Builders

    Hand-craft packed modifier chains when not using the reflection generator.
    RS_MOD_SLOT builds one 4-bit slot; RS_MODS packs up to four slots into a uint16_t.
    Use RS_M_END (== 0) to fill unused trailing slots.
==============================================================================================*/

#define RS_MOD_SLOT( op, is_const )   ( ((op) & 0x3) | (((is_const) & 0x1) << 2) )
#define RS_MODS( s0, s1, s2, s3 )     ( (uint16_t)((s0) | ((s1) << 4) | ((s2) << 8) | ((s3) << 12)) )
#define RS_M_END                      RS_MOD_SLOT( RS_MOD_NONE,     0 )
#define RS_M_PTR                      RS_MOD_SLOT( RS_MOD_PTR,      0 )
#define RS_M_CONST_PTR                RS_MOD_SLOT( RS_MOD_PTR,      1 )   /* T* const -- the pointer is const */
#define RS_M_ARRAY                    RS_MOD_SLOT( RS_MOD_ARRAY,    0 )
#define RS_M_FUNCTION                 RS_MOD_SLOT( RS_MOD_FUNCTION, 0 )
#define RS_NO_MODS                    ( (uint16_t)0 )

/*==============================================================================================
    Codegen Helpers

    Used by generated <name>.generated.c files and hand-rolled rs_register implementations
    to build rs_type_t / rs_field_t initializers.
==============================================================================================*/

#define RS_OFFSETOF( T, m )    ( (uint16_t)offsetof( T, m ) )
#define RS_SIZEOF( T )         ( (uint16_t)sizeof( T ) )
#define RS_ALIGNOF( T )        ( (uint8_t)_Alignof( T ) )
#define RS_FIELD_SIZE( T, m )  ( (uint16_t)sizeof( ((T*)0)->m ) )
#define RS_ARRAY_COUNT( a )    ( (uint16_t)( sizeof( a ) / sizeof( (a)[0] ) ) )

/*==============================================================================================
    Registration API

    Vtable passed to generated <name>_rs_register() functions.  DLL modules call through
    this instead of calling rs_register_type etc. directly (those access g_rs in the host).
==============================================================================================*/

typedef struct rs_reg_api_s
{
    rs_name_t           ( *intern             )( const char* );
    uint16_t            ( *rs_register_type   )( const rs_type_t*, const rs_field_t*, uint16_t );
    uint16_t            ( *rs_register_enum   )( const rs_type_t*, const rs_enum_t*,  uint16_t );
    uint16_t            ( *rs_register_bitset )( const rs_type_t*, const rs_enum_t*,  uint16_t );
    bool                ( *rs_type_add_attr   )( uint16_t type_id,  const rs_attrib_t* );
    bool                ( *rs_field_add_attr  )( uint16_t field_id, const rs_attrib_t* );
    const rs_type_t*    ( *rs_get_type        )( uint16_t type_id );

} rs_reg_api_t;

// clang-format on
/*============================================================================================*/
#endif    // RS_IMPORT_H
