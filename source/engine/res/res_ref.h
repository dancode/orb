/*==============================================================================================

    engine/res/res_ref.h - The reference section of a cooked file.

    A cooked file (.orb_font, .tex, .oshd, ...) carries the NAMES of the resources its content
    refers to -- the textures a material samples, the meshes a level places -- in one section
    the packager can find and read without knowing the format:

        [ format header, opening with res_ref_head_t ][ reference section ][ payload ... ]

    Every cooked format's header BEGINS with the five fields of res_ref_head_t: the format's
    own magic and version, then ref_count, ref_size and ref_offset, which locate the section.
    The section is ref_count names, each NUL-terminated, packed in order, zero-padded to a
    multiple of RES_REF_ALIGN bytes; ref_size is that padded length, 0 when there are no
    names.  The section sits immediately after the format's fixed header, so ref_offset is
    that header's size -- every writer here does that and every loader checks it.

    The cooker writes the section, since only it understands the content.  The packager
    (developer/dev_ship, docs/CONTENT.md) reads res_ref_head_t off the front of any cooked file,
    validates the section (res_ref_locate) and walks the names (res_ref_next), so the walk
    from a target's manifest through its content to everything it drags in never parses a
    format.  Runtime loaders only step over the section to reach their payload.

    A reference is a RUNTIME dependency: something the content needs loaded beside it.  A cook
    INPUT -- the TTF a recipe bakes from -- is not a reference and is never written here.

    No cooker emits a reference yet: every file written today has ref_count 0 and ref_size 0.
    Header-only, so the format headers -- shared with tools that link base + sys only -- can
    include it.

==============================================================================================*/
#ifndef RES_REF_H
#define RES_REF_H

#include <stddef.h>
#include <string.h>

#include "engine/res/res.h"

// clang-format off

#define RES_REF_MAX           4096u                              // names per file; more is corrupt
#define RES_REF_ALIGN         8u                                 // section padding (u64 fields may follow)
#define RES_REF_SIZE_MAX      ( RES_REF_MAX * ( ( u32 )RES_NAME_MAX + 1u ) )   // 1 MiB; a multiple of 8

/* The five fields every cooked format's header opens with, in this order. */
typedef struct res_ref_head_s
{
    u32 magic;        // the format's own; never interpreted here
    u32 version;      // the format's own
    u32 ref_count;    // names in the reference section, <= RES_REF_MAX
    u32 ref_size;     // bytes of the section incl. padding: a multiple of RES_REF_ALIGN, 0 iff ref_count is 0
    u32 ref_offset;   // byte offset of the section from the start of the file: the fixed header's size

} res_ref_head_t;

/* Place in a format header's file to prove it opens with res_ref_head_t's fields. */
#define RES_REF_HEAD_ASSERT( T )                                                                   \
    _Static_assert( offsetof( T, magic ) == 0 && offsetof( T, version ) == 4                      \
                    && offsetof( T, ref_count ) == 8 && offsetof( T, ref_size ) == 12              \
                    && offsetof( T, ref_offset ) == 16,                                            \
                    #T " must open with the res_ref_head_t fields" )

/* Bounds on the head alone, before any size arithmetic.  A loader also checks that ref_offset
   equals its own header size; the packager checks the section lies inside the file. */
static inline bool
res_ref_head_ok( const res_ref_head_t* h )
{
    if ( h->ref_count > RES_REF_MAX || h->ref_size > RES_REF_SIZE_MAX )
        return false;
    if ( h->ref_size % RES_REF_ALIGN != 0 )
        return false;
    if ( ( h->ref_count == 0 ) != ( h->ref_size == 0 ) )
        return false;
    return h->ref_offset >= sizeof( res_ref_head_t );
}

/* The padded byte length of a section holding these names (0 for none).  False -- and
   *out_size 0 -- on a name res_name_ok refuses or a count past RES_REF_MAX. */
static inline bool
res_ref_measure( const char* const* names, u32 count, u32* out_size )
{
    *out_size = 0;
    if ( count > RES_REF_MAX )
        return false;
    u64 n = 0;
    for ( u32 i = 0; i < count; ++i )
    {
        if ( !res_name_ok( names[ i ] ) )
            return false;
        n += strlen( names[ i ] ) + 1;
    }
    n = ( n + RES_REF_ALIGN - 1 ) & ~( u64 )( RES_REF_ALIGN - 1 );
    if ( n > RES_REF_SIZE_MAX )
        return false;
    *out_size = ( u32 )n;
    return true;
}

/* Write the section into `out`: the names in order, NUL-terminated, zero padding up to the
   length res_ref_measure reports.  `cap` must hold that length. */
static inline bool
res_ref_write( u8* out, u32 cap, const char* const* names, u32 count )
{
    u32 size;
    if ( !res_ref_measure( names, count, &size ) || size > cap )
        return false;
    memset( out, 0, size );
    u32 o = 0;
    for ( u32 i = 0; i < count; ++i )
    {
        size_t n = strlen( names[ i ] ) + 1;
        memcpy( out + o, names[ i ], n );
        o += ( u32 )n;
    }
    return true;
}

/* Validate a section against its head: exactly `count` names, each NUL-terminated inside
   `size` and canonical, then nothing but zero padding shorter than one RES_REF_ALIGN.  So one
   name list has exactly one valid byte string. */
static inline bool
res_ref_section_ok( const u8* sec, u32 size, u32 count )
{
    u32 o = 0;
    for ( u32 i = 0; i < count; ++i )
    {
        u32 start = o;
        while ( o < size && sec[ o ] )
            ++o;
        if ( o >= size )
            return false;                                   /* unterminated */
        if ( !res_name_ok( ( const char* )sec + start ) )
            return false;                                   /* empty or non-canonical */
        ++o;
    }
    if ( size - o >= RES_REF_ALIGN )
        return false;                                       /* bytes no count admits to */
    for ( ; o < size; ++o )
        if ( sec[ o ] )
            return false;                                   /* padding must be zero */
    return true;
}

/* Iterate the names of a section res_ref_section_ok accepted.  *cursor starts at 0; each call
   returns the next name, NULL after the last. */
static inline const char*
res_ref_next( const u8* sec, u32 size, u32* cursor )
{
    if ( *cursor >= size || !sec[ *cursor ] )
        return NULL;
    const char* name = ( const char* )sec + *cursor;
    *cursor += ( u32 )strlen( name ) + 1;
    return name;
}

/* The packager's entry: locate and validate the reference section of a whole cooked file held
   in memory, without knowing its format.  On success the section is described (size 0 and
   count 0 for a file that names nothing); on failure the file is not a cooked file this
   contract covers, or is corrupt. */
static inline bool
res_ref_locate( const void* file, u64 file_size, const u8** out_sec, u32* out_size, u32* out_count )
{
    *out_sec   = NULL;
    *out_size  = 0;
    *out_count = 0;
    if ( !file || file_size < sizeof( res_ref_head_t ) )
        return false;

    res_ref_head_t h;
    memcpy( &h, file, sizeof( h ) );
    if ( !res_ref_head_ok( &h ) )
        return false;
    if ( ( u64 )h.ref_offset + h.ref_size > file_size )
        return false;

    const u8* sec = ( const u8* )file + h.ref_offset;
    if ( !res_ref_section_ok( sec, h.ref_size, h.ref_count ) )
        return false;

    *out_sec   = sec;
    *out_size  = h.ref_size;
    *out_count = h.ref_count;
    return true;
}

// clang-format on
/*============================================================================================*/
#endif    // RES_REF_H
