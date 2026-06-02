/*==============================================================================================

    engine/ref/ref_api.h : ref_api_t function-pointer struct and module accessor macros.

==============================================================================================*/
#ifndef REF_API_H
#define REF_API_H

#include "engine/ref/ref.h"
#include "engine/mod/mod_import.h"

// clang-format off

/*==============================================================================================
    Reflection Runtime API
==============================================================================================*/

typedef struct ref_api_s
{
    /* Lookup */
    uint16_t            ( *find_type_by_name    )( const char* name );
    const ref_type_t*   ( *get_type             )( uint16_t type_id );
    const ref_field_t*  ( *get_field            )( uint16_t field_id );
    const ref_field_t*  ( *find_field           )( uint16_t type_id, const char* name );
    const ref_attrib_t* ( *type_get_attr        )( uint16_t type_id, const char* key );
    const ref_attrib_t* ( *field_get_attr       )( uint16_t field_id, const char* key );
    uint16_t            ( *type_get_attr_values )( uint16_t type_id, const char* key, const ref_attrib_t** out );
    uint16_t            ( *field_get_attr_values)( uint16_t field_id, const char* key, const ref_attrib_t** out );
    ref_name_t          ( *intern               )( const char* s );
    const char*         ( *cstr                 )( ref_name_t id );

    /* Iteration */
    uint16_t            ( *each_type            )( ref_type_cb_t cb, void* user );
    uint16_t            ( *each_type_in_frame   )( uint16_t frame_id, ref_type_cb_t cb, void* user );
    uint16_t            ( *each_field           )( uint16_t type_id, ref_field_cb_t cb, void* user );
    uint16_t            ( *each_enumerator      )( uint16_t type_id, ref_enum_cb_t cb, void* user );

    /* Bitset helpers */
    size_t              ( *bitset_describe      )( uint16_t type_id, int32_t value, char* buf, size_t cap );

    /* Union discriminant */
    const ref_field_t*  ( *union_case_field     )( uint16_t union_type_id, int32_t case_value );

    /* Walkers */
    void                ( *walk_refs            )( void* inst, uint16_t type_id, ref_ref_visitor_t fn, void* user );
    void                ( *walk                 )( void* inst, uint16_t type_id, ref_visitor_t fn, void* user );

    /* Serialization */ 
    size_t              ( *write                )( const void* src, uint16_t type_id, uint8_t* out, size_t cap );
    ref_io_status_t     ( *read                 )( void* dst, uint16_t type_id, const uint8_t* buf, size_t cap, size_t* consumed );
    uint32_t            ( *peek_type_hash       )( const uint8_t* buf, size_t cap );

    /* Diagnostics */
    size_t              ( *field_describe       )( const ref_field_t* f, char* buf, size_t cap );
    void                ( *print_type           )( uint16_t type_id );
    void                ( *print_types          )( void );

} ref_api_t;

/*============================================================================================*/
/* ref is always statically linked into the host — REF_STATIC is set by the build globally. */

#if defined( BUILD_STATIC ) || defined( REF_STATIC )
    MOD_GATEWAY_STATIC( ref_api_t, ref )
#else
    MOD_GATEWAY_DYNAMIC( ref_api_t, ref )
#endif

#if defined( BUILD_STATIC ) || defined( REF_STATIC )
    #define MOD_USE_REF    /* static build — no pointer needed */
    #define MOD_FETCH_REF  true
#else
    #define MOD_USE_REF    MOD_DEFINE_API_PTR( ref_api_t, ref )
    #define MOD_FETCH_REF  MOD_FETCH_API( ref_api_t, ref )
#endif

// clang-format on
/*============================================================================================*/
#endif    // REF_API_H
