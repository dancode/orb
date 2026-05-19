/*==============================================================================================

    engine/rs/rs_host.h - Host-only reflection API: lifecycle, frames, registration, lookup,
    walkers, serialization, diagnostics, and tests. Includes rs.h.
    See rs.md for the full boot sequence and how reflection integrates with the mod lifecycle.

==============================================================================================*/
#ifndef RS_HOST_H
#define RS_HOST_H

#include "engine/rs/rs_api.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    Lifecycle

    Self-bootstraps on first rs_register_module call — hosts need not call rs_init explicitly.
    rs_init is idempotent and available for test setups that need a clean registry.
==============================================================================================*/

void                rs_init                 ( void );
void                rs_exit                 ( void );

/*==============================================================================================
    Module Integration

    Push/pop a module's reflection frame. Called automatically by rs_wire_mod_callbacks()
    — hosts do not need to call these directly.
==============================================================================================*/

uint16_t            rs_register_module      ( const char* name, const mod_desc_t* desc );
void                rs_unregister_module    ( const char* name );

/*==============================================================================================
    Frames
==============================================================================================*/

uint16_t            rs_push_frame           ( const char* name );
void                rs_pop_frame            ( uint16_t frame_id );
bool                rs_finalize_frame       ( uint16_t frame_id );   /* resolve forward refs; false on error */
const rs_frame_t*   rs_get_frame            ( uint16_t frame_id );

/*==============================================================================================
    Registration

    Low-level entry points used by generated code and hand-rolled registration.
    Set field.type_hash = rs_hash_str(base_type_name) before calling.
    Attributes must be added contiguously per owner (all of type A's before type B's).
==============================================================================================*/

uint16_t            rs_register_type        ( const rs_type_t*, const rs_field_t*, uint16_t field_count );
uint16_t            rs_register_enum        ( const rs_type_t*, const rs_enum_t*,  uint16_t count );
uint16_t            rs_register_bitset      ( const rs_type_t*, const rs_enum_t*,  uint16_t count );
uint16_t            rs_register_function    ( const rs_type_t*, const rs_field_t* return_then_params, uint16_t count );
bool                rs_type_add_attr        ( uint16_t type_id,  const rs_attrib_t* );
bool                rs_field_add_attr       ( uint16_t field_id, const rs_attrib_t* );

/*==============================================================================================
    String Pool
==============================================================================================*/

rs_name_t           rs_intern               ( const char* s );  /* intern into pool; generated code calls api->intern */
const char*         rs_cstr                 ( rs_name_t id );   /* direct pointer into pool — no copy */

/*==============================================================================================
    Lookup
==============================================================================================*/

const rs_type_t*    rs_get_type             ( uint16_t type_id );
uint16_t            rs_find_type            ( uint32_t name_hash );
uint16_t            rs_find_type_by_name    ( const char* name );
const rs_field_t*   rs_get_field            ( uint16_t field_id );
const rs_field_t*   rs_find_field           ( uint16_t type_id,  const char* name );
const rs_attrib_t*  rs_type_get_attr        ( uint16_t type_id,  const char* name );
const rs_attrib_t*  rs_field_get_attr       ( uint16_t field_id, const char* name );
const rs_enum_t*    rs_enum_find_by_name    ( uint16_t type_id,  const char* name );
const rs_enum_t*    rs_enum_find_by_value   ( uint16_t type_id,  int64_t value );
const rs_enum_t*    rs_get_enumerator       ( uint16_t enum_id );
void                rs_get_stats            ( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count );

/*==============================================================================================
    Bitset Helpers  (type must have kind == RS_KIND_BITSET)

    Greedy bit-claim: registration order determines priority when flags have overlapping bits.
    Place multi-bit constants before their single-bit components to match them first.
==============================================================================================*/

bool                rs_enum_is_bitset       ( uint16_t type_id );
const rs_enum_t*    rs_bitset_find_flag     ( uint16_t type_id, int64_t mask );
uint16_t            rs_bitset_each_set_flag ( uint16_t type_id, int64_t value, rs_enum_cb_t cb, void* user );
size_t              rs_bitset_describe      ( uint16_t type_id, int64_t value, char* buf, size_t buf_size );

/*==============================================================================================
    Function Signature Accessors  (type must have kind == RS_KIND_FUNCTION)
==============================================================================================*/

const rs_field_t*   rs_function_get_return  ( uint16_t type_id );
uint16_t            rs_function_param_count ( uint16_t type_id );
const rs_field_t*   rs_function_get_param   ( uint16_t type_id, uint16_t param_index );

/*==============================================================================================
    Iteration
==============================================================================================*/

uint16_t            rs_each_type            ( rs_type_cb_t  cb, void* user );
uint16_t            rs_each_type_in_frame   ( uint16_t frame_id, rs_type_cb_t cb, void* user );
uint16_t            rs_each_field           ( uint16_t type_id,  rs_field_cb_t cb, void* user );
uint16_t            rs_each_enumerator      ( uint16_t type_id,  rs_enum_cb_t  cb, void* user );

/*==============================================================================================
    Walkers

    rs_walk_refs — visits every pointer-bearing slot in an instance. Does NOT follow pointers;
    hands each slot to the visitor. Recurses into nested structs and inline arrays of structs.
    Supported shapes: T, T*, T[N], T*[N], T(*)[N]. Function pointers and deep chains skipped.

    rs_walk — visits every field value. Recurses into nested structs. Function pointers and
    deep chains are visited once as opaque slots. Use field->mods to distinguish shapes.
==============================================================================================*/

void                rs_walk_refs            ( void* instance, uint16_t type_id, rs_ref_visitor_t visit, void* user );
void                rs_walk                 ( void* instance, uint16_t type_id, rs_visitor_t     visit, void* user );

/*==============================================================================================
    Serialization

    Format: [ 20-byte header ][ raw sizeof(T) body ]
    Pointer slots and @transient fields are zeroed in the saved body.
    Compatibility is gated on schema_hash — any layout change produces a mismatch.
    Pointers read back as NULL; persist references via stable IDs, not raw pointers.
==============================================================================================*/

size_t              rs_write                ( const void* instance, uint16_t type_id, uint8_t* buf, size_t cap );
rs_io_status_t      rs_read                 ( void* instance, uint16_t expected_type_id, const uint8_t* buf, size_t cap, size_t* bytes_read );
uint32_t            rs_peek_type_hash       ( const uint8_t* buf, size_t cap );

/*==============================================================================================
    Diagnostics
==============================================================================================*/

void                rs_print_types          ( void );
void                rs_print_type           ( uint16_t type_id );
void                rs_print_frame          ( uint16_t frame_id );
size_t              rs_field_describe       ( const rs_field_t* f, char* buf, size_t buf_size );

/*==============================================================================================
    Tests
==============================================================================================*/

void                rs_run_tests            ( void );

/*==============================================================================================
    Mod Callbacks

    Install pre_init / post_exit hooks so the mod system drives rs frame push/pop automatically.
    Safe to call before rs_init or mod_init_all have run.
==============================================================================================*/

static inline void
rs_host_on_pre_init( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( user );
    rs_register_module( name, desc ); /* no-op when desc->rs_register is NULL */
}

static inline void
rs_host_on_post_exit( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( desc );
    UNUSED( user );
    rs_unregister_module( name ); /* silent when no frame exists for `name` */
}

static inline void
rs_wire_mod_callbacks( void )
{
    mod_set_pre_init_cb ( rs_host_on_pre_init,  NULL );
    mod_set_post_exit_cb( rs_host_on_post_exit, NULL );
}

// clang-format on
/*============================================================================================*/
#endif    // RS_HOST_H
