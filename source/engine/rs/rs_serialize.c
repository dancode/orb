/* engine/rs/rs_serialize.c - Binary read/write gated on schema_hash.

   Save format: a 20-byte header followed by a raw sizeof(T) byte copy of the instance.
   Before writing, pointer slots are zeroed (runtime addresses have no meaning on reload)
   and @transient fields are zeroed (computed/cached data is never meaningful on load).
   rs_read validates magic, name_hash, schema_hash, and body_size before memcpy, refusing
   any buffer that does not exactly match the registered type layout.

   Header layout (all fields little-endian u32):
       [0..3]   RS_SAVE_MAGIC  (0x31307372 = 'rs01')
       [4..7]   type name_hash
       [8..11]  schema_hash   (field layout fingerprint)
       [12..15] body_size     (sizeof(T) at registration time)
       [16..19] reserved / 0
*/

// clang-format off

/*==============================================================================================
    Binary I/O Helpers

    Byte-by-byte little-endian helpers make the save format endian-stable across platforms.
    On x86/x64 (the current target) this is a no-op in practice, but writing it explicitly
    ensures the format remains correct if the engine is ever ported to a big-endian target.
==============================================================================================*/

static void rs_write_u32_le( uint8_t* dst, uint32_t v )
{
    dst[0]=(uint8_t)(v); dst[1]=(uint8_t)(v>>8); dst[2]=(uint8_t)(v>>16); dst[3]=(uint8_t)(v>>24);
}

static uint32_t rs_read_u32_le( const uint8_t* src )
{
    return (uint32_t)src[0] | ((uint32_t)src[1]<<8) | ((uint32_t)src[2]<<16) | ((uint32_t)src[3]<<24);
}

/*==============================================================================================
    Serialization Prep

    Before writing the instance body to the save buffer, two redaction passes sanitize
    volatile fields so the saved bytes are deterministic and safe to reload:

      1. Pointer zeroing -- all pointer slots are set to NULL. Runtime addresses are
         meaningless after a reload or between sessions; leaving them would make saves
         non-reproducible and would crash on naive memcpy-back.

      2. Transient field zeroing -- fields tagged with the @transient attribute represent
         cached or computed state (e.g. a pre-built acceleration structure, a render handle)
         that must be rebuilt at runtime. They are always zeroed on save so that loaded
         instances start in a clean "not yet initialized" state.
==============================================================================================*/

static void
rs_serialize_zero_ptr( void** slot, uint16_t pointee_id, const rs_field_t* f, void* user )
{
    (void)pointee_id; (void)f; (void)user;
    /* Visitor callback: null out the pointer slot in the save buffer copy. */
    *slot = NULL;
}

static void
rs_serialize_zero_transient( uint8_t* body, uint16_t type_id )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || ( t->kind != RS_KIND_STRUCT && t->kind != RS_KIND_UNION ) ) return;

    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        const rs_field_t* f = &g_rs.fields[ t->field_index + i ];

        /* Zero any field annotated @transient -- it holds runtime-only computed state. */
        if ( rs_field_get_attr( (uint16_t)( t->field_index + i ), "transient" ) )
        {
            memset( body + f->offset, 0, f->size );
            continue;
        }

        /* Recurse into nested aggregates to catch @transient fields inside them. */
        if ( f->mods == RS_MODS_VALUE && ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION ) )
            rs_serialize_zero_transient( body + f->offset, f->type_id );
    }
}

/*==============================================================================================
    Binary Serialization API

    Schema-gated raw memory snapshots. The save format is a fixed 20-byte header followed
    by a sanitized copy of sizeof(T) bytes. No field-by-field encoding -- the struct is
    written as-is after pointer and @transient slots are zeroed. This is fast and compact
    but means the format is not self-describing; the schema_hash is the compatibility gate.
==============================================================================================*/

size_t
rs_write( const void* instance, uint16_t type_id, uint8_t* buf, size_t cap )
{
    if ( !instance || !buf ) return 0;

    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || ( t->kind != RS_KIND_STRUCT && t->kind != RS_KIND_UNION ) ) return 0;

    size_t need = (size_t)RS_SAVE_HEADER_SIZE + t->size;
    if ( cap < need ) return 0;

    /* Write the 20-byte header: magic, name_hash, schema_hash, body_size, reserved. */
    rs_write_u32_le( buf +  0, RS_SAVE_MAGIC );
    rs_write_u32_le( buf +  4, t->name_hash );
    rs_write_u32_le( buf +  8, t->schema_hash );
    rs_write_u32_le( buf + 12, (uint32_t)t->size );
    rs_write_u32_le( buf + 16, 0 );

    /* Copy the instance verbatim into the body, then sanitize volatile data in-place. */
    uint8_t* body = buf + RS_SAVE_HEADER_SIZE;
    memcpy( body, instance, t->size );

    rs_walk_refs( body, type_id, rs_serialize_zero_ptr, NULL );   /* zero pointer slots */
    rs_serialize_zero_transient( body, type_id );                 /* zero @transient fields */

    return need;
}

rs_io_status_t
rs_read( void* instance, uint16_t expected_type_id,
         const uint8_t* buf, size_t cap, size_t* bytes_read )
{
    if ( !instance || !buf ) return RS_IO_BAD_ARG;
    if ( cap < RS_SAVE_HEADER_SIZE ) return RS_IO_TRUNCATED;

    uint32_t magic       = rs_read_u32_le( buf +  0 );
    uint32_t type_hash   = rs_read_u32_le( buf +  4 );
    uint32_t schema_hash = rs_read_u32_le( buf +  8 );
    uint32_t body_size   = rs_read_u32_le( buf + 12 );

    /* Validate in order of cheapest check first:
       magic -> name identity -> field layout -> body size -> buffer capacity. */
    if ( magic != RS_SAVE_MAGIC ) return RS_IO_INCOMPAT;

    const rs_type_t* t = rs_get_type( expected_type_id );
    if ( !t ) return RS_IO_NO_TYPE;

    if ( t->name_hash   != type_hash   ) return RS_IO_INCOMPAT;   /* wrong type entirely */
    if ( t->schema_hash != schema_hash ) return RS_IO_INCOMPAT;   /* layout changed (hot-reload / version) */
    if ( body_size      != t->size     ) return RS_IO_INCOMPAT;   /* sizeof mismatch */

    if ( cap < (size_t)RS_SAVE_HEADER_SIZE + body_size ) return RS_IO_TRUNCATED;

    memcpy( instance, buf + RS_SAVE_HEADER_SIZE, body_size );

    if ( bytes_read ) *bytes_read = (size_t)RS_SAVE_HEADER_SIZE + body_size;
    return RS_IO_OK;
}

uint32_t
rs_peek_type_hash( const uint8_t* buf, size_t cap )
{
    /* Lightweight pre-check: read only the type_hash field from the header without
       performing a full read. Used to identify the type before choosing an expected_type_id. */
    if ( !buf || cap < RS_SAVE_HEADER_SIZE ) return 0;
    if ( rs_read_u32_le( buf + 0 ) != RS_SAVE_MAGIC ) return 0;
    return rs_read_u32_le( buf + 4 );
}

/*============================================================================================*/
// clang-format on
