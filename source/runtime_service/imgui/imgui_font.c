/*==============================================================================================

    runtime_service/imgui/imgui_font.c -- Font management and per-glyph dispatch.

    Manages two font sources and dispatches transparently between them:
        Builtin   -- the two hardcoded bitmap atlases from imgui_font_builtin.c.
                     Always available; bitmap_font_select() picks 8x8 or 8x12.
        TrueType  -- an optional .orb_font loaded at runtime via tt_font_load().
                     When loaded it takes priority; tt_font_unload() reverts to bitmap.

    Both sources resolve their metrics into font_metrics_t (defined in imgui_font_builtin.c)
    and point s_font at the active one.  All dispatch helpers read from s_font; callers
    never inspect which source is active.

    Glyph lookup for the TrueType path:
        lookup[] indexed by (codepoint - 32); entries with advance == 0 are missing glyphs.

    Included by imgui.c after imgui_font_builtin.c.

==============================================================================================*/
// clang-format off

#include "tools/font_tool/orb_font.h"

/*----------------------------------------------------------------------------------------------
    tt_font_t -- runtime state for one loaded TrueType font (.orb_font file)
----------------------------------------------------------------------------------------------*/

typedef struct
{
    font_metrics_t    metrics;          // first: common resolved values (char_h, char_w, etc.)

    i32               ascent;           // pixels above baseline (positive)
    i32               descent;          // pixels below baseline (negative)
    u32               atlas_w;          // width of the font atlas in pixels
    u32               atlas_h;          // height of the font atlas in pixels
    rhi_texture_t     atlas;            // GPU texture handle for the atlas
    u32               atlas_idx;        // bindless index for the atlas texture (0 if not registered)
    bool              active;           // true while a font is loaded and in use.

    orb_font_glyph_t  lookup[ 95 ];     // codepoints 32..126

} tt_font_t;

static tt_font_t s_tt_font;

/*----------------------------------------------------------------------------------------------
    tt_font_unload / tt_font_load
----------------------------------------------------------------------------------------------*/

static void
tt_font_unload( void )
{
    if ( !s_tt_font.active )
        return;

    if ( s_tt_font.atlas_idx != 0 )
    {
        rhi()->unregister_texture( s_tt_font.atlas_idx );
        s_tt_font.atlas_idx = 0;
    }
    if ( rhi_handle_valid( s_tt_font.atlas ) )
    {
        rhi()->texture_destroy( s_tt_font.atlas );
        s_tt_font.atlas = ( rhi_texture_t ){ 0 };
    }
    s_tt_font.active = false;
    s_font = &s_bitmap_active->metrics;
}

static bool
tt_font_load( const char* path )
{
    tt_font_unload();

    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;

    /* Validate header. */
    orb_font_header_t hdr;
    if ( fread( &hdr, sizeof( hdr ), 1, f ) != 1
         || hdr.magic   != ORB_FONT_MAGIC
         || hdr.version != ORB_FONT_VERSION
         || hdr.glyph_count == 0 || hdr.glyph_count > 256
         || hdr.atlas_w == 0     || hdr.atlas_h == 0 )
    {
        fclose( f );
        return false;
    }

    /* Build lookup table from glyph records. */
    memset( s_tt_font.lookup, 0, sizeof( s_tt_font.lookup ) );
    for ( u32 i = 0; i < hdr.glyph_count; ++i )
    {
        orb_font_glyph_t g;
        if ( fread( &g, sizeof( g ), 1, f ) != 1 ) { fclose( f ); return false; }
        if ( g.codepoint >= 32 && g.codepoint <= 126 )
            s_tt_font.lookup[ g.codepoint - 32 ] = g;
    }

    /* Read pixel data and upload to GPU. */
    u32 pixel_count = hdr.atlas_w * hdr.atlas_h;
    u8* pixels      = (u8*)malloc( pixel_count );
    if ( !pixels ) { fclose( f ); return false; }

    if ( fread( pixels, 1, pixel_count, f ) != pixel_count )
    {
        free( pixels );
        fclose( f );
        return false;
    }
    fclose( f );

    /* create the render texture and upload the atlas pixels */

    s_tt_font.atlas = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = hdr.atlas_w,
        .height       = hdr.atlas_h,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_R8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = "imgui_tt_font",
    } );
    if ( !rhi_handle_valid( s_tt_font.atlas ) ) { free( pixels ); return false; }

    if ( !rhi()->upload_texture( s_tt_font.atlas, pixels, pixel_count, 0, 0 ) )
    {
        free( pixels );
        rhi()->texture_destroy( s_tt_font.atlas );
        s_tt_font.atlas = ( rhi_texture_t ){ 0 };
        return false;
    }
    free( pixels );

    s_tt_font.atlas_idx = rhi()->register_texture( s_tt_font.atlas );
    if ( s_tt_font.atlas_idx == 0 )
    {
        rhi()->texture_destroy( s_tt_font.atlas );
        s_tt_font.atlas = ( rhi_texture_t ){ 0 };
        return false;
    }

    /* Resolve common metrics and activate. */

    s_tt_font.ascent  = hdr.ascent;
    s_tt_font.descent = hdr.descent;
    s_tt_font.atlas_w = hdr.atlas_w;
    s_tt_font.atlas_h = hdr.atlas_h;
    s_tt_font.active  = true;

    s_tt_font.metrics = ( font_metrics_t ){
        .char_h       = (f32)( hdr.ascent - hdr.descent ),
        .line_h       = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
        .char_w       = 0.0f,
        .atlas_idx    = s_tt_font.atlas_idx,
        .proportional = true,
    };
    s_font = &s_tt_font.metrics;
    return true;
}

/*----------------------------------------------------------------------------------------------
    font_init / font_shutdown
----------------------------------------------------------------------------------------------*/

static bool
font_init( void )
{
    if ( !bitmap_atlas_init( &s_bitmap_8 ) )
        return false;
    if ( !bitmap_atlas_init( &s_bitmap_12 ) )
    {
        bitmap_atlas_shutdown( &s_bitmap_8 );
        return false;
    }
    bitmap_font_select( s_layout.font_size );
    return true;
}

static void
font_shutdown( void )
{
    tt_font_unload();
    bitmap_atlas_shutdown( &s_bitmap_12 );
    bitmap_atlas_shutdown( &s_bitmap_8 );
    s_bitmap_active = NULL;
    s_font          = NULL;
}

/*----------------------------------------------------------------------------------------------
    font_char_w / font_char_h / font_line_h / font_text_w / font_atlas_idx

    Dispatch helpers -- all read from s_font, set by bitmap_font_select() or tt_font_load().
----------------------------------------------------------------------------------------------*/

static f32 font_char_w( void )    { return s_font->char_w;    }
static f32 font_char_h( void )    { return s_font->char_h;    }
static f32 font_line_h( void )    { return s_font->line_h;    }
static u32 font_atlas_idx( void ) { return s_font->atlas_idx; }

static f32
font_text_w( const char* str )
{
    f32 w = 0.0f;
    if ( s_font->proportional )
    {
        for ( ; *str; ++str )
        {
            u8 ch = (u8)*str;
            if ( ch >= 32 && ch <= 126 )
                w += (f32)s_tt_font.lookup[ ch - 32 ].advance;
        }
    }
    else
    {
        for ( ; *str; ++str )
            w += s_font->char_w;
    }
    return w;
}

/*----------------------------------------------------------------------------------------------
    font_glyph -- per-character draw parameters; dispatches between TrueType and bitmap paths.

    Outputs:
        u0..v1   atlas UV rect for the glyph bitmap
        ox, oy   pixel offsets from (cursor_x, text_y) to the top-left of the bitmap
        gw, gh   pixel dimensions of the bitmap to draw (0 for invisible glyphs like space)
        advance  horizontal cursor advance in pixels
----------------------------------------------------------------------------------------------*/

static void
font_glyph( u8 ch,
            f32* u0, f32* v0, f32* u1, f32* v1,
            f32* ox, f32* oy, f32* gw, f32* gh,
            f32* advance )
{
    if ( s_font->proportional )
    {
        if ( ch < 32 || ch > 126 ) ch = (u8)'?';
        const orb_font_glyph_t* g = &s_tt_font.lookup[ ch - 32 ];

        f32 iw = 1.0f / (f32)s_tt_font.atlas_w;
        f32 ih = 1.0f / (f32)s_tt_font.atlas_h;
        *u0 = (f32)g->atlas_x * iw;
        *v0 = (f32)g->atlas_y * ih;
        *u1 = *u0 + (f32)g->w * iw;
        *v1 = *v0 + (f32)g->h * ih;

        *ox      = (f32)g->bearing_x;
        *oy      = (f32)( s_tt_font.ascent - (i32)g->bearing_y );
        *gw      = (f32)g->w;
        *gh      = (f32)g->h;
        *advance = (f32)g->advance;
    }
    else
    {
        /* Bitmap path: fixed-grid UV, full-cell draw, monospace advance. */
        if ( ch < 32 || ch > 127 ) ch = (u8)'?';
        const bitmap_font_def_t* def = s_bitmap_active->def;

        u32 idx = (u32)( ch - 32 );
        u32 col = idx % def->glyphs_row;
        u32 row = idx / def->glyphs_row;

        *u0 = (f32)( col * def->glyph_w ) / (f32)def->atlas_w;
        *v0 = (f32)( row * def->glyph_h ) / (f32)def->atlas_h;
        *u1 = *u0 + (f32)def->glyph_w     / (f32)def->atlas_w;
        *v1 = *v0 + (f32)def->glyph_h     / (f32)def->atlas_h;

        *ox      = 0.0f;
        *oy      = 0.0f;
        *gw      = s_font->char_w;
        *gh      = s_font->char_h;
        *advance = s_font->char_w;
    }
}

// clang-format on
/*============================================================================================*/
