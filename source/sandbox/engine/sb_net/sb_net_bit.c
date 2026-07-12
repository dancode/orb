/*==============================================================================================

    sb_net_bit.c -- Layer 3 tests: bit writer/reader round-trips, varints,
    quantization, alignment, and overflow safety.

==============================================================================================*/

/*==============================================================================================
    Round-trip every width with patterned values
==============================================================================================*/

static void
net_test_bit_widths( void )
{
    printf( "  bit widths\n" );

    u32          buf[ 64 ];
    bit_writer_t w;
    bit_reader_t r;

    /* For each width 1..32, write three values that exercise low/high/patterned bits. */
    bit_writer_init( &w, buf, sizeof( buf ) );
    for ( i32 bits = 1; bits <= 32; bits++ )
    {
        u32 mask = ( u32 )( ( ( u64 )1 << bits ) - 1 );
        bit_write_bits( &w, 1, bits );
        bit_write_bits( &w, mask, bits );
        bit_write_bits( &w, 0xA5A5A5A5u & mask, bits );
    }
    i32 wire_bytes = bit_writer_flush( &w );
    sb_check( wire_bytes > 0, "width sweep flushed" );

    bit_reader_init( &r, buf, wire_bytes );
    bool all_ok = true;
    for ( i32 bits = 1; bits <= 32; bits++ )
    {
        u32 mask = ( u32 )( ( ( u64 )1 << bits ) - 1 );
        all_ok &= bit_read_bits( &r, bits ) == 1;
        all_ok &= bit_read_bits( &r, bits ) == mask;
        all_ok &= bit_read_bits( &r, bits ) == ( 0xA5A5A5A5u & mask );
    }
    sb_check( all_ok && bit_reader_ok( &r ), "width sweep round-trip" );
}

/*==============================================================================================
    Typed values, floats, and byte blocks at odd alignment
==============================================================================================*/

static void
net_test_bit_typed( void )
{
    printf( "  typed values\n" );

    u32          buf[ 64 ];
    bit_writer_t w;
    bit_reader_t r;

    const u8 blob[ 5 ] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };

    bit_writer_init( &w, buf, sizeof( buf ) );
    bit_write_bool( &w, true );
    bit_write_bool( &w, false );
    bit_write_u8( &w, 0x7F );
    bit_write_u16( &w, 0xBEEF );
    bit_write_u32( &w, 0xDEADBEEF );
    bit_write_u64( &w, 0x123456789ABCDEF0ull );
    bit_write_f32( &w, 3.14159f );
    bit_write_f64( &w, -2.718281828459045 );
    bit_write_bits( &w, 5, 3 );            /* leave the stream at an odd bit offset */
    bit_write_bytes( &w, blob, 5 );        /* aligns, then raw bytes */
    bit_write_bits( &w, 2, 2 );            /* trailing bits after the block */
    i32 wire_bytes = bit_writer_flush( &w );
    sb_check( wire_bytes > 0, "typed flushed" );

    bit_reader_init( &r, buf, wire_bytes );
    sb_check( bit_read_bool( &r ) == true, "bool true" );
    sb_check( bit_read_bool( &r ) == false, "bool false" );
    sb_check( bit_read_u8( &r ) == 0x7F, "u8" );
    sb_check( bit_read_u16( &r ) == 0xBEEF, "u16" );
    sb_check( bit_read_u32( &r ) == 0xDEADBEEF, "u32" );
    sb_check( bit_read_u64( &r ) == 0x123456789ABCDEF0ull, "u64" );
    sb_check( bit_read_f32( &r ) == 3.14159f, "f32 exact" );
    sb_check( bit_read_f64( &r ) == -2.718281828459045, "f64 exact" );
    sb_check( bit_read_bits( &r, 3 ) == 5, "odd offset bits" );

    u8 blob_in[ 5 ] = { 0 };
    bit_read_bytes( &r, blob_in, 5 );
    sb_check( memcmp( blob_in, blob, 5 ) == 0, "byte block intact" );
    sb_check( bit_read_bits( &r, 2 ) == 2, "trailing bits" );
    sb_check( bit_reader_ok( &r ), "typed stream ok" );
}

/*==============================================================================================
    Varints
==============================================================================================*/

static void
net_test_bit_varint( void )
{
    printf( "  varints\n" );

    static const u32 edges32[] = { 0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 0xFFFFFFFFu };
    static const u64 edges64[] = { 0, 127, 128, 0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull };

    u32          buf[ 64 ];
    bit_writer_t w;
    bit_reader_t r;

    bit_writer_init( &w, buf, sizeof( buf ) );
    for ( i32 i = 0; i < ( i32 )ARRAY_COUNT( edges32 ); i++ ) bit_write_varint_u32( &w, edges32[ i ] );
    for ( i32 i = 0; i < ( i32 )ARRAY_COUNT( edges64 ); i++ ) bit_write_varint_u64( &w, edges64[ i ] );
    i32 wire_bytes = bit_writer_flush( &w );

    bit_reader_init( &r, buf, wire_bytes );
    bool all_ok = true;
    for ( i32 i = 0; i < ( i32 )ARRAY_COUNT( edges32 ); i++ ) all_ok &= bit_read_varint_u32( &r ) == edges32[ i ];
    for ( i32 i = 0; i < ( i32 )ARRAY_COUNT( edges64 ); i++ ) all_ok &= bit_read_varint_u64( &r ) == edges64[ i ];
    sb_check( all_ok && bit_reader_ok( &r ), "varint edges round-trip" );

    /* Small values must cost one byte. */
    bit_writer_init( &w, buf, sizeof( buf ) );
    bit_write_varint_u32( &w, 100 );
    sb_check( w.bits_written == 8, "small varint is one byte" );
}

/*==============================================================================================
    Quantized ints and floats
==============================================================================================*/

static void
net_test_bit_quantized( void )
{
    printf( "  quantized values\n" );

    u32          buf[ 64 ];
    bit_writer_t w;
    bit_reader_t r;

    sb_check( bit_bits_for_range( 0 ) == 0, "range 0 costs 0 bits" );
    sb_check( bit_bits_for_range( 1 ) == 1, "range 1 costs 1 bit" );
    sb_check( bit_bits_for_range( 255 ) == 8, "range 255 costs 8 bits" );
    sb_check( bit_bits_for_range( 256 ) == 9, "range 256 costs 9 bits" );

    bit_writer_init( &w, buf, sizeof( buf ) );
    bit_write_int_range( &w, -100, -100, 100 );
    bit_write_int_range( &w, 0, -100, 100 );
    bit_write_int_range( &w, 100, -100, 100 );
    bit_write_int_range( &w, 999, -100, 100 );    /* out of range: clamps to max */
    bit_write_int_range( &w, 7, 7, 7 );           /* zero-range: costs nothing */
    i32 wire_bytes = bit_writer_flush( &w );

    bit_reader_init( &r, buf, wire_bytes );
    sb_check( bit_read_int_range( &r, -100, 100 ) == -100, "int_range min" );
    sb_check( bit_read_int_range( &r, -100, 100 ) == 0, "int_range mid" );
    sb_check( bit_read_int_range( &r, -100, 100 ) == 100, "int_range max" );
    sb_check( bit_read_int_range( &r, -100, 100 ) == 100, "int_range clamped" );
    sb_check( bit_read_int_range( &r, 7, 7 ) == 7, "int_range zero-range" );
    sb_check( bit_reader_ok( &r ), "int_range stream ok" );

    /* Quantized floats: error bound is resolution / 2 across the whole range.
       361 values at 17 bits each need ~768 bytes. */
    static u32 qbuf[ 256 ];
    const f32  res    = 1.0f / 256.0f;
    bool       in_tol = true;
    bit_writer_init( &w, qbuf, sizeof( qbuf ) );
    for ( i32 i = 0; i <= 360; i++ ) bit_write_float_q( &w, ( f32 )i * 0.987f, 0.0f, 360.0f, res );
    wire_bytes = bit_writer_flush( &w );
    sb_check( wire_bytes > 0, "float_q sweep flushed" );

    bit_reader_init( &r, qbuf, wire_bytes );
    for ( i32 i = 0; i <= 360; i++ )
    {
        f32 v   = ( f32 )i * 0.987f;
        f32 got = bit_read_float_q( &r, 0.0f, 360.0f, res );
        f32 err = got - v;
        if ( err < 0 ) err = -err;
        in_tol &= err <= res * 0.5f + 0.0001f;
    }
    sb_check( in_tol && bit_reader_ok( &r ), "float_q within half resolution" );
}

/*==============================================================================================
    Overflow safety
==============================================================================================*/

static void
net_test_bit_overflow( void )
{
    printf( "  overflow safety\n" );

    u32          buf[ 2 ];    /* 8 bytes = 64 bits */
    bit_writer_t w;
    bit_reader_t r;

    bit_writer_init( &w, buf, sizeof( buf ) );
    bit_write_u32( &w, 0x11111111 );
    bit_write_u32( &w, 0x22222222 );
    sb_check( bit_writer_ok( &w ), "writer full but not overflowed" );

    bit_write_bits( &w, 1, 1 );    /* one bit too many */
    sb_check( !bit_writer_ok( &w ), "writer overflow detected" );
    sb_check( bit_writer_flush( &w ) == -1, "flush reports overflow" );
    sb_check( buf[ 0 ] == 0x11111111 && buf[ 1 ] == 0x22222222, "buffer not corrupted" );

    /* Reader: drain exactly, then one bit too many. */
    bit_reader_init( &r, buf, 8 );
    sb_check( bit_read_u32( &r ) == 0x11111111, "read word 0" );
    sb_check( bit_read_u32( &r ) == 0x22222222, "read word 1" );
    sb_check( bit_reader_ok( &r ), "reader drained cleanly" );

    sb_check( bit_read_bits( &r, 1 ) == 0, "past-end read returns 0" );
    sb_check( !bit_reader_ok( &r ), "reader overflow detected" );
    sb_check( bit_read_u32( &r ) == 0, "reads keep returning 0 after overflow" );

    /* Truncated varint must flag, not loop or crash. */
    u32 vbuf[ 4 ];
    bit_writer_init( &w, vbuf, sizeof( vbuf ) );
    bit_write_varint_u32( &w, 0xFFFFFFFFu );
    i32 wire_bytes = bit_writer_flush( &w );

    bit_reader_init( &r, vbuf, wire_bytes - 1 );    /* chop the final group */
    bit_read_varint_u32( &r );
    sb_check( !bit_reader_ok( &r ), "truncated varint flagged" );
}

/*==============================================================================================
    Suite entry
==============================================================================================*/

static void
net_test_bit( void )
{
    net_test_bit_widths();
    net_test_bit_typed();
    net_test_bit_varint();
    net_test_bit_quantized();
    net_test_bit_overflow();
}

/*============================================================================================*/
