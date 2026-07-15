/*==============================================================================================

    engine/pack/pack.c -- compression library implementation.

    Unity build entry for the pack static library.  Thin, allocation-honest wrappers over the
    vendored miniz amalgamation (declarations from vendor/miniz.h; definitions compiled in
    pack_miniz.c, pack's own TU).  The wrappers exist so miniz stays quarantined inside this
    library: callers see pack_* and the opaque handles in pack.h, never mz_* types.

    Handles are single malloc blocks around the miniz archive struct; readers borrow the
    caller's archive bytes (fs keeps the whole .zip in memory for the mount's lifetime),
    writers build into miniz's own heap and hand the finished blob to the caller.

==============================================================================================*/

// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"

#include "engine/pack/pack_miniz.h"  /* miniz config -- must precede vendor/miniz.h */
#include "vendor/miniz.h"            /* mz_* declarations (impl in pack_miniz.c) */

#include "engine/pack/pack_host.h"

/*==============================================================================================
    Buffer codec
==============================================================================================*/

u32
pack_bound( u32 src_size )
{
    return ( u32 )mz_compressBound( ( mz_ulong )src_size );
}

bool
pack_deflate( const void* src, u32 src_size, void* dst, u32* dst_size, int level )
{
    if ( !src || !dst || !dst_size )
        return false;

    if ( level < 0 )  level = 0;
    if ( level > 10 ) level = 10;

    mz_ulong out_len = ( mz_ulong )*dst_size;
    if ( mz_compress2( ( unsigned char* )dst, &out_len,
                       ( const unsigned char* )src, ( mz_ulong )src_size, level ) != MZ_OK )
        return false;

    *dst_size = ( u32 )out_len;
    return true;
}

bool
pack_inflate( const void* src, u32 src_size, void* dst, u32* dst_size )
{
    if ( !src || !dst || !dst_size )
        return false;

    mz_ulong out_len = ( mz_ulong )*dst_size;
    if ( mz_uncompress( ( unsigned char* )dst, &out_len,
                        ( const unsigned char* )src, ( mz_ulong )src_size ) != MZ_OK )
        return false;

    *dst_size = ( u32 )out_len;
    return true;
}

u32
pack_crc32( u32 crc, const void* data, u32 size )
{
    return ( u32 )mz_crc32( ( mz_ulong )crc, ( const unsigned char* )data, ( size_t )size );
}

/*==============================================================================================
    ZIP reading
==============================================================================================*/

struct pack_zip_s
{
    mz_zip_archive za;
};

pack_zip_t*
pack_zip_open( const void* data, u32 size )
{
    if ( !data || !size )
        return NULL;

    pack_zip_t* zip = ( pack_zip_t* )calloc( 1, sizeof( pack_zip_t ) );
    if ( !zip )
        return NULL;

    if ( !mz_zip_reader_init_mem( &zip->za, data, ( size_t )size, 0 ) )
    {
        free( zip );
        return NULL;
    }
    return zip;
}

void
pack_zip_close( pack_zip_t* zip )
{
    if ( !zip )
        return;
    mz_zip_reader_end( &zip->za );
    free( zip );
}

u32
pack_zip_count( pack_zip_t* zip )
{
    return zip ? ( u32 )mz_zip_reader_get_num_files( &zip->za ) : 0;
}

int
pack_zip_find( pack_zip_t* zip, const char* name )
{
    if ( !zip || !name || !name[ 0 ] )
        return -1;
    return mz_zip_reader_locate_file( &zip->za, name, NULL, 0 );
}

bool
pack_zip_stat( pack_zip_t* zip, int index, pack_zip_stat_t* out )
{
    if ( !zip || !out )
        return false;

    mz_zip_archive_file_stat st;
    if ( !mz_zip_reader_file_stat( &zip->za, ( mz_uint )index, &st ) )
        return false;

    snprintf( out->name, sizeof( out->name ), "%s", st.m_filename );
    out->size = ( u32 )st.m_uncomp_size;
    return true;
}

bool
pack_zip_extract( pack_zip_t* zip, int index, void* dst, u32 dst_size )
{
    if ( !zip || !dst )
        return false;
    return mz_zip_reader_extract_to_mem( &zip->za, ( mz_uint )index, dst,
                                         ( size_t )dst_size, 0 ) != MZ_FALSE;
}

/*==============================================================================================
    ZIP writing
==============================================================================================*/

struct pack_zip_writer_s
{
    mz_zip_archive za;
};

pack_zip_writer_t*
pack_zip_writer_begin( void )
{
    pack_zip_writer_t* w = ( pack_zip_writer_t* )calloc( 1, sizeof( pack_zip_writer_t ) );
    if ( !w )
        return NULL;

    if ( !mz_zip_writer_init_heap( &w->za, 0, 0 ) )
    {
        free( w );
        return NULL;
    }
    return w;
}

bool
pack_zip_writer_add( pack_zip_writer_t* w, const char* name, const void* data, u32 size,
                     int level )
{
    if ( !w || !name || !name[ 0 ] )
        return false;

    if ( level < 0 )  level = 0;
    if ( level > 10 ) level = 10;

    return mz_zip_writer_add_mem( &w->za, name, data, ( size_t )size,
                                  ( mz_uint )level ) != MZ_FALSE;
}

bool
pack_zip_writer_end( pack_zip_writer_t* w, void** out_data, u32* out_size )
{
    if ( out_data ) *out_data = NULL;
    if ( out_size ) *out_size = 0;
    if ( !w || !out_data || !out_size )
    {
        pack_zip_writer_abort( w );
        return false;
    }

    void*  buf = NULL;
    size_t sz  = 0;
    bool   ok  = mz_zip_writer_finalize_heap_archive( &w->za, &buf, &sz ) != MZ_FALSE;

    mz_zip_writer_end( &w->za );   /* releases writer state; the heap blob survives */
    free( w );

    if ( !ok )
    {
        free( buf );
        return false;
    }
    *out_data = buf;               /* miniz default allocator is malloc -- caller free()s */
    *out_size = ( u32 )sz;
    return true;
}

void
pack_zip_writer_abort( pack_zip_writer_t* w )
{
    if ( !w )
        return;
    mz_zip_writer_end( &w->za );
    free( w );
}

/*==============================================================================================
    Unity includes
==============================================================================================*/

#include "engine/pack/pack_api.c"   // API struct + module descriptor (needs the impls above)

// clang-format on
/*============================================================================================*/
