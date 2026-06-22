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

    Included by imgui_backend.c after imgui_font_builtin.c.

==============================================================================================*/
// clang-format off

#include "tools/font_tool/orb_font.h" /* font file formats and on-disk structure */

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

void
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

bool
tt_font_load( const char* path )
{
    tt_font_unload();

    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;

    /* Validate orb font format header */

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

    /* Read pixel data and upload to GPU.  The atlas is staged taller than the file: one
       appended row is filled opaque (0xFF) as the white texel for solid-color draws, then
       IMGUI_DASH_PATTERN_COUNT stipple rows for dashed lines.  Solid fills and dashes sample
       this atlas and merge with text.  Glyph V coords divide by the padded height (atlas_h
       below), so the appended rows never bleed into the bottom glyph row. */
    u32 tex_h       = hdr.atlas_h + 1u + IMGUI_DASH_PATTERN_COUNT;
    u32 glyph_bytes = hdr.atlas_w * hdr.atlas_h;
    u32 pixel_count = hdr.atlas_w * tex_h;
    u8* pixels      = (u8*)malloc( pixel_count );
    if ( !pixels ) { fclose( f ); return false; }

    if ( fread( pixels, 1, glyph_bytes, f ) != glyph_bytes )
    {
        free( pixels );
        fclose( f );
        return false;
    }
    fclose( f );

    /* White texel strip: fill the first appended row opaque, then the dash pattern rows. */
    memset( &pixels[ glyph_bytes ], 0xFF, hdr.atlas_w );
    font_paint_dash_rows( pixels, hdr.atlas_w, hdr.atlas_h + 1u );

    /* create the render texture and upload the atlas pixels */

    s_tt_font.atlas = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = hdr.atlas_w,
        .height       = tex_h,
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
    s_tt_font.atlas_h = tex_h;          /* padded: glyph UV math divides by the uploaded height */
    s_tt_font.active  = true;

    s_tt_font.metrics = ( font_metrics_t ){
        .char_h       = (f32)( hdr.ascent - hdr.descent ),
        .line_h       = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
        .char_w       = 0.0f,
        .size         = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
        .atlas_idx    = s_tt_font.atlas_idx,
        .proportional = true,
        /* White texel: center of the first appended row (pixel row hdr.atlas_h). */
        .white_u      = 0.5f / (f32)hdr.atlas_w,
        .white_v      = ( (f32)hdr.atlas_h + 0.5f ) / (f32)tex_h,
    };
    /* Dash pattern rows follow the white row at pixel row hdr.atlas_h + 1. */
    font_dash_row_v( s_tt_font.metrics.dash_v, hdr.atlas_h + 1u, tex_h );
    s_font = &s_tt_font.metrics;

    printf( "[imgui] loaded font '%s' (char_h=%.1f line_h=%.1f)\n",
            path, s_font->char_h, s_font->line_h );

    return true;
}

/*----------------------------------------------------------------------------------------------
    font_init / font_shutdown
----------------------------------------------------------------------------------------------*/

static void
font_shutdown( void )
{
    icon_atlas_shutdown();
    tt_font_unload();
    for ( u32 i = 0; i < IMGUI_FONT_BITMAP_MAX; ++i )
        bitmap_atlas_shutdown( &s_bitmaps[ i ] );
    s_bitmap_active = NULL;
    s_font          = NULL;
}

static bool
font_init( void )
{
    bitmap_show_sizes();

    /* bitmap_atlas_shutdown is safe on uninitialized fonts, so any failure here
       can just delegate to font_shutdown for a single cleanup path. */

    bool ok = true;
    for ( u32 i = 0; i < IMGUI_FONT_BITMAP_MAX; ++i )
        ok = ok && bitmap_atlas_init( &s_bitmaps[ i ] );
    if ( !ok ) { font_shutdown(); return false; }
    bitmap_font_select( IMGUI_FONT_BITMAP_12 );

    /* Runtime icon atlas shares the font lifecycle: created after rhi is up, torn down with fonts. */
    if ( !icon_atlas_init() ) { font_shutdown(); return false; }

    return true; /* built in fonts initialized successfully */
}

/*----------------------------------------------------------------------------------------------
    font_char_w / font_char_h / font_line_h / font_text_w / font_atlas_idx

    Dispatch helpers -- all read from s_font, set by bitmap_font_select() or tt_font_load().
----------------------------------------------------------------------------------------------*/

static f32 font_char_w      ( void ) { return s_font->char_w;    }
f32 font_char_h      ( void ) { return s_font->char_h;    }
f32 font_line_h      ( void ) { return s_font->line_h;    }
f32 font_em          ( void ) { return s_font->size;      }   // nominal type size (em) -- layout base
static u32 font_atlas_idx   ( void ) { return s_font->atlas_idx; }

/* Whether a TrueType font is currently active (vs. a built-in bitmap).  The UI unit's font API
   (imgui_set_bmp_scale) keys off this -- a bmp-scale change only re-derives layout when no TT
   font overrides the bitmap. */
bool font_is_tt( void ) { return s_tt_font.active; }

/* Log the active font (type, name, metrics) -- the font internals (s_bitmap_active->def) live in
   this unit, so the print does too; the UI unit's imgui_set_font just calls it. */
void
font_print_active( void )
{
    const bitmap_font_def_t* def = s_bitmap_active->def;
    printf( "[imgui] set font '%s : %s' (char_h=%.1f line_h=%.1f)\n",
            s_font->proportional ? "TrueType" : "Bitmap", def->debug_name, s_font->char_h, s_font->line_h );
}

/* Horizontal advance of one character in the active font.  Used by the text edit engine to
   measure glyph positions without emitting draw geometry (cursor placement, click-to-offset). */
f32
font_char_advance( u8 ch )
{
    if ( s_font->proportional )
    {
        if ( ch < 32 || ch > 126 ) ch = (u8)'?';
        return (f32)s_tt_font.lookup[ ch - 32 ].advance;
    }
    return s_font->char_w;
}

/* UV of the active atlas's white texel (appended bottom row) for solid-color draws. */
static void font_white_uv( f32* u, f32* v ) { *u = s_font->white_u; *v = s_font->white_v; }

/* Center V of the dash pattern row whose baked on-fraction is closest to `duty`.  Tessellated
   dashed lines sample this row, tiling it along the line via REPEAT addressing on U. */
static f32
font_dash_v( f32 duty )
{
    u32 best  = 0;
    f32 bestd = 1e30f;
    for ( u32 p = 0; p < IMGUI_DASH_PATTERN_COUNT; ++p )
    {
        f32 d = s_dash_duty[ p ] - duty;
        if ( d < 0.0f ) d = -d;
        if ( d < bestd ) { bestd = d; best = p; }
    }
    return s_font->dash_v[ best ];
}

/* Total bytes of GPU memory held by font atlas textures (R8_UNORM, 1 byte/pixel):
   every initialized bitmap atlas, plus the TrueType atlas when one is loaded. */
static u32
font_atlas_bytes( void )
{
    u32 bytes = 0;
    for ( u32 i = 0; i < IMGUI_FONT_BITMAP_MAX; ++i )
    {
        const bitmap_font_t* bf = &s_bitmaps[ i ];
        if ( rhi_handle_valid( bf->atlas ) )
            bytes += bf->def->atlas_w * bf->tex_h;   /* tex_h includes the white row */
    }
    if ( s_tt_font.active )
        bytes += s_tt_font.atlas_w * s_tt_font.atlas_h;
    return bytes;
}

/* Width of the first n bytes of str (stops early at a NUL).  Labels measure only their visible
   span this way -- the bytes before a "##" marker -- so reserved label space matches what draws. */
f32
font_text_w_n( const char* str, u32 n )
{
    f32 w = 0.0f;
    if ( s_font->proportional )
    {
        for ( u32 i = 0; i < n && str[ i ]; ++i )
        {
            u8 ch = (u8)str[ i ];
            if ( ch >= 32 && ch <= 126 )
                w += (f32)s_tt_font.lookup[ ch - 32 ].advance;
        }
    }
    else
    {
        for ( u32 i = 0; i < n && str[ i ]; ++i )
            w += s_font->char_w;
    }
    return w;
}

f32
font_text_w( const char* str )
{
    return font_text_w_n( str, 0xFFFFFFFFu );
}

/*----------------------------------------------------------------------------------------------
    font_glyph -- per-character draw parameters; dispatches between TrueType and bitmap paths.

    Outputs:
        u0..v1   atlas UV rect for the glyph bitmap
        ox, oy   pixel offsets from (cursor_x, text_y) to the top-left of the bitmap
        gw, gh   pixel dimensions of the bitmap to draw (0 for invisible glyphs like space)
        advance  horizontal cursor advance in pixels
----------------------------------------------------------------------------------------------*/

void
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

        /* V divides by the padded upload height (tex_h), not the glyph-grid height,
           so the appended white row stays outside every glyph's UV range. */
        f32 tex_h = (f32)s_bitmap_active->tex_h;
        *u0 = (f32)( col * def->glyph_w ) / (f32)def->atlas_w;
        *v0 = (f32)( row * def->glyph_h ) / tex_h;
        *u1 = *u0 + (f32)def->glyph_w     / (f32)def->atlas_w;
        *v1 = *v0 + (f32)def->glyph_h     / tex_h;

        *ox      = 0.0f;
        *oy      = 0.0f;
        *gw      = s_font->char_w;
        *gh      = s_font->char_h;
        *advance = s_font->char_w;
    }
}

// clang-format on
/*============================================================================================*/
