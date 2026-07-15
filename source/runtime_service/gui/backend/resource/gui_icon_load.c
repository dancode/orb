/*==============================================================================================

    runtime_service/gui/backend/resource/gui_icon_load.c -- Icon pixel SOURCING from disk.

    gui_icon.c (included just before this file) owns icon bookkeeping and packing but is
    deliberately blind to where the coverage bytes come from -- callers hand it raw R8.  This file
    is one such caller: it decodes an image file (PNG and the other stb_image formats) into R8
    coverage and feeds it straight to icon_register, so a loaded icon is indistinguishable from a
    procedurally-filled one downstream (same tenant, same shared-atlas UVs, same batching).

    PNG is the expected authoring format for icons: lossless and carries an alpha channel, which is
    exactly the coverage an R8 icon wants.  Coverage is taken from alpha when the source has it, and
    from luminance otherwise, so both alpha-on-transparent art and plain greyscale masks work.

    This is the icon analogue of gui_font_internal.c's .orb_font loader -- same raw stdio read, same
    "decode then res_atlas_add (via icon_register)" shape -- and, like it, depends on no higher
    service (no asset/fs layer): the gui backend owns its own resource loading.

    stb_image's implementation is compiled here.  It is marked STB_IMAGE_STATIC so its symbols stay
    private to this (gui backend) translation unit -- the asset service compiles its own external
    copy (asset_image.c), and a host that links both must not see the two collide.

    Included by gui_backend.c after resource/gui_icon.c (needs icon_register) and after
    resource/gui_font.c (which pulled in sys_host.h for sys_exe_dir).

==============================================================================================*/
// clang-format off

#include <stdlib.h>                 // malloc / free
#include "engine/sys/sys_host.h"    // sys_exe_dir -- built-in icons resolve engine-relative

/* stb_image: memory-only decode (no fopen path of its own); STATIC so the symbols do not clash
   with the asset service's external copy when a host links both libraries. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "vendor/stb_image.h"

/*----------------------------------------------------------------------------------------------
    icon_read_file -- slurp an entire file into a fresh malloc'd buffer (caller frees).  Raw stdio,
    matching font_slot_load; the gui backend does its own resource I/O and takes no fs dependency.
----------------------------------------------------------------------------------------------*/

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

/*----------------------------------------------------------------------------------------------
    icon_load_file -- decode an image file to R8 coverage and register it as an icon.

    Returns the new icon id, or GUI_ICON_NONE if the file is missing, undecodable, or the icon
    atlas is full.  A missing file is a quiet failure (the caller decides whether that matters);
    a present-but-broken file logs, since it signals a real asset problem.
----------------------------------------------------------------------------------------------*/

gui_icon_id_t
icon_load_file( const char* name, const char* path )
{
    if ( !name || !path )
        return GUI_ICON_NONE;

    u32 size = 0;
    u8* file = icon_read_file( path, &size );
    if ( !file )
        return GUI_ICON_NONE;   // missing / unreadable -- quiet

    /* Decode to RGBA8 (req_comp = 4); `comp` reports the SOURCE channel layout so we know whether
       the file actually carried an alpha channel or is greyscale/RGB. */
    int      w = 0, h = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory( file, (int)size, &w, &h, &comp, STBI_rgb_alpha );
    free( file );
    if ( !rgba )
    {
        printf( "[gui] icon '%s' decode failed ('%s'): %s\n",
                name, path, stbi_failure_reason() );
        return GUI_ICON_NONE;
    }

    /* Reduce RGBA -> R8 coverage.  Sources with alpha (RGBA / grey+alpha) use the alpha channel --
       the natural coverage for icon art drawn on transparency.  Sources without alpha (RGB / grey)
       fall back to luminance, which for an expanded greyscale mask is just the red channel. */
    bool has_alpha = ( comp == 4 || comp == 2 );
    u32  count     = (u32)w * (u32)h;
    u8*  coverage  = (u8*)malloc( count );
    if ( !coverage )
    {
        stbi_image_free( rgba );
        return GUI_ICON_NONE;
    }
    for ( u32 i = 0; i < count; ++i )
        coverage[ i ] = has_alpha ? rgba[ i * 4 + 3 ] : rgba[ i * 4 + 0 ];
    stbi_image_free( rgba );

    gui_icon_id_t id = icon_register( name, (u32)w, (u32)h, coverage );
    free( coverage );
    return id;
}

/*----------------------------------------------------------------------------------------------
    Built-in icon set -- declared upfront here and loaded in one pass at backend init.

    Add engine icons by dropping a PNG under <engine>/assets/icons and naming it here; look one up
    at draw time with gui()->find_icon( "<name>" ).  Paths are engine-relative and resolved like the
    built-in fonts (sys_exe_dir + "/../"), so hosts work from any working directory.  A host or the
    editor can register its OWN icons on top of these at runtime via gui()->load_icon.
----------------------------------------------------------------------------------------------*/

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
    char dir[ 512 ];
    sys_exe_dir( dir, (int)sizeof( dir ) );

    u32 loaded = 0;
    for ( u32 i = 0; i < ARRAY_COUNT( s_builtin_icons ); ++i )
    {
        /* Prefer the engine-relative location (one level above the exe dir, matching fonts);
           fall back to a plain CWD-relative path for relocated layouts. */
        char path[ 576 ];
        snprintf( path, sizeof( path ), "%s/../%s", dir, s_builtin_icons[ i ].path );

        gui_icon_id_t id = icon_load_file( s_builtin_icons[ i ].name, path );
        if ( id == GUI_ICON_NONE )
            id = icon_load_file( s_builtin_icons[ i ].name, s_builtin_icons[ i ].path );
        if ( id != GUI_ICON_NONE )
            ++loaded;
    }

    if ( loaded > 0 )
        printf( "[gui] loaded %u/%u built-in icons\n",
                loaded, (u32)ARRAY_COUNT( s_builtin_icons ) );
}

// clang-format on
/*============================================================================================*/
