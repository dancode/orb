/*==============================================================================================

    dev_image.c -- Developer 2D image utility library.

    Unity build entry for the dev_image static library.  Compiles stb_image (decode) and
    stb_image_write (PNG encode) here, both marked STATIC so their symbols stay private to this
    TU -- the gui backend (gui_icon_load.c) and the asset service (asset_image.c) carry their
    own stb_image copies, and a host linking all three must not see them collide.  miniz was
    considered for the PNG writer but rejected: the vendored miniz.h hard-defines MINIZ_EXPORT
    with no static option, so a client linking dev_image next to fs (which compiles its own
    miniz for the ZIP reader) would hit duplicate symbols.

    File reads go through a raw stdio slurp + stbi_load_from_memory (matching gui_icon_load.c);
    stb_image_write keeps its own stdio path for output.  sys is used only for directory
    creation in dev_image_split_sheet.

==============================================================================================*/

// clang-format off

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "orb.h"

PUSH_WARNINGS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "vendor/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"
POP_WARNINGS

#include "engine/sys/sys_host.h"
#include "developer/dev_image/dev_image.h"

#define DEV_IMAGE_PATH_MAX 512

/*==============================================================================================
    Error reporting -- one static buffer, printf-style setter, mirrored from dev_font.
==============================================================================================*/

static char g_error[ 512 ];

static bool
err( const char* fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( g_error, sizeof( g_error ), fmt, args );
    va_end( args );
    return false;
}

const char*
dev_image_last_error( void )
{
    return g_error;
}

/*==============================================================================================
    image_read_file -- slurp an entire file into a fresh malloc'd buffer (caller frees).
==============================================================================================*/

static u8*
image_read_file( const char* path, u32* out_size )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return NULL;

    fseek( f, 0, SEEK_END );
    long len = ftell( f );
    fseek( f, 0, SEEK_SET );
    if ( len <= 0 )
    {
        fclose( f );
        return NULL;
    }

    u8* buf = (u8*)malloc( (size_t)len );
    if ( !buf )
    {
        fclose( f );
        return NULL;
    }

    if ( fread( buf, 1, (size_t)len, f ) != (size_t)len )
    {
        free( buf );
        fclose( f );
        return NULL;
    }
    fclose( f );

    *out_size = (u32)len;
    return buf;
}

/*==============================================================================================
    Load / free / write
==============================================================================================*/

bool
dev_image_load( const char* path, dev_image_t* out )
{
    memset( out, 0, sizeof( *out ) );
    if ( !path )
        return err( "dev_image_load: NULL path" );

    u32 size = 0;
    u8* file = image_read_file( path, &size );
    if ( !file )
        return err( "cannot read '%s'", path );

    /* Decode to RGBA8 (req_comp = 4).  stb folds every transparency spelling -- real alpha
       channel, or RGB/paletted tRNS color-key -- into the decoded alpha channel. */
    int      w = 0, h = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory( file, (int)size, &w, &h, &comp, STBI_rgb_alpha );
    free( file );
    if ( !rgba )
        return err( "cannot decode '%s': %s", path, stbi_failure_reason() );

    out->pixels = rgba;   /* stbi buffers are plain malloc blocks; dev_image_free uses free() */
    out->w      = w;
    out->h      = h;
    return true;
}

void
dev_image_free( dev_image_t* img )
{
    if ( img && img->pixels )
        free( img->pixels );
    if ( img )
        memset( img, 0, sizeof( *img ) );
}

bool
dev_image_write_png( const char* path, const dev_image_t* img )
{
    if ( !path || !img || !img->pixels || img->w <= 0 || img->h <= 0 )
        return err( "dev_image_write_png: invalid arguments" );

    if ( !stbi_write_png( path, img->w, img->h, 4, img->pixels, img->w * 4 ) )
        return err( "cannot write '%s'", path );
    return true;
}

/*==============================================================================================
    Pixel operations
==============================================================================================*/

bool
dev_image_crop( const dev_image_t* src, int x, int y, int w, int h, dev_image_t* out )
{
    memset( out, 0, sizeof( *out ) );
    if ( !src || !src->pixels )
        return err( "dev_image_crop: NULL source" );
    if ( w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > src->w || y + h > src->h )
        return err( "dev_image_crop: rect %d,%d %dx%d outside source %dx%d",
                    x, y, w, h, src->w, src->h );

    u8* pixels = (u8*)malloc( (size_t)w * (size_t)h * 4 );
    if ( !pixels )
        return err( "dev_image_crop: out of memory (%dx%d)", w, h );

    for ( int row = 0; row < h; ++row )
        memcpy( pixels + (size_t)row * (size_t)w * 4,
                src->pixels + ( (size_t)( y + row ) * (size_t)src->w + (size_t)x ) * 4,
                (size_t)w * 4 );

    out->pixels = pixels;
    out->w      = w;
    out->h      = h;
    return true;
}

bool
dev_image_has_alpha( const dev_image_t* img )
{
    if ( !img || !img->pixels )
        return false;

    u32 count = (u32)img->w * (u32)img->h;
    for ( u32 i = 0; i < count; ++i )
        if ( img->pixels[ i * 4 + 3 ] != 255 )
            return true;
    return false;
}

void
dev_image_key_luma( dev_image_t* img )
{
    if ( !img || !img->pixels )
        return;

    u32 count = (u32)img->w * (u32)img->h;
    for ( u32 i = 0; i < count; ++i )
    {
        u8* px = &img->pixels[ i * 4 ];
        u8  c  = px[ 0 ];                   // luminance = brightest colour channel
        if ( px[ 1 ] > c ) c = px[ 1 ];
        if ( px[ 2 ] > c ) c = px[ 2 ];
        px[ 3 ] = c;
    }
}

/*==============================================================================================
    dev_image_split_sheet -- grid-cut a sprite sheet into per-cell PNGs.
==============================================================================================*/

/* Basename of path minus extension, e.g. "a/b/sheet.png" -> "sheet". */
static void
path_stem( const char* path, char* out, int out_size )
{
    const char* base = path;
    for ( const char* p = path; *p; ++p )
        if ( *p == '/' || *p == '\\' )
            base = p + 1;

    const char* dot = strrchr( base, '.' );
    int         len = dot && dot > base ? (int)( dot - base ) : (int)strlen( base );
    if ( len > out_size - 1 )
        len = out_size - 1;
    memcpy( out, base, (size_t)len );
    out[ len ] = '\0';
}

int
dev_image_split_sheet( const char* sheet_path, int cols, int rows, const char* out_dir,
                       u32 flags )
{
    if ( !sheet_path || !out_dir )
    {
        err( "dev_image_split_sheet: NULL path" );
        return -1;
    }
    if ( cols <= 0 || rows <= 0 )
    {
        err( "grid must be positive, got %dx%d", cols, rows );
        return -1;
    }

    dev_image_t sheet;
    if ( !dev_image_load( sheet_path, &sheet ) )
        return -1;

    /* An uneven division means the grid count is wrong for this sheet; truncating would shear
       every cell off its true boundary, so refuse rather than emit misaligned crops. */
    if ( sheet.w % cols != 0 || sheet.h % rows != 0 )
    {
        err( "sheet %dx%d does not divide evenly into a %dx%d grid",
             sheet.w, sheet.h, cols, rows );
        dev_image_free( &sheet );
        return -1;
    }
    int cell_w = sheet.w / cols;
    int cell_h = sheet.h / rows;

    if ( flags & DEV_IMAGE_SPLIT_KEY_LUMA )
        dev_image_key_luma( &sheet );

    if ( !sys_dir_make( out_dir ) )
    {
        err( "cannot create output directory '%s'", out_dir );
        dev_image_free( &sheet );
        return -1;
    }

    char stem[ 128 ];
    path_stem( sheet_path, stem, (int)sizeof( stem ) );

    int written = 0;
    for ( int r = 0; r < rows; ++r )
    {
        for ( int c = 0; c < cols; ++c )
        {
            dev_image_t cell;
            if ( !dev_image_crop( &sheet, c * cell_w, r * cell_h, cell_w, cell_h, &cell ) )
            {
                dev_image_free( &sheet );
                return -1;
            }

            char out_path[ DEV_IMAGE_PATH_MAX ];
            snprintf( out_path, sizeof( out_path ), "%s/%s_r%02d_c%02d.png",
                      out_dir, stem, r, c );

            bool ok = dev_image_write_png( out_path, &cell );
            dev_image_free( &cell );
            if ( !ok )
            {
                dev_image_free( &sheet );
                return -1;
            }
            ++written;
        }
    }

    dev_image_free( &sheet );
    return written;
}

// clang-format on
/*============================================================================================*/
