/*==============================================================================================

    runtime_service/gui/font/gui_font_load.c -- the .orb_font parser (metrics + pixels).

    Reads a baked .orb_font from disk into a registry slot: the type metrics, the per-glyph advance
    /placement table, and the raw R8 glyph bitmap.  All CPU: no atlas, no GPU.  The bitmap is stored
    in the slot (slot->pixels) and the slot flagged needs_upload; the render side later reads those
    pixels and packs them into the shared atlas (draw/gui_glyph.c).  .orb_font is a proportional font
    baked offline by font_tool: an R8 atlas of packed glyph bitmaps plus per-glyph records (UV rect,
    bearing, advance).

    Compiled into the gui_font.c resource unit; included after font/gui_font_core.c.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    font_slot_load -- parse a .orb_font from disk into `slot`.  Does not activate the slot.

    On success the slot holds resolved metrics, the advance/placement table, and its resident R8
    glyph pixels (slot->pixels), and is flagged needs_upload.  A failed load leaves the slot's
    previous contents intact.
==============================================================================================*/

static bool
font_slot_load( font_slot_t* slot, const char* path )
{
    FILE* f = fopen( path, "rb" );
    if ( !f )
        return false;

    /* Validate orb font format header.  v3 (pure glyph coverage) and v2 (legacy, trailing reserved
       band) are byte-compatible on disk -- the band, if present, just rides along as dead space in
       this font's packed rect -- so accept either. */

    orb_font_header_t hdr;
    if ( fread( &hdr, sizeof( hdr ), 1, f ) != 1
         || hdr.magic   != ORB_FONT_MAGIC
         || ( hdr.version != 3u && hdr.version != 2u )
         || hdr.glyph_count == 0 || hdr.glyph_count > 256
         || hdr.atlas_w == 0     || hdr.atlas_h == 0 )
    {
        fclose( f );
        return false;
    }

    /* Build the lookup table from glyph records. */

    orb_font_glyph_t lookup[ ORB_FONT_CP_COUNT ];
    memset( lookup, 0, sizeof( lookup ) );
    for ( u32 i = 0; i < hdr.glyph_count; ++i )
    {
        orb_font_glyph_t g;
        if ( fread( &g, sizeof( g ), 1, f ) != 1 ) { fclose( f ); return false; }
        if ( g.codepoint >= ORB_FONT_CP_FIRST && g.codepoint <= ORB_FONT_CP_LAST )
            lookup[ g.codepoint - ORB_FONT_CP_FIRST ] = g;
    }

    /* Read the full baked atlas into a fresh CPU buffer.  v3 fonts are pure glyph coverage; a legacy
       v2 font also carries a blank bottom band, which just rides along as dead space inside this
       font's packed rect (assists are atlas-level now).  hdr.atlas_h is the packed rect height. */

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

    /* Commit into the slot.  The resident bitmap replaces any prior one (a live re-bake); the render
       side re-uploads it into the atlas at its next frame_begin sync (needs_upload).  A loaded
       .orb_font has no integer upscale, so metrics are exact. */

    free( slot->pixels );
    slot->pixels  = pixels;
    slot->atlas_w = hdr.atlas_w;
    slot->atlas_h = hdr.atlas_h;

    slot->ascent  = hdr.ascent;
    slot->descent = hdr.descent;
    memcpy( slot->lookup, lookup, sizeof( lookup ) );

    slot->used         = true;
    slot->needs_upload = true;
    slot->metrics      = ( font_metrics_t ){
        .char_h = (f32)( hdr.ascent - hdr.descent ),
        .line_h = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
        .size   = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
    };

    printf( "[gui] loaded font '%s' (char_h=%.1f line_h=%.1f)\n",
            path, slot->metrics.char_h, slot->metrics.line_h );

    return true;
}

/*==============================================================================================
    Public load API -- make a font resident by id.  Pure resource work: parse + store; no atlas.
    Metrics are ready on return; the pixels reach the GPU at the render side's next frame_begin.
==============================================================================================*/

/* Parse a font into a new id and activate it.  Returns the id, or 0 on failure (registry full, or
   the file failed to load). */
u32
font_load( const char* path )
{
    u32 id = font_alloc_slot();
    if ( id == 0 )
        return 0;

    if ( !font_slot_load( font_slot_ptr( id ), path ) )
        return 0;

    font_activate( id );
    return id;
}

/* Parse a font into an existing id (id 0 swaps the default).  Returns false on a bad id or a failed
   load; a failed load leaves the slot's previous font intact.  Re-loading the active id refreshes
   its metrics in place -- the pixels swap in the atlas at the next frame_begin sync. */
bool
font_load_into( u32 id, const char* path )
{
    font_slot_t* slot = font_slot_ptr( id );
    if ( !slot )
        return false;

    if ( !font_slot_load( slot, path ) )
        return false;

    if ( font_active_id() == id )
        font_activate( id );            // metrics rebuilt in place; refresh active pointers
    return true;
}

// clang-format on
/*============================================================================================*/
