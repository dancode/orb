/*==============================================================================================

    engine/rs/rs_api.h : rs_api_t function-pointer struct and module accessor macros.

==============================================================================================*/
#ifndef RS_API_H
#define RS_API_H

#include "engine/rs/rs.h"
#include "engine/mod/mod_import.h"

// clang-format off

/*==============================================================================================
    Reflection Runtime API
==============================================================================================*/

typedef struct rs_api_s
{
    /* Lookup */
    uint16_t            ( *find_type_by_name  )( const char* name );
    const rs_type_t*    ( *get_type           )( uint16_t type_id );
    const rs_field_t*   ( *get_field          )( uint16_t field_id );
    const rs_field_t*   ( *find_field         )( uint16_t type_id, const char* name );
    const rs_attrib_t*  ( *type_get_attr      )( uint16_t type_id, const char* key );
    const rs_attrib_t*  ( *field_get_attr     )( uint16_t field_id, const char* key );
    rs_name_t           ( *intern             )( const char* s );
    const char*         ( *cstr               )( rs_name_t id );

    /* Iteration */
    uint16_t            ( *each_type          )( rs_type_cb_t cb, void* user );
    uint16_t            ( *each_type_in_frame )( uint16_t frame_id, rs_type_cb_t cb, void* user );
    uint16_t            ( *each_field         )( uint16_t type_id, rs_field_cb_t cb, void* user );
    uint16_t            ( *each_enumerator    )( uint16_t type_id, rs_enum_cb_t cb, void* user );

    /* Bitset helpers */
    size_t              ( *bitset_describe    )( uint16_t type_id, int64_t value, char* buf, size_t cap );

    /* Walkers */
    void                ( *walk_refs          )( void* inst, uint16_t type_id, rs_ref_visitor_t fn, void* user );
    void                ( *walk               )( void* inst, uint16_t type_id, rs_visitor_t fn, void* user );

    /* Serialization */ 
    size_t              ( *write              )( const void* src, uint16_t type_id, uint8_t* out, size_t cap );
    rs_io_status_t      ( *read               )( void* dst, uint16_t type_id, const uint8_t* buf, size_t cap, size_t* consumed );
    uint32_t            ( *peek_type_hash     )( const uint8_t* buf, size_t cap );

    /* Diagnostics */
    size_t              ( *field_describe     )( const rs_field_t* f, char* buf, size_t cap );
    void                ( *print_type         )( uint16_t type_id );
    void                ( *print_types        )( void );

} rs_api_t;

/*============================================================================================*/
/* rs is always statically linked into the host — RS_STATIC is set by CMake globally. */

#if defined( BUILD_STATIC ) || defined( RS_STATIC )
    MOD_GATEWAY_STATIC( rs_api_t, rs )
#else
    MOD_GATEWAY_DYNAMIC( rs_api_t, rs )
#endif

#if defined( BUILD_STATIC ) || defined( RS_STATIC )
    #define MOD_USE_RS    /* static build — no pointer needed */
    #define MOD_FETCH_RS  true
#else
    #define MOD_USE_RS    MOD_DEFINE_API_PTR( rs_api_t, rs )
    #define MOD_FETCH_RS  MOD_FETCH_API( rs_api_t, rs )
#endif

// clang-format on
/*============================================================================================*/
#endif    // RS_API_H
