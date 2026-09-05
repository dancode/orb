/*==============================================================================================

    runtime_service/gui/font/gui_font_load.c -- the .orb_font parser (metrics + pixels).

    Parses a baked .orb_font into a registry slot: the type metrics, the per-glyph advance
    /placement table, and the raw R8 glyph bitmap.  All CPU: no atlas, no GPU.  The bitmap is
    stored in the slot (slot->pixels) and the slot flagged needs_upload; the render side later
    reads those pixels and packs them into the shared atlas (draw/gui_glyph.c).  .orb_font is a
    proportional font baked offline by font_tool: an R8 atlas of packed glyph bitmaps plus
    per-glyph records (UV rect, bearing, advance).

    The parser works over BYTES in memory.  The by-name loaders read a bake through the fs
    mounts (gui_res.h, name + ".orb_font"); the _mem loaders take bytes the caller holds -- the
    runtime baker's output, a test fixture -- and use the name only as the slot's identity.

    Compiled into the gui_font.c resource unit; included after font/gui_font_core.c.

==============================================================================================*/
// clang-format off

/* A read cursor over the bake's bytes.  Every stage pulls fixed-size records through font_rd and
   fails on a short read, so a truncated file dies at parse rather than at first use. */
typedef struct
{
    const u8* p;
    const u8* end;

} font_rd_t;

static bool
font_rd( font_rd_t* r, void* out, size_t n )
{
    if ( (size_t)( r->end - r->p ) < n )
        return false;
    memcpy( out, r->p, n );
    r->p += n;
    return true;
}

/*==============================================================================================
    Stage 1 -- the header.

    The glyph record layout is not byte-compatible across versions (orb_font.h), so this loader
    requires exactly ORB_FONT_VERSION -- an older file's records would misread at a different
    record size, not just underfill a header tail.

    Dimensions are BOUNDED, not just nonzero: atlas_w * atlas_h sizes a malloc and the memory
    accounting, and an unchecked product wraps u32 (65536 x 65536 = 0) -- a corrupt file must die
    here at parse, not survive to a zero-byte buffer with 4-billion-pixel metrics.  Width is the
    baker's own page contract (ORB_FONT_PAGE_MAX_W_SDF, the wider destination); height has no
    format cap, so 4096 stands in as "beyond any atlas this could ever land in".  Metrics must give
    a positive glyph box and line advance -- layout divides by both.
==============================================================================================*/

static bool
font_header_read( font_rd_t* r, orb_font_header_t* hdr )
{
    memset( hdr, 0, sizeof( *hdr ) );
    if ( !font_rd( r, hdr, sizeof( *hdr ) )
         || hdr->magic   != ORB_FONT_MAGIC
         || hdr->version  != ORB_FONT_VERSION
         || hdr->glyph_count == 0 || hdr->glyph_count > ORB_FONT_MAX_GLYPHS
         || hdr->atlas_w == 0     || hdr->atlas_w > ORB_FONT_PAGE_MAX_W_SDF
         || hdr->atlas_h == 0     || hdr->atlas_h > 4096u
         || hdr->ascent <= hdr->descent
         || hdr->ascent - hdr->descent + hdr->line_gap <= 0 )
        return false;

    /* The spread lives in the same baked-page space as atlas_w -- it cannot outreach the page that
       holds it -- so that bound also caps font_slot_t.sdf_range (u16) from truncating a corrupt
       file's value.  The reference section is bounded by the format (res_ref_head_ok) and must
       sit right after this header, before it sizes anything. */
    return hdr->sdf_range <= ORB_FONT_PAGE_MAX_W_SDF
        && res_ref_head_ok( (const res_ref_head_t*)hdr )
        && hdr->ref_offset == sizeof( *hdr );
}

/*==============================================================================================
    Stage 1b -- the reference section, and the file's exact length.

    Between the header and the glyph records sit hdr->ref_size bytes of resource names
    (orb_font.h, engine/res/res_ref.h).  A font names nothing, so the section is empty in every
    file baked today and this loader only steps over it.  It is also where a sequential reader
    is easiest to mislead: glyph records read from the wrong offset can pass their per-record
    checks by luck.  So the file's length is checked against everything the header claims --
    header, references, records, pixels -- before any record is trusted.
==============================================================================================*/

static bool
font_refs_skip( font_rd_t* r, const u8* file_start, const orb_font_header_t* hdr )
{
    u64 refs   = hdr->ref_size;
    u64 expect = (u64)sizeof( *hdr ) + refs
               + (u64)hdr->glyph_count * sizeof( orb_font_glyph_t )
               + (u64)hdr->atlas_w * hdr->atlas_h;

    if ( (u64)( r->end - file_start ) != expect )
        return false;
    if ( (u64)( r->end - r->p ) < refs )
        return false;
    r->p += refs;
    return true;
}

/*==============================================================================================
    Stage 2 -- the glyph table, split into the two lookup tiers.

    ASCII lands in the dense `lookup` (indexed cp-32, the fast path), everything else -- a
    font_tool -range bake -- goes to a fresh *ext_out, kept sorted by codepoint for binary search
    (font_slot_cp).  A bake writes ranges in ascending codepoint order so the insertion sort below
    is normally a straight append; it exists so a hand-assembled file still resolves correctly.
    *ext_out is NULL for an ASCII-only font, and nothing is left allocated on failure.
==============================================================================================*/

static bool
font_glyphs_read( font_rd_t* r, const orb_font_header_t* hdr, orb_font_glyph_t* lookup,
                  orb_font_glyph_t** ext_out, u32* ext_count_out )
{
    memset( lookup, 0, ORB_FONT_CP_COUNT * sizeof( *lookup ) );
    *ext_out       = NULL;
    *ext_count_out = 0;

    /* Sized by glyph_count -- a -range bake may carry NO ASCII records at all, so that is the only
       safe bound.  Freed below if nothing extended actually landed. */
    orb_font_glyph_t* ext = (orb_font_glyph_t*)malloc( hdr->glyph_count * sizeof( orb_font_glyph_t ) );
    u32               ext_count = 0;
    if ( !ext )
        return false;

    for ( u32 i = 0; i < hdr->glyph_count; ++i )
    {
        orb_font_glyph_t g;

        /* A glyph rect outside the page would sample other tenants' pixels once the page is a
           tenant of the shared atlas -- reject the file, same as any other corruption. */
        bool ok = font_rd( r, &g, sizeof( g ) )
               && (u32)g.atlas_x + g.w <= hdr->atlas_w
               && (u32)g.atlas_y + g.h <= hdr->atlas_h;
        if ( !ok )
        {
            free( ext );
            return false;
        }

        if ( g.codepoint >= ORB_FONT_CP_FIRST && g.codepoint <= ORB_FONT_CP_LAST )
        {
            lookup[ g.codepoint - ORB_FONT_CP_FIRST ] = g;
        }
        else if ( g.codepoint > ORB_FONT_CP_LAST )   /* below-space records are dead weight */
        {
            u32 at = ext_count;
            while ( at > 0 && ext[ at - 1 ].codepoint > g.codepoint ) { ext[ at ] = ext[ at - 1 ]; --at; }
            ext[ at ] = g;
            ++ext_count;
        }
    }

    if ( ext_count == 0 ) { free( ext ); ext = NULL; }
    *ext_out       = ext;
    *ext_count_out = ext_count;
    return true;
}

/*==============================================================================================
    font_slot_load -- parse a .orb_font's bytes into `slot`.  Does not activate the slot.

    Four stages, each all-or-nothing: header, reference section + length check, glyph table,
    glyph pixels.  On success the slot holds resolved metrics, the advance/placement table, and
    its resident R8 glyph pixels (slot->pixels), and is flagged needs_upload.  A failed load at
    ANY stage leaves the slot's previous contents intact and leaks nothing.  `name` is the
    resource name the bake was requested under, kept as the slot's identity for debug readouts.
==============================================================================================*/

static bool
font_slot_load( font_slot_t* slot, const void* data, u32 size, const char* name )
{
    if ( !data || size == 0 )
        return false;
    if ( !name )
        name = "";

    font_rd_t r = { (const u8*)data, (const u8*)data + size };

    orb_font_header_t hdr;
    orb_font_glyph_t  lookup[ ORB_FONT_CP_COUNT ];
    orb_font_glyph_t* ext       = NULL;
    u32               ext_count = 0;

    if ( !font_header_read( &r, &hdr ) || !font_refs_skip( &r, (const u8*)data, &hdr )
         || !font_glyphs_read( &r, &hdr, lookup, &ext, &ext_count ) )
        return false;

    /* Copy the baked atlas into a fresh CPU buffer.  v3 fonts are pure glyph coverage; a legacy
       v2 font also carries a blank bottom band, which just rides along as dead space inside this
       font's packed rect (assists are atlas-level now).  hdr.atlas_h is the packed rect height. */

    u32  pixel_count = hdr.atlas_w * hdr.atlas_h;
    u8*  pixels      = (u8*)malloc( pixel_count );
    bool pixels_ok   = pixels && font_rd( &r, pixels, pixel_count );

    if ( !pixels_ok )
    {
        free( pixels );
        free( ext );
        return false;
    }

    /* Commit into the slot.  The resident bitmap replaces any prior one (a live re-bake); the render
       side re-uploads it into the atlas at its next frame_begin sync (needs_upload).  A loaded
       .orb_font has no integer upscale, so metrics are exact. */

    free( slot->pixels );
    slot->pixels    = pixels;
    slot->atlas_w   = (u16)hdr.atlas_w;    /* bounded by ORB_FONT_PAGE_MAX_W_SDF, font_header_read */
    slot->atlas_h   = (u16)hdr.atlas_h;    /* bounded by 4096, font_header_read */
    slot->sdf_range = (u16)hdr.sdf_range;  /* bounded by ORB_FONT_PAGE_MAX_W_SDF, font_header_read; 0 for every pre-v4 font */

    slot->ascent  = hdr.ascent;
    slot->descent = hdr.descent;
    memcpy( slot->lookup, lookup, sizeof( lookup ) );
    free( slot->ext );
    slot->ext       = ext;
    slot->ext_count = (u16)ext_count;      /* bounded by ORB_FONT_MAX_GLYPHS, font_header_read */

    slot->name_off      = font_name_intern( name );
    slot->used          = true;
    slot->needs_upload  = true;
    slot->upload_failed = false;   /* a fresh page deserves a fresh attempt (retry gate) */
    slot->metrics      = ( font_metrics_t ){
        .char_h = (f32)( hdr.ascent - hdr.descent ),
        .line_h = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
        .size   = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
    };

    /* One call per message: a sink that frames per message cannot reassemble a split line. */
    if ( slot->sdf_range )
        gui_log( GUI_LOG_INFO, "loaded font '%s' (char_h=%.1f line_h=%.1f, sdf spread %u px)",
                 name, slot->metrics.char_h, slot->metrics.line_h, slot->sdf_range );
    else
        gui_log( GUI_LOG_INFO, "loaded font '%s' (char_h=%.1f line_h=%.1f)",
                 name, slot->metrics.char_h, slot->metrics.line_h );

    return true;
}

/*==============================================================================================
    Public load API -- make a font resident by id.  Pure resource work: parse + store; no atlas.
    Metrics are ready on return; the pixels reach the GPU at the render side's next frame_begin.
==============================================================================================*/

/* Parse a bake's bytes into a new id and activate it.  Returns the id, or 0 on failure (registry
   full, or the bytes failed to parse). */
u32
font_load_mem( const void* data, u32 size, const char* name )
{
    u32 id = font_alloc_slot();
    if ( id == 0 )
    {
        gui_log( GUI_LOG_WARN, "font registry full (%u slots) -- load of '%s' rejected",
                 GUI_FONT_REGISTRY_MAX, name ? name : "" );
        return 0;
    }

    if ( !font_slot_load( font_slot_ptr( id ), data, size, name ) )
        return 0;

    font_activate( id );
    return id;
}

/* Read `name` + ".orb_font" through the fs mounts and load it into a new id.  A name no mount
   serves is reported: a public caller asked for a specific bake and should learn it is not
   there.  (The resolver probes the mounts itself before falling back, so its misses stay quiet.) */
u32
font_load( const char* name )
{
    fs_blob_t b = gui_res_read( name, ".orb_font" );
    if ( !b.ok )
    {
        gui_log( GUI_LOG_WARN, "font '%s': no %s.orb_font in the content mounts (build_tool -content cooks it)",
                 name ? name : "", name ? name : "" );
        return 0;
    }
    u32 id = font_load_mem( b.data, b.size, name );
    fs()->free( &b );
    return id;
}

/* Parse a bake's bytes into an existing id (id 0 swaps the default).  Returns false on a bad id
   or a failed parse; a failed parse leaves the slot's previous font intact.  Re-loading the active
   id refreshes its metrics in place -- the pixels swap in the atlas at the next frame_begin sync. */
bool
font_load_into_mem( u32 id, const void* data, u32 size, const char* name )
{
    font_slot_t* slot = font_slot_ptr( id );
    if ( !slot )
        return false;

    if ( !font_slot_load( slot, data, size, name ) )
        return false;

    /* Aim the active slot at `id` when it is already the selected one -- this is what first
       ACTIVATES slot 0, whose id is the zero-initialized default before anything selects a font. */
    if ( font_active_id() == id )
        font_activate( id );
    return true;
}

bool
font_load_into( u32 id, const char* name )
{
    fs_blob_t b = gui_res_read( name, ".orb_font" );
    if ( !b.ok )
    {
        gui_log( GUI_LOG_WARN, "font '%s': no %s.orb_font in the content mounts (build_tool -content cooks it)",
                 name ? name : "", name ? name : "" );
        return false;
    }
    bool ok = font_load_into_mem( id, b.data, b.size, name );
    fs()->free( &b );
    return ok;
}

// clang-format on
/*============================================================================================*/
