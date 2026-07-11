#ifndef NET_BIT_H
#define NET_BIT_H
/*==============================================================================================

    engine/net/net_bit.h -- Bit-level wire serialization (Layer 3).

    A bit writer/reader pair over caller-owned buffers. All functions are ORB_INLINE so DLL
    modules serialize at full speed with no vtable hop. Wire format is little-endian bit
    order: values are packed LSB-first into u32 words via a u64 scratch (yojimbo-style).

    Contract:
      - Buffers are accessed a whole u32 word at a time. Allocate capacity in multiples of
        4 bytes; the *valid data* length may be any byte count.
      - Writer: check bit_writer_ok() (or watch bit_writer_flush()'s return) once at the
        end. Writes past capacity set a sticky overflow flag and are dropped.
      - Reader: reads past the end return 0 and set a sticky overflow flag -- callers batch
        reads and check bit_reader_ok() once, no per-read error handling.

==============================================================================================*/

#include <string.h>    // memcpy (float punning)

#include "orb.h"
#include "base/bit.h"

/*==============================================================================================
    Types
==============================================================================================*/

typedef struct bit_writer_s
{
    u32* words;         // output buffer, whole-word granularity
    i32  word_cap;      // capacity in u32 words
    u64  scratch;       // staged bits, flushed a word at a time
    i32  scratch_bits;  // bits currently staged in scratch
    i32  word_index;    // next buffer slot to flush into
    i32  bits_written;  // total bits accepted so far
    bool overflow;      // sticky: a write ran past capacity and was dropped

} bit_writer_t;

typedef struct bit_reader_s
{
    const u32* words;      // input buffer, whole-word granularity
    i32        num_bits;   // total valid bits (valid bytes * 8)
    u64        scratch;    // staged bits not yet consumed
    i32        scratch_bits;
    i32        word_index;
    i32        bits_read;  // total bits consumed so far
    bool       overflow;   // sticky: a read ran past num_bits and returned 0

} bit_reader_t;

/*==============================================================================================
    Writer
==============================================================================================*/

/* `bytes` is the buffer capacity; only whole u32 words are used (bytes / 4 rounds down). */
ORB_INLINE void
bit_writer_init( bit_writer_t* w, void* buffer, i32 bytes )
{
    w->words        = ( u32* )buffer;
    w->word_cap     = bytes / 4;
    w->scratch      = 0;
    w->scratch_bits = 0;
    w->word_index   = 0;
    w->bits_written = 0;
    w->overflow     = false;
}

/* Append the low `bits` (1..32) of `value`. */
ORB_INLINE void
bit_write_bits( bit_writer_t* w, u32 value, i32 bits )
{
    if ( w->bits_written + bits > w->word_cap * 32 )
    {
        w->overflow = true;
        return;
    }

    u64 mask = ( ( u64 )1 << bits ) - 1;
    w->scratch |= ( ( u64 )value & mask ) << w->scratch_bits;
    w->scratch_bits += bits;
    w->bits_written += bits;

    if ( w->scratch_bits >= 32 )
    {
        w->words[ w->word_index++ ] = ( u32 )w->scratch;
        w->scratch >>= 32;
        w->scratch_bits -= 32;
    }
}

/* Zero-pad up to the next byte boundary so byte-block data starts memcpy-clean. */
ORB_INLINE void
bit_writer_align( bit_writer_t* w )
{
    i32 rem = w->bits_written & 7;
    if ( rem ) bit_write_bits( w, 0, 8 - rem );
}

/* Append a raw byte block (aligns to a byte boundary first). */
ORB_INLINE void
bit_write_bytes( bit_writer_t* w, const void* data, i32 size )
{
    const u8* bytes = ( const u8* )data;
    bit_writer_align( w );
    for ( i32 i = 0; i < size; i++ ) bit_write_bits( w, bytes[ i ], 8 );
}

/* Flush the staged partial word. Call once when the payload is complete.
   Returns the wire byte count, or -1 if any write overflowed. */
ORB_INLINE i32
bit_writer_flush( bit_writer_t* w )
{
    if ( w->overflow ) return -1;
    if ( w->scratch_bits > 0 )
    {
        w->words[ w->word_index++ ] = ( u32 )w->scratch;
        w->scratch      = 0;
        w->scratch_bits = 0;
    }
    return ( w->bits_written + 7 ) / 8;
}

ORB_INLINE bool
bit_writer_ok( const bit_writer_t* w )
{
    return !w->overflow;
}

/*==============================================================================================
    Reader
==============================================================================================*/

/* `bytes` is the valid data length. The buffer behind it must be readable at whole-word
   granularity (capacity rounded up to a multiple of 4). */
ORB_INLINE void
bit_reader_init( bit_reader_t* r, const void* buffer, i32 bytes )
{
    r->words        = ( const u32* )buffer;
    r->num_bits     = bytes * 8;
    r->scratch      = 0;
    r->scratch_bits = 0;
    r->word_index   = 0;
    r->bits_read    = 0;
    r->overflow     = false;
}

/* Consume `bits` (1..32). Returns 0 and sets the sticky overflow flag on a read past the end. */
ORB_INLINE u32
bit_read_bits( bit_reader_t* r, i32 bits )
{
    if ( r->bits_read + bits > r->num_bits )
    {
        r->overflow = true;
        return 0;
    }

    if ( r->scratch_bits < bits )
    {
        r->scratch |= ( u64 )r->words[ r->word_index++ ] << r->scratch_bits;
        r->scratch_bits += 32;
    }

    u64 mask   = ( ( u64 )1 << bits ) - 1;
    u32 result = ( u32 )( r->scratch & mask );
    r->scratch >>= bits;
    r->scratch_bits -= bits;
    r->bits_read += bits;
    return result;
}

/* Skip up to the next byte boundary (mirrors bit_writer_align). */
ORB_INLINE void
bit_reader_align( bit_reader_t* r )
{
    i32 rem = r->bits_read & 7;
    if ( rem ) bit_read_bits( r, 8 - rem );
}

/* Read a raw byte block written by bit_write_bytes (aligns first). */
ORB_INLINE void
bit_read_bytes( bit_reader_t* r, void* out, i32 size )
{
    u8* bytes = ( u8* )out;
    bit_reader_align( r );
    for ( i32 i = 0; i < size; i++ ) bytes[ i ] = ( u8 )bit_read_bits( r, 8 );
}

ORB_INLINE bool
bit_reader_ok( const bit_reader_t* r )
{
    return !r->overflow;
}

/*==============================================================================================
    Typed helpers
==============================================================================================*/

ORB_INLINE void bit_write_bool( bit_writer_t* w, bool v ) { bit_write_bits( w, v ? 1 : 0, 1 ); }
ORB_INLINE void bit_write_u8( bit_writer_t* w, u8 v )     { bit_write_bits( w, v, 8 ); }
ORB_INLINE void bit_write_u16( bit_writer_t* w, u16 v )   { bit_write_bits( w, v, 16 ); }
ORB_INLINE void bit_write_u32( bit_writer_t* w, u32 v )   { bit_write_bits( w, v, 32 ); }

ORB_INLINE bool bit_read_bool( bit_reader_t* r ) { return bit_read_bits( r, 1 ) != 0; }
ORB_INLINE u8   bit_read_u8( bit_reader_t* r )   { return ( u8 )bit_read_bits( r, 8 ); }
ORB_INLINE u16  bit_read_u16( bit_reader_t* r )  { return ( u16 )bit_read_bits( r, 16 ); }
ORB_INLINE u32  bit_read_u32( bit_reader_t* r )  { return bit_read_bits( r, 32 ); }

ORB_INLINE void
bit_write_u64( bit_writer_t* w, u64 v )
{
    bit_write_bits( w, ( u32 )v, 32 );
    bit_write_bits( w, ( u32 )( v >> 32 ), 32 );
}

ORB_INLINE u64
bit_read_u64( bit_reader_t* r )
{
    u64 lo = bit_read_bits( r, 32 );
    u64 hi = bit_read_bits( r, 32 );
    return lo | ( hi << 32 );
}

ORB_INLINE void
bit_write_f32( bit_writer_t* w, f32 v )
{
    u32 u;
    memcpy( &u, &v, 4 );
    bit_write_bits( w, u, 32 );
}

ORB_INLINE f32
bit_read_f32( bit_reader_t* r )
{
    u32 u = bit_read_bits( r, 32 );
    f32 v;
    memcpy( &v, &u, 4 );
    return v;
}

ORB_INLINE void
bit_write_f64( bit_writer_t* w, f64 v )
{
    u64 u;
    memcpy( &u, &v, 8 );
    bit_write_u64( w, u );
}

ORB_INLINE f64
bit_read_f64( bit_reader_t* r )
{
    u64 u = bit_read_u64( r );
    f64 v;
    memcpy( &v, &u, 8 );
    return v;
}

/*==============================================================================================
    Variable-length integers -- 7-bit groups, small values cost one byte
==============================================================================================*/

ORB_INLINE void
bit_write_varint_u32( bit_writer_t* w, u32 v )
{
    for ( ;; )
    {
        u32 group = v & 0x7F;
        v >>= 7;
        bit_write_bits( w, group | ( v ? 0x80 : 0 ), 8 );
        if ( !v ) return;
    }
}

ORB_INLINE u32
bit_read_varint_u32( bit_reader_t* r )
{
    u32 result = 0;
    i32 shift  = 0;
    for ( i32 i = 0; i < 5; i++ )
    {
        u32 group = bit_read_bits( r, 8 );
        result |= ( group & 0x7F ) << shift;
        if ( !( group & 0x80 ) ) return result;
        shift += 7;
    }
    r->overflow = true;    /* malformed: too many continuation groups */
    return 0;
}

ORB_INLINE void
bit_write_varint_u64( bit_writer_t* w, u64 v )
{
    for ( ;; )
    {
        u32 group = ( u32 )( v & 0x7F );
        v >>= 7;
        bit_write_bits( w, group | ( v ? 0x80 : 0 ), 8 );
        if ( !v ) return;
    }
}

ORB_INLINE u64
bit_read_varint_u64( bit_reader_t* r )
{
    u64 result = 0;
    i32 shift  = 0;
    for ( i32 i = 0; i < 10; i++ )
    {
        u64 group = bit_read_bits( r, 8 );
        result |= ( group & 0x7F ) << shift;
        if ( !( group & 0x80 ) ) return result;
        shift += 7;
    }
    r->overflow = true;    /* malformed: too many continuation groups */
    return 0;
}

/*==============================================================================================
    Quantized values -- range-bounded ints and fixed-resolution floats
==============================================================================================*/

/* Bits needed to encode any value in 0..range (0 when range is 0 -- nothing to send). */
ORB_INLINE i32
bit_bits_for_range( u32 range )
{
    return ( range == 0 ) ? 0 : 32 - bit_u32_clz( range );
}

/* Integer known to lie in [min, max]; costs bit_bits_for_range( max - min ) bits. */
ORB_INLINE void
bit_write_int_range( bit_writer_t* w, i32 v, i32 min, i32 max )
{
    i32 bits = bit_bits_for_range( ( u32 )( max - min ) );
    if ( bits == 0 ) return;
    if ( v < min ) v = min;
    if ( v > max ) v = max;
    bit_write_bits( w, ( u32 )( v - min ), bits );
}

ORB_INLINE i32
bit_read_int_range( bit_reader_t* r, i32 min, i32 max )
{
    i32 bits = bit_bits_for_range( ( u32 )( max - min ) );
    if ( bits == 0 ) return min;
    return min + ( i32 )bit_read_bits( r, bits );
}

/* Float quantized to `resolution` steps across [min, max]; round-trip error is at most
   resolution / 2. E.g. an angle at 1/256 degree: bit_write_float_q( w, a, 0, 360, 1.0f / 256 ). */
ORB_INLINE void
bit_write_float_q( bit_writer_t* w, f32 v, f32 min, f32 max, f32 resolution )
{
    u32 steps = ( u32 )( ( max - min ) / resolution + 0.5f );
    i32 bits  = bit_bits_for_range( steps );
    if ( bits == 0 ) return;

    f32 q = ( v - min ) / resolution + 0.5f;
    u32 u = ( q <= 0.0f ) ? 0 : ( u32 )q;
    if ( u > steps ) u = steps;
    bit_write_bits( w, u, bits );
}

ORB_INLINE f32
bit_read_float_q( bit_reader_t* r, f32 min, f32 max, f32 resolution )
{
    u32 steps = ( u32 )( ( max - min ) / resolution + 0.5f );
    i32 bits  = bit_bits_for_range( steps );
    if ( bits == 0 ) return min;
    return min + ( f32 )bit_read_bits( r, bits ) * resolution;
}

/*============================================================================================*/
#endif    // NET_BIT_H
