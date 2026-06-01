/*==============================================================================================

    engine/ref/ref_host.h - Host-only reflection API: lifecycle, frames, registration, lookup,
    walkers, serialization, diagnostics, and tests. Includes ref.h.
    See ref.md for the full boot sequence and how reflection integrates with the mod lifecycle.

==============================================================================================*/
#ifndef REF_HOST_H
#define REF_HOST_H

#include "engine/ref/ref_api.h"
#include "engine/mod/mod_host.h"
#include "engine/mod/mod_export.h"

// clang-format off
/*==============================================================================================
    Lifecycle

    Self-bootstraps on first ref_register_module call — hosts need not call ref_init explicitly.
    ref_init is idempotent and available for test setups that need a clean registry.
==============================================================================================*/

void                ref_init                 ( void );
void                ref_exit                 ( void );

/*==============================================================================================
    Module Integration

    Push/pop a module's reflection frame. Called automatically by ref_wire_mod_callbacks()
    — hosts do not need to call these directly.
==============================================================================================*/

uint16_t            ref_register_module      ( const char* name, const mod_desc_t* desc );
void                ref_unregister_module    ( const char* name );

/*==============================================================================================
    Frames
==============================================================================================*/

uint16_t            ref_push_frame           ( const char* name );
void                ref_pop_frame            ( uint16_t frame_id );
bool                ref_finalize_frame       ( uint16_t frame_id );   /* resolve forward refs; false on error */
const ref_frame_t*  ref_get_frame            ( uint16_t frame_id );

/*==============================================================================================
    Registration

    Low-level entry points used by generated code and hand-rolled registration.
    Set field.type_hash = ref_hash_str(base_type_name) before calling.

    ATTR ORDERING RULE (hand-rolled registration only):
        Add ALL attributes for a given type or field before adding any attribute to the
        next type or field. Attributes are stored in a flat contiguous pool; interleaving
        owners corrupts the layout. Generated code always satisfies this automatically.

        Correct:
            ref_register_type( &ta, fields_a, n );   // register type A
            ref_type_add_attr( id_a, &attr1 );        // A's attrs, all together
            ref_type_add_attr( id_a, &attr2 );
            ref_register_type( &tb, fields_b, m );   // register type B
            ref_type_add_attr( id_b, &attr3 );        // B's attrs

        Wrong -- triggers FATAL in both debug and release:
            ref_type_add_attr( id_a, &attr1 );
            ref_type_add_attr( id_b, &attr3 );        // interleaved: corrupts pool
            ref_type_add_attr( id_a, &attr2 );        // slot is no longer contiguous

==============================================================================================*/

uint16_t            ref_register_type        ( const ref_type_t*, const ref_field_t*, uint16_t field_count );
uint16_t            ref_register_enum        ( const ref_type_t*, const ref_enum_t*,  uint16_t count );
uint16_t            ref_register_bitset      ( const ref_type_t*, const ref_enum_t*,  uint16_t count );
uint16_t            ref_register_function    ( const ref_type_t*, const ref_field_t* return_then_params, uint16_t count );
bool                ref_type_add_attr        ( uint16_t type_id,  const ref_attrib_t* );
bool                ref_field_add_attr       ( uint16_t field_id, const ref_attrib_t* );

/*==============================================================================================
    String Pool
==============================================================================================*/

ref_name_t          ref_intern               ( const char* s );  /* intern into pool; generated code calls api->intern */
const char*         ref_cstr                 ( ref_name_t id );   /* direct pointer into pool — no copy */

/*==============================================================================================
    Lookup
==============================================================================================*/

const ref_type_t*   ref_get_type             ( uint16_t type_id );
uint16_t            ref_find_type            ( uint32_t name_hash );
uint16_t            ref_find_type_by_name    ( const char* name );
const ref_field_t*  ref_get_field            ( uint16_t field_id );
const ref_field_t*  ref_find_field           ( uint16_t type_id,  const char* name );
const ref_attrib_t* ref_type_get_attr        ( uint16_t type_id,  const char* name );
const ref_attrib_t* ref_field_get_attr       ( uint16_t field_id, const char* name );
uint16_t            ref_type_get_attr_values  ( uint16_t type_id,  const char* name, const ref_attrib_t** out );
uint16_t            ref_field_get_attr_values ( uint16_t field_id, const char* name, const ref_attrib_t** out );
const ref_enum_t*   ref_enum_find_by_name    ( uint16_t type_id,  const char* name );
const ref_enum_t*   ref_enum_find_by_value   ( uint16_t type_id,  int32_t value );
const ref_enum_t*   ref_get_enumerator       ( uint16_t enum_id );
void                ref_get_stats            ( uint16_t* type_count, uint16_t* field_count, uint16_t* frame_count );

/*==============================================================================================
    Bitset Helpers  (type must have kind == REF_KIND_BITSET)

    Greedy bit-claim: registration order determines priority when flags have overlapping bits.
    Place multi-bit constants before their single-bit components to match them first.
==============================================================================================*/

bool                ref_enum_is_bitset       ( uint16_t type_id );
const ref_enum_t*   ref_bitset_find_flag     ( uint16_t type_id, int32_t mask );
uint16_t            ref_bitset_each_set_flag ( uint16_t type_id, int32_t value, ref_enum_cb_t cb, void* user );
size_t              ref_bitset_describe      ( uint16_t type_id, int32_t value, char* buf, size_t buf_size );

/*==============================================================================================
    Union Discriminant Accessor  (type must have kind == REF_KIND_UNION)

    Map a discriminant value to the union member it selects. Members declare their tag value
    with the @case attribute. Returns NULL if not a union or no member matches the value.
==============================================================================================*/

const ref_field_t*  ref_union_case_field     ( uint16_t union_type_id, int32_t case_value );

/*==============================================================================================
    Function Signature Accessors  (type must have kind == REF_KIND_FUNCTION)
==============================================================================================*/

const ref_field_t*  ref_function_get_return  ( uint16_t type_id );
uint16_t            ref_function_param_count ( uint16_t type_id );
const ref_field_t*  ref_function_get_param   ( uint16_t type_id, uint16_t param_index );

/*==============================================================================================
    Iteration
==============================================================================================*/

uint16_t            ref_each_type            ( ref_type_cb_t  cb, void* user );
uint16_t            ref_each_type_in_frame   ( uint16_t frame_id, ref_type_cb_t cb, void* user );
uint16_t            ref_each_field           ( uint16_t type_id,  ref_field_cb_t cb, void* user );
uint16_t            ref_each_enumerator      ( uint16_t type_id,  ref_enum_cb_t  cb, void* user );

/*==============================================================================================
    Walkers

    ref_walk_refs — visits every pointer-bearing slot in an instance. Does NOT follow pointers;
    hands each slot to the visitor. Recurses into nested structs and inline arrays of structs.
    Supported shapes: T, T*, T[N], T*[N], T(*)[N]. Function pointers and deep chains skipped.

    ref_walk — visits every field value. Recurses into nested structs. Function pointers and
    deep chains are visited once as opaque slots. Use field->mods to distinguish shapes.
==============================================================================================*/

void                ref_walk_refs            ( void* instance, uint16_t type_id, ref_ref_visitor_t visit, void* user );
void                ref_walk                 ( void* instance, uint16_t type_id, ref_visitor_t     visit, void* user );

/*==============================================================================================
    Serialization

    Format: [ 20-byte header ][ raw sizeof(T) body ]
    Pointer slots and @transient fields are zeroed in the saved body.
    Compatibility is gated on schema_hash — any layout change produces a mismatch.
    Pointers read back as NULL; persist references via stable IDs, not raw pointers.
==============================================================================================*/

size_t              ref_write                ( const void* instance, uint16_t type_id, uint8_t* buf, size_t cap );
ref_io_status_t      ref_read                 ( void* instance, uint16_t expected_type_id, const uint8_t* buf, size_t cap, size_t* bytes_read );
uint32_t            ref_peek_type_hash       ( const uint8_t* buf, size_t cap );

/*==============================================================================================
    Diagnostics
==============================================================================================*/

void                ref_print_types          ( void );
void                ref_print_type           ( uint16_t type_id );
void                ref_print_frame          ( uint16_t frame_id );
size_t              ref_field_describe       ( const ref_field_t* f, char* buf, size_t buf_size );

/*==============================================================================================
    Tests
==============================================================================================*/

void                ref_run_tests            ( void );

/*==============================================================================================
    Mod Callbacks

    Install pre_init / post_exit hooks so the mod system drives rs frame push/pop automatically.
    Safe to call before ref_init or mod_init_all have run.
==============================================================================================*/

static inline void
ref_host_on_pre_init( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( user );
    ref_register_module( name, desc ); /* no-op when desc->ref_register is NULL */
}

static inline void
ref_host_on_post_exit( const char* name, const mod_desc_t* desc, void* user )
{
    UNUSED( desc );
    UNUSED( user );
    ref_unregister_module( name ); /* silent when no frame exists for `name` */
}

static inline void
ref_wire_mod_callbacks( void )
{
    mod_set_pre_init_cb ( ref_host_on_pre_init,  NULL );
    mod_set_post_exit_cb( ref_host_on_post_exit, NULL );
}

/*==============================================================================================
    Module Descriptor

    Used by the host to register the ref module:
        mod_static_load( "ref", ref_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_static( ref );
==============================================================================================*/

mod_desc_t* ref_get_mod_desc( void );

// clang-format on
/*============================================================================================*/
#endif    // REF_HOST_H
