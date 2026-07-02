/*==============================================================================================

    runtime_service/gui/backend/gui_load_font_orb.c -- the .orb_font loader.

    .orb_font is a proportional font baked offline by font_tool: an R8 atlas of packed glyph
    bitmaps plus per-glyph records (UV rect, bearing, advance).  Glyph dispatch reads a lookup[]
    table since advances are variable-width, not a fixed grid.  It is currently the only font
    source format gui loads (the bitmap-font path was removed), so this file has no sibling
    format to distinguish itself from -- the name just says what it reads.

    font_slot_load() reads the file, expands the atlas tail via font_finalize_atlas() (the same
    white texel + dash rows every font carries), creates the slot's owned GPU atlas via
    gui_atlas_create (gui_load_atlas.h), and fills the registry slot.  The slot owns its atlas and
    releases it on reload / free (font_slot_free_gpu).

    Included by gui_backend.c after gui_load_font.h, before gui_load_font.c.

    Nothing in this file is called outside the font unit: font_slot_load / font_slot_glyph /
    font_slot_char_advance are FILE-LOCAL (shared with gui_load_font.c only, declared in
    gui_load_font.h). There is no PUBLIC or BACKEND-INTERNAL surface here.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    font_slot_load -- load a .orb_font from disk into `slot`.  Does not activate the slot.

    On success the slot owns a freshly created GPU atlas and metrics describe a proportional font;
    any atlas the slot previously owned is released only after the new one is fully built, so a
    failed load leaves the slot's previous font intact.
----------------------------------------------------------------------------------------------*/

bool
font_slot_load( font_slot_t* slot, const char* path )
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

    /* Build the metrics scale-free fields here; the scale-dependent fields below are exact (a
       loaded .orb_font has no integer upscale). */
    font_metrics_t metrics = ( font_metrics_t ){
        .type = {
            .char_h = (f32)( hdr.ascent - hdr.descent ),
            .line_h = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
            .size   = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
        },
    };

    /* Append the white texel + dash rows and resolve white/dash/UV-scale metrics. */
    font_finalize_atlas( pixels, hdr.atlas_w, hdr.atlas_h, tex_h, &metrics );

    /* Reload swap-safety: when this slot already holds an atlas, in-flight frames may still be
       sampling it (and hold the bindless set bound).  Re-baking a font live -- e.g. the font
       browser dragging the size slider -- otherwise creates/destroys an atlas and rewrites its
       bindless slot every frame, and the deferred reclaim races the GPU under that churn,
       eventually faulting the device (VK_ERROR_DEVICE_LOST).  Drain the GPU first so the old
       atlas has no live readers before we build and register its replacement and tear it down.
       Reloads are rare and human-triggered, so the full stall is imperceptible. */
    if ( slot->used )
        rhi()->device_wait_idle();

    /* Create the render texture and upload the atlas pixels. */

    gui_atlas_t atlas;
    if ( !gui_atlas_create( &atlas, hdr.atlas_w, tex_h, pixels, "gui_font_atlas" ) )
    {
        free( pixels );
        return false;
    }
    free( pixels );

    /* All GPU work succeeded -- commit into the slot.  Release any atlas it held only now, so a
       failed load above leaves the previous font intact. */

    font_slot_free_gpu( slot );

    slot->ascent  = hdr.ascent;
    slot->descent = hdr.descent;
    memcpy( slot->lookup, lookup, sizeof( lookup ) );

    slot->atlas = atlas;               /* padded: glyph UV math divides by the uploaded height */
    slot->used  = true;

    metrics.atlas.atlas_idx = atlas.atlas_idx;
    slot->metrics           = metrics;

    printf( "[gui] loaded font '%s' (char_h=%.1f line_h=%.1f)\n",
            path, slot->metrics.type.char_h, slot->metrics.type.line_h );

    return true;
}

/*----------------------------------------------------------------------------------------------
    font_slot_char_advance / font_slot_glyph -- per-glyph metrics and draw parameters for a slot.
----------------------------------------------------------------------------------------------*/

f32
font_slot_char_advance( const font_slot_t* slot, u8 ch )
{
    if ( ch < 32 || ch > 126 ) ch = (u8)'?';
    return (f32)slot->lookup[ ch - 32 ].advance;
}

void
font_slot_glyph( const font_slot_t* slot, u8 ch,
                 f32* u0, f32* v0, f32* u1, f32* v1,
                 f32* ox, f32* oy, f32* gw, f32* gh, f32* advance )
{
    if ( ch < 32 || ch > 126 ) ch = (u8)'?';
    const orb_font_glyph_t* g = &slot->lookup[ ch - 32 ];
    const font_atlas_sample_t* m = &slot->metrics.atlas;

    f32 iw = m->inv_atlas_w;            /* precomputed at load -- no per-glyph divide */
    f32 ih = m->inv_atlas_h;
    *u0 = (f32)g->atlas_x * iw;
    *v0 = (f32)g->atlas_y * ih;
    *u1 = *u0 + (f32)g->w * iw;
    *v1 = *v0 + (f32)g->h * ih;

    *ox      = (f32)g->bearing_x;
    *oy      = (f32)( slot->ascent - (i32)g->bearing_y );
    *gw      = (f32)g->w;
    *gh      = (f32)g->h;
    *advance = (f32)g->advance;
}

// clang-format on
/*============================================================================================*/
