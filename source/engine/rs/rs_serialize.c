/* engine/rs/rs_serialize.c - Binary read/write gated on schema_hash.
   Body = raw sizeof(T) bytes; pointer slots and @transient fields are zeroed before save. */

// clang-format off

/*==============================================================================================
    Binary I/O Helpers

    Endian-neutral (little-endian) read/write for header fields.
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

    Walks an instance and zeros out volatile data (pointers, @transient) before writing.
==============================================================================================*/

static void
rs_serialize_zero_ptr( void** slot, uint16_t pointee_id, const rs_field_t* f, void* user )
{
    (void)pointee_id; (void)f; (void)user;
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

        if ( rs_field_get_attr( (uint16_t)( t->field_index + i ), "transient" ) )
        {
            memset( body + f->offset, 0, f->size );
            continue;
        }

        uint8_t s0 = RS_MOD_GET( f->mods, 0 );
        if ( RS_MOD_OP( s0 ) == RS_MOD_NONE && ( f->kind == RS_KIND_STRUCT || f->kind == RS_KIND_UNION ) )
            rs_serialize_zero_transient( body + f->offset, f->type_id );
    }
}

/*==============================================================================================
    Binary Serialization API

    Schema-gated raw memory snapshots. Supports pointer zeroing and transient skip.
==============================================================================================*/

size_t
rs_write( const void* instance, uint16_t type_id, uint8_t* buf, size_t cap )
{
    if ( !instance || !buf ) return 0;

    const rs_type_t* t = rs_get_type( type_id );
    if ( !t || ( t->kind != RS_KIND_STRUCT && t->kind != RS_KIND_UNION ) ) return 0;

    size_t need = (size_t)RS_SAVE_HEADER_SIZE + t->size;
    if ( cap < need ) return 0;

    rs_write_u32_le( buf +  0, RS_SAVE_MAGIC );
    rs_write_u32_le( buf +  4, t->hash );
    rs_write_u32_le( buf +  8, t->schema_hash );
    rs_write_u32_le( buf + 12, (uint32_t)t->size );
    rs_write_u32_le( buf + 16, 0 );

    uint8_t* body = buf + RS_SAVE_HEADER_SIZE;
    memcpy( body, instance, t->size );

    rs_walk_refs( body, type_id, rs_serialize_zero_ptr, NULL );
    rs_serialize_zero_transient( body, type_id );

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

    if ( magic != RS_SAVE_MAGIC ) return RS_IO_INCOMPAT;

    const rs_type_t* t = rs_get_type( expected_type_id );
    if ( !t ) return RS_IO_NO_TYPE;

    if ( t->hash        != type_hash   ) return RS_IO_INCOMPAT;
    if ( t->schema_hash != schema_hash ) return RS_IO_INCOMPAT;
    if ( body_size      != t->size     ) return RS_IO_INCOMPAT;

    if ( cap < (size_t)RS_SAVE_HEADER_SIZE + body_size ) return RS_IO_TRUNCATED;

    memcpy( instance, buf + RS_SAVE_HEADER_SIZE, body_size );

    if ( bytes_read ) *bytes_read = (size_t)RS_SAVE_HEADER_SIZE + body_size;
    return RS_IO_OK;
}

uint32_t
rs_peek_type_hash( const uint8_t* buf, size_t cap )
{
    if ( !buf || cap < RS_SAVE_HEADER_SIZE ) return 0;
    if ( rs_read_u32_le( buf + 0 ) != RS_SAVE_MAGIC ) return 0;
    return rs_read_u32_le( buf + 4 );
}

/*============================================================================================*/
// clang-format on
