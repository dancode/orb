/*==============================================================================================

    runtime_service/gui/draw/gui_icon_load.c -- Icon pixel SOURCING from disk.

    gui_icon.c (included just before this file) owns icon bookkeeping and packing but is
    deliberately blind to where the coverage bytes come from -- callers hand it raw R8.  This file
    is one such caller: it decodes an image file (PNG and the other stb_image formats) into R8
    coverage and feeds it straight to icon_register, so a loaded icon is indistinguishable from a
    procedurally-filled one downstream (same tenant, same shared-atlas UVs, same batching).

    PNG is the expected authoring format for icons: lossless and carries an alpha channel, which is
    exactly the coverage an R8 icon wants.  Coverage is taken from alpha when the source has it, and
    from luminance otherwise, so both alpha-on-transparent art and plain greyscale masks work.

    This is the icon analogue of font/gui_font_load.c's .orb_font parser -- same raw stdio read, same
    "decode then res_atlas_add (via icon_register)" shape -- and, like it, depends on no higher
    service (no asset/fs layer): the gui backend owns its own resource loading.

    stb_image's implementation is compiled here.  It is marked STB_IMAGE_STATIC so its symbols stay
    private to this (gui backend) translation unit -- the asset service compiles its own external
    copy (asset_image.c), and a host that links both must not see the two collide.

    Included by gui_draw.c immediately after gui_icon.c, whose icon_register it feeds.

==============================================================================================*/
// clang-format off

#include <stdlib.h>   // malloc / free

#include "engine/sys/sys_host.h"   // sys_root_dir -- built-in icon assets resolve root-relative

/* stb_image: memory-only decode (no fopen path of its own); STATIC so the symbols do not clash
   with the asset service's external copy when a host links both libraries. */
PUSH_WARNINGS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "vendor/stb_image.h"
POP_WARNINGS

/*==============================================================================================
    icon_read_file -- slurp an entire file into a fresh malloc'd buffer (caller frees).  Raw stdio,
    matching font_slot_load; the gui backend does its own resource I/O and takes no fs dependency.
==============================================================================================*/

static u8*
icon_read_file( const char* path, u32* out_size )
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
    icon_load_file / icon_load_file_sdf -- decode an image file to R8 coverage and register it.

    Returns the new icon id, or GUI_ICON_NONE if the file is missing, undecodable, or the icon
    atlas is full.  A missing file is a quiet failure (the caller decides whether that matters);
    a present-but-broken file logs, since it signals a real asset problem.

    Two entry points over one decode.  The DECODE is identical -- both want the same R8 coverage
    out of the same PNG -- and only the registration forks, which is the honest shape: coverage is
    what an image file holds, and whether it is stored as coverage or transformed into a distance
    field is a decision about how it will be DRAWN, not about how it is read.

    Note the asymmetry the SDF path wants from its source art, though (gui_icon_sdf.c): it should
    be several times the size it will be displayed at, and it should carry a transparent margin.
    Neither is true of a good coverage icon, which wants to be authored at exactly its display size
    and can fill its bitmap to the edge.  The same PNG is rarely the best input to both.
==============================================================================================*/

static gui_icon_id_t
icon_load_file_impl( const char* name, const char* path, bool sdf, u32 out_max )
{
    if ( !name || !path )
        return GUI_ICON_NONE;

    u32 size = 0;
    u8* file = icon_read_file( path, &size );
    if ( !file )
        return GUI_ICON_NONE;   // missing / unreadable -- quiet

    /* Decode to RGBA8 (req_comp = 4).  stb resolves transparency for us regardless of how the
       source spelled it -- a real alpha channel, or an RGB/paletted image with a tRNS color-key --
       into the decoded alpha channel, so we read alpha from the pixels rather than trusting the
       source component count (`comp`), which would report 3 for a tRNS-keyed RGB icon and miss it. */
    int      w = 0, h = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory( file, (int)size, &w, &h, &comp, STBI_rgb_alpha );
    free( file );
    if ( !rgba )
    {
        gui_log( GUI_LOG_WARN, "icon '%s' decode failed ('%s'): %s",
                 name, path, stbi_failure_reason() );
        return GUI_ICON_NONE;
    }

    u32 count = (u32)w * (u32)h;

    /* Pick the coverage channel.  An icon's shape lives in alpha when the image carries any
       transparency (a real alpha channel, or an RGB/paletted source whose tRNS key stb already
       folded into the decoded alpha); a fully-opaque image has no alpha to read, so its shape is
       the luminance of the colour instead.  Deciding from the decoded pixels -- not the source
       component count -- is what makes tRNS-keyed icons work: they report comp==3 yet do carry
       alpha here. */
    bool use_alpha = false;
    for ( u32 i = 0; i < count; ++i )
        if ( rgba[ i * 4 + 3 ] != 255 ) { use_alpha = true; break; }

    u8* coverage = (u8*)malloc( count );
    if ( !coverage )
    {
        stbi_image_free( rgba );
        return GUI_ICON_NONE;
    }

    u8 peak = 0;   // brightest coverage byte -- 0 means the icon is entirely blank
    for ( u32 i = 0; i < count; ++i )
    {
        const u8* px = &rgba[ i * 4 ];
        u8        c;
        if ( use_alpha )
        {
            c = px[ 3 ];                                  // alpha = coverage
        }
        else
        {
            c = px[ 0 ];                                  // luminance = brightest colour channel
            if ( px[ 1 ] > c ) c = px[ 1 ];
            if ( px[ 2 ] > c ) c = px[ 2 ];
        }
        coverage[ i ] = c;
        if ( c > peak ) peak = c;
    }
    stbi_image_free( rgba );

    /* A completely blank result is almost always a bad export (e.g. an all-transparent PNG, or a
       flat-colour image whose shape was meant to be in an alpha channel that is not there) rather
       than intended art -- say so, since it would otherwise register a valid-but-invisible icon. */
    if ( peak == 0 )
        gui_log( GUI_LOG_WARN, "icon '%s' has no visible pixels -- '%s' decoded fully %s (%dx%d)",
                 name, path, use_alpha ? "transparent" : "black", w, h );

    gui_icon_id_t id = sdf ? icon_register_sdf( name, (u32)w, (u32)h, coverage, out_max )
                           : icon_register    ( name, (u32)w, (u32)h, coverage );
    free( coverage );
    return id;
}

gui_icon_id_t
icon_load_file( const char* name, const char* path )
{
    return icon_load_file_impl( name, path, false, 0 );
}

gui_icon_id_t
icon_load_file_sdf( const char* name, const char* path, u32 out_max )
{
    return icon_load_file_impl( name, path, true, out_max );
}

/*==============================================================================================
    Built-in icon set -- declared upfront here and loaded in one pass at backend init.

    Add engine icons by dropping a PNG under <root>/assets/icons and naming it here; look one up
    at draw time with gui()->find_icon( "<name>" ).  Paths are root-relative, resolved against
    sys_root_dir() like the built-in fonts, so hosts work from any working directory.  A host or the
    editor can register its OWN icons on top of these at runtime via gui()->load_icon.
==============================================================================================*/

typedef struct
{
    const char* name;   // find_icon lookup key
    const char* path;   // engine-relative source image

} gui_icon_decl_t;

static const gui_icon_decl_t s_builtin_icons[] =
{
    { "save",   "assets/icons/save.png"   },
    { "folder", "assets/icons/folder.png" },
    { "file",   "assets/icons/file.png"   },
    { "gear",   "assets/icons/gear.png"   },
    { "grid",   "assets/icons/grid.png"   },
    { "wire",   "assets/icons/wire.png"   },
    { "view",   "assets/icons/view.png"   },
};

void
icon_load_builtins( void )
{
    u32 loaded = 0;
    for ( u32 i = 0; i < ARRAY_COUNT( s_builtin_icons ); ++i )
    {
        char path[ 576 ];
        fmt_snprintf( path, sizeof( path ), "%s/%s", sys_root_dir(), s_builtin_icons[ i ].path );

        gui_icon_id_t id = icon_load_file( s_builtin_icons[ i ].name, path );
        if ( id != GUI_ICON_NONE )
            ++loaded;
    }

    if ( loaded > 0 )
        gui_log( GUI_LOG_INFO, "loaded %u/%u built-in icons",
                 loaded, (u32)ARRAY_COUNT( s_builtin_icons ) );
}

// clang-format on
/*============================================================================================*/
