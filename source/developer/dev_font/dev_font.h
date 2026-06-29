#ifndef DEV_FONT_H
#define DEV_FONT_H
/*==============================================================================================

    dev_font.h -- Developer runtime font baker.

    Rasterizes a TTF/OTF font at a given pixel size using stb_truetype, packs the glyph
    bitmaps with stb_rect_pack, and writes the result to assets/font_cache/ as an .orb_font
    binary.  The format is identical to font_tool output, so the existing ttf_load_file()
    path in the GUI loads it without modification.

    Intended for development builds only.  In release, fonts are pre-baked by font_tool
    (FreeType quality) and shipped in assets/font/.

    Usage
    -----
        dev_font_init( NULL );     // auto-detect build root from exe location

        char path[ 512 ];
        if ( dev_font_get( "consola.ttf", 18, path, sizeof( path ) ) )
            gui()->load_font_file( my_id, path );

    Path resolution
    ---------------
    When ttf_path contains no directory separator it is searched in order:
        1. <build_root>/assets/font_source/
        2. C:\Windows\Fonts\           (Windows)
           /usr/share/fonts/truetype/  (Linux / macOS)
    Paths that already contain a separator are used as-is.

    Cache
    -----
    dev_font_get() writes assets/font_cache/<stem>_<size>px.orb_font.
    On a repeated call the cache file is returned immediately if its modification time
    is >= the source TTF's modification time -- no re-bake occurs.

==============================================================================================*/

#include "orb.h"

typedef struct
{
    const char* build_dir;  /* repo root; NULL = auto-detect from exe location */

} dev_font_settings_t;

/* Initialize.  Must be called once before dev_font_get().  Returns false on error. */
bool        dev_font_init( const dev_font_settings_t* settings );
void        dev_font_shutdown( void );

/* Locate or bake a font atlas.
   ttf_path      -- source TTF/OTF path or bare filename (searched as described above).
   size_px       -- glyph height in pixels (6..256).
   out_path      -- receives the absolute path to the .orb_font on success.
   out_path_size -- capacity of out_path in bytes.
   Returns true on success; call dev_font_last_error() for the failure reason. */
bool        dev_font_get( const char* ttf_path, int size_px,
                          char* out_path, int out_path_size );

const char* dev_font_last_error( void );

/*============================================================================================*/
#endif  /* DEV_FONT_H */
