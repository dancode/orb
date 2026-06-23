/*==============================================================================================

    runtime_service/imgui/backend/imgui_font_ttf.c -- ttf fonts: proportional .orb_font atlases.

    A "ttf font" is a proportional font baked into an .orb_font file by font_tool: an R8 atlas of
    packed glyph bitmaps plus per-glyph records (UV rect, bearing, advance).  Unlike a bmp font it
    is variable-width, so glyph dispatch reads a lookup[] table rather than a fixed grid.

    ttf_load_file() reads the file, expands the atlas tail via font_finalize_atlas() (the same white
    texel + dash rows every font carries), creates an owned GPU atlas, and fills a registry slot.
    The slot owns its atlas and releases it on reload / free (font_slot_free_gpu).

    Included by imgui_backend.c after imgui_font.h, before imgui_font.c.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    ttf_load_file -- load a .orb_font from disk into `slot`.  Does not activate the slot.

    On success the slot owns a freshly created GPU atlas and metrics describe a proportional font;
    any atlas the slot previously owned is released only after the new one is fully built, so a
    failed load leaves the slot's previous font intact.
----------------------------------------------------------------------------------------------*/

bool
ttf_load_file( font_slot_t* slot, const char* path )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;

    /* Validate orb font format header. */

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

    /* Build the lookup table from glyph records. */

    orb_font_glyph_t lookup[ 95 ];
    memset( lookup, 0, sizeof( lookup ) );
    for ( u32 i = 0; i < hdr.glyph_count; ++i )
    {
        orb_font_glyph_t g;
        if ( fread( &g, sizeof( g ), 1, f ) != 1 ) { fclose( f ); return false; }
        if ( g.codepoint >= 32 && g.codepoint <= 126 )
            lookup[ g.codepoint - 32 ] = g;
    }

    /* Read the glyph pixels into a staging buffer sized for the appended white + dash rows. */

    u32 tex_h       = font_atlas_tex_h( hdr.atlas_h );
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

    /* Build the metrics scale-free fields here; the scale-dependent fields below are exact (a ttf
       has no integer upscale). */
    font_metrics_t metrics = ( font_metrics_t ){
        .char_h       = (f32)( hdr.ascent - hdr.descent ),
        .line_h       = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
        .char_w       = 0.0f,
        .size         = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
        .proportional = true,
    };

    /* Append the white texel + dash rows and resolve white/dash/UV-scale metrics. */
    font_finalize_atlas( pixels, hdr.atlas_w, hdr.atlas_h, tex_h, &metrics );

    /* Create the render texture and upload the atlas pixels. */

    rhi_texture_t atlas = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = hdr.atlas_w,
        .height       = tex_h,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_R8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = "imgui_ttf_font",
    } );
    if ( !rhi_handle_valid( atlas ) ) { free( pixels ); return false; }

    if ( !rhi()->upload_texture( atlas, pixels, pixel_count, 0, 0 ) )
    {
        free( pixels );
        rhi()->texture_destroy( atlas );
        return false;
    }
    free( pixels );

    u32 atlas_idx = rhi()->register_texture( atlas );
    if ( atlas_idx == 0 )
    {
        rhi()->texture_destroy( atlas );
        return false;
    }

    /* All GPU work succeeded -- commit into the slot.  Release any atlas it held only now, so a
       failed load above leaves the previous font intact. */

    font_slot_free_gpu( slot );

    slot->src        = FONT_SRC_TTF;
    slot->ttf.ascent  = hdr.ascent;
    slot->ttf.descent = hdr.descent;
    memcpy( slot->ttf.lookup, lookup, sizeof( lookup ) );

    slot->atlas      = atlas;
    slot->atlas_idx  = atlas_idx;
    slot->atlas_w    = hdr.atlas_w;
    slot->atlas_h    = tex_h;            /* padded: glyph UV math divides by the uploaded height */
    slot->owns_atlas = true;
    slot->used       = true;

    metrics.atlas_idx = atlas_idx;
    slot->metrics     = metrics;

    printf( "[imgui] loaded ttf font '%s' (char_h=%.1f line_h=%.1f)\n",
            path, slot->metrics.char_h, slot->metrics.line_h );

    return true;
}

/*----------------------------------------------------------------------------------------------
    ttf_char_advance / ttf_glyph -- per-glyph metrics and draw parameters for a ttf slot.
----------------------------------------------------------------------------------------------*/

f32
ttf_char_advance( const font_slot_t* slot, u8 ch )
{
    if ( ch < 32 || ch > 126 ) ch = (u8)'?';
    return (f32)slot->ttf.lookup[ ch - 32 ].advance;
}

void
ttf_glyph( const font_slot_t* slot, u8 ch,
           f32* u0, f32* v0, f32* u1, f32* v1,
           f32* ox, f32* oy, f32* gw, f32* gh, f32* advance )
{
    if ( ch < 32 || ch > 126 ) ch = (u8)'?';
    const orb_font_glyph_t* g = &slot->ttf.lookup[ ch - 32 ];
    const font_metrics_t*   m = &slot->metrics;

    f32 iw = m->inv_atlas_w;            /* precomputed at load -- no per-glyph divide */
    f32 ih = m->inv_atlas_h;
    *u0 = (f32)g->atlas_x * iw;
    *v0 = (f32)g->atlas_y * ih;
    *u1 = *u0 + (f32)g->w * iw;
    *v1 = *v0 + (f32)g->h * ih;

    *ox      = (f32)g->bearing_x;
    *oy      = (f32)( slot->ttf.ascent - (i32)g->bearing_y );
    *gw      = (f32)g->w;
    *gh      = (f32)g->h;
    *advance = (f32)g->advance;
}

// clang-format on
/*============================================================================================*/
