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
    ttf_path may be a bare filename ("CascadiaMono.ttf"), a friendly font name ("Cascadia Mono"),
    or a full path.  A request with no directory separator is searched in order:
        1. <build_root>/assets/font_source/   (exact name, then + .ttf / .otf / .ttc)
        2. C:\Windows\Fonts\           (Windows)  (exact filename, then + .ttf / .otf / .ttc)
           /usr/share/fonts/truetype/  (Linux / macOS)
        3. By friendly name -- the OS font registry (Windows: HKLM then HKCU), then a
           normalized-stem scan of font_source/ and the system font dir.  This resolves names
           that differ from the filename ("Consolas" -> consola.ttf, "Cascadia Mono" ->
           CascadiaMono.ttf) so fonts need not be copied into font_source/ first.
    Paths that already contain a separator are used as-is.

    Cache
    -----
    dev_font_get() writes assets/font_cache/<stem>_<size>px[<rangetag>].orb_font; fine
    (FreeType) bakes add an "_ft" tag before the extension.  On a repeated call the cache
    file is returned immediately if its modification time is >= the source TTF's
    modification time -- no re-bake occurs.

==============================================================================================*/

#include "orb.h"

typedef struct
{
    const char* build_dir;  /* repo root; NULL = auto-detect from exe location */

} dev_font_settings_t;

/* One codepoint span [lo, hi] a bake covers.  A -range spec parses into a sorted, merged
   list of these. */
typedef struct
{
    u32 lo, hi;

} dev_font_range_t;

#define DEV_FONT_RANGE_MAX 32   /* spans one spec may carry after merging */

/* Bake quality tier for dev_font_get_ex.  FAST is the stb_truetype bake this library performs
   itself -- milliseconds, good enough to work against.  FINE is a FreeType bake performed by
   spawning font_tool.exe (which must sit next to the calling exe in bin/); its output is cached
   with an "_ft" filename tag so the two tiers never overwrite each other. */
typedef enum
{
    DEV_FONT_FAST = 0,        // stb bake into assets/font_cache (dev_font_get's behavior)
    DEV_FONT_FINE_IF_CACHED,  // fresh fine cache file if one exists, else FAST
    DEV_FONT_FINE             // blocking font_tool.exe spawn (FreeType quality)

} dev_font_quality_t;

/* One rasterized glyph handed to dev_font_bake_write.  The front-end fills this: stb_truetype in
   dev_font's own baker, FreeType in font_tool.  bearing_y follows the orb_font convention
   (positive = above baseline); a front-end using a different sign converts before filling this. */
typedef struct
{
    u32  codepoint;
    u8*  bitmap;      // row-major coverage, w*h bytes; NULL for whitespace / empty glyphs
    int  w, h;        // bitmap dimensions in pixels
    int  bearing_x;   // cursor-to-left-edge offset, pixels
    int  bearing_y;   // baseline-to-top offset, pixels (positive = above baseline)
    int  advance;     // horizontal advance, pixels

} dev_font_glyph_t;

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

/* dev_font_get with a quality tier and a codepoint range.  range_spec is a -range spec string
   (see dev_font_range_parse); NULL or "" bakes the ASCII contract and keeps the untagged cache
   filename, so existing FAST caches stay valid.  FINE_IF_CACHED never blocks on font_tool: it
   returns the fine cache only when a fresh, header-valid file already exists (a torn file from
   a killed bake is rejected and ignored). */
bool        dev_font_get_ex( const char* ttf_path, int size_px, dev_font_quality_t quality,
                             const char* range_spec, char* out_path, int out_path_size );

/* dev_font_get_ex followed by a read of the bake it located: *out_data receives a malloc'd
   buffer of *out_size bytes the caller frees with free().  The shape gui's runtime font baker
   wants (gui_font_bake_fn takes bytes, never a path) -- a host adapter calls this and kicks a
   refine.  False, with nothing allocated, when the bake cannot be produced or read. */
bool        dev_font_get_bytes( const char* ttf_path, int size_px, dev_font_quality_t quality,
                                const char* range_spec, void** out_data, u32* out_size );

/* Fire-and-forget background FreeType refine of the fine ("_ft") cache file for this request.
   A worker thread spawns font_tool.exe; the caller keeps whatever bake it already has, and the
   next run's FINE_IF_CACHED picks the fine file up.  No-op when the fine cache is already fresh
   or a refine for this exact (request, size, range) is still in flight. */
void        dev_font_refine_kick( const char* ttf_path, int size_px, const char* range_spec );

/* Parse a range spec -- comma-separated preset names (ascii|latin1|latin|greek|cyrillic) and/or
   explicit LO[-HI] codepoint spans (strtoul base 0: hex or decimal) -- into sorted, merged
   spans.  Returns the span count, or 0 on a bad spec (dev_font_last_error set).  NULL/"" is a
   valid spec meaning the ASCII contract and yields that single span. */
int         dev_font_range_parse( const char* spec, dev_font_range_t* out, int cap );

/* Filename tag for a spec ("latin,greek" -> "_latin-greek"; unsafe chars map to '-'), so bakes
   of different ranges never overwrite each other.  NULL/"" writes "" (the untagged default). */
void        dev_font_range_suffix( const char* spec, char* out, int out_size );

/* Resolve a bare filename, a friendly font name ("Cascadia Mono"), or a path to an absolute
   TTF/OTF/TTC path that exists on disk, using the same search order as dev_font_get (see Path
   resolution).  No baking occurs.  Shared with font_tool so the offline (FreeType) baker accepts
   the same inputs as the runtime stb baker.  Requires dev_font_init(); returns false and sets
   dev_font_last_error() when the font cannot be found. */
bool        dev_font_resolve( const char* request, char* out_path, int out_path_size );

/* Shared bake back-end: pack `glyphs` into the smallest square atlas that fits and write an
   .orb_font to out_path.  Both bakers call this after rasterizing (dev_font via stb_truetype,
   font_tool via FreeType) so the packing heuristic and file layout live in one place.  The caller
   owns each glyph's bitmap and frees it after this returns.  `label` names the source in the log
   line.  Returns false and sets dev_font_last_error() on failure.

   sdf_range describes what the bytes in those bitmaps MEAN and goes straight to the header:
   0 = coverage (the only thing the stb front-end can produce), > 0 = a distance field with that
   spread in pixels.  Packing and layout are identical either way -- an SDF glyph is just a bigger
   rect with different numbers in it -- which is why this stayed one back-end. */
bool        dev_font_bake_write( const char* out_path, const dev_font_glyph_t* glyphs, u32 count,
                                 int ascent, int descent, int line_gap, int size_px,
                                 u32 sdf_range, const char* label );

const char* dev_font_last_error( void );

/* Absolute path of the assets/font/ directory (the raw TTF sources; where dev_font_get searches
   for bare filenames).  Returns false if dev_font_init() has not been called. */
bool        dev_font_source_dir( char* out_path, int out_path_size );

/*============================================================================================*/
#endif  /* DEV_FONT_H */
