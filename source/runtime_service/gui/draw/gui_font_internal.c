/*==============================================================================================

    runtime_service/gui/draw/gui_font_internal.c -- the .orb_font loader + glyph dispatch.

    The render-touching half of the font system: it parses a baked .orb_font, registers its glyph
    pixels into the shared resource atlas (gui_res_atlas.c), and fills a text-tier registry slot
    (metrics + advance table + tenant handle) through the text/ leaf's font_slot_ptr / font_activate.
    Glyph UV dispatch (font_slot_glyph) and the deferred-reload queue also live here.

    The loaded-font REGISTRY and the measurement readers are the text/ leaf (text/gui_text.c) --
    nothing about measurement lives here; this file only loads pixels and hands metrics down.

    Included by gui_draw.c after gui_draw.h (-> text/gui_text.h types) + gui_res_atlas.h (the shared
    atlas), before gui_font.c.  .orb_font is a proportional font baked offline by font_tool: an R8
    atlas of packed glyph bitmaps plus per-glyph records (UV rect, bearing, advance).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    font_slot_load -- load a .orb_font from disk into `slot`.  Does not activate the slot.

    On success the slot's glyph pixels are resident in the shared atlas and its metrics + advance
    table describe a proportional font.  A slot that already holds a font (a live re-bake) updates
    its existing tenant in place; a failed load leaves the slot's previous font intact.
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

    /* Read the full baked atlas.  v3 fonts are pure glyph coverage; a legacy v2 font also carries a
       blank bottom band, which just rides along as dead space inside this font's packed rect (assists
       are atlas-level now).  hdr.atlas_h is the packed rect height fed to the shared atlas. */

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

    /* Pack the glyph pixels into the shared resource atlas.  A fresh slot adds a new tenant; a
       reload (slot already used) updates its existing tenant in place -- the persistent texture and
       stable bindless slot mean no per-frame create/destroy churn and no device drain here (the
       deferred flush at frame_begin sequences the GPU upload to a safe between-frames point). */
    u32 tenant;
    if ( slot->used && slot->atlas_tenant )
    {
        if ( !res_atlas_update( slot->atlas_tenant, pixels, hdr.atlas_w, hdr.atlas_h ) )
        {
            free( pixels );
            return false;   // slot keeps its previous font intact
        }
        tenant = slot->atlas_tenant;
    }
    else
    {
        tenant = res_atlas_add( pixels, hdr.atlas_w, hdr.atlas_h );
        if ( tenant == 0 )
        {
            free( pixels );
            return false;
        }
    }
    free( pixels );

    /* Commit into the slot (a loaded .orb_font has no integer upscale, so metrics are exact). */
    slot->ascent  = hdr.ascent;
    slot->descent = hdr.descent;
    memcpy( slot->lookup, lookup, sizeof( lookup ) );

    slot->atlas_tenant = tenant;
    slot->used         = true;
    slot->metrics      = ( font_metrics_t ){
        .type = {
            .char_h = (f32)( hdr.ascent - hdr.descent ),
            .line_h = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
            .size   = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
        },
    };

    printf( "[gui] loaded font '%s' (char_h=%.1f line_h=%.1f)\n",
            path, slot->metrics.type.char_h, slot->metrics.type.line_h );

    return true;
}

/*==============================================================================================
    font_slot_glyph -- per-character draw parameters for a slot.

    Outputs:
        u0..v1   atlas UV rect for the glyph bitmap
        ox, oy   pixel offsets from (cursor_x, text_y) to the top-left of the bitmap
        gw, gh   pixel dimensions of the bitmap to draw (0 for invisible glyphs like space)
        advance  horizontal cursor advance in pixels
==============================================================================================*/

static void
font_slot_glyph( const font_slot_t* slot, u8 ch,
                 f32* u0, f32* v0, f32* u1, f32* v1,
                 f32* ox, f32* oy, f32* gw, f32* gh, f32* advance )
{
    if ( ch < ORB_FONT_CP_FIRST || ch > ORB_FONT_CP_LAST ) ch = (u8)'?';
    const orb_font_glyph_t* g = &slot->lookup[ ch - ORB_FONT_CP_FIRST ];

    /* Glyph atlas_x/atlas_y are in the font's own baked pixel space; rebase by the font's live
       page origin in the shared atlas (valid across repacks) and scale by the shared atlas dims. */
    u32 px, py;
    res_atlas_origin( slot->atlas_tenant, &px, &py );
    f32 iw = res_atlas_inv_w();
    f32 ih = res_atlas_inv_h();
    *u0 = (f32)( px + g->atlas_x ) * iw;
    *v0 = (f32)( py + g->atlas_y ) * ih;
    *u1 = *u0 + (f32)g->w * iw;
    *v1 = *v0 + (f32)g->h * ih;

    *ox      = (f32)g->bearing_x;
    *oy      = (f32)( slot->ascent - (i32)g->bearing_y );
    *gw      = (f32)g->w;
    *gh      = (f32)g->h;
    *advance = (f32)g->advance;
}

/*==============================================================================================
    Deferred reload queue.

    A live font swap (font_load_into on a slot that already holds a font) rebuilds glyph pixels in
    the shared atlas.  Done mid-build that GPU churn -- upload / register -- interleaves with a frame
    the host is still assembling and, across the multi-context floater pass, with frames still in
    flight, which can fault the device (VK_ERROR_DEVICE_LOST).  Instead such a (re)load is queued
    here and committed once per frame from font_flush_pending(), which the UI unit calls at
    frame_begin -- a clean point between frames.  The slot keeps showing its current font until the
    swap lands, so there is no half-loaded slot to draw.
==============================================================================================*/

#define GUI_FONT_PATH_MAX 512

typedef struct
{
    bool used;                          // a deferred (re)load is queued
    u32  id;                            // target slot id
    char path[ GUI_FONT_PATH_MAX ];     // .orb_font to load at the next flush

} font_reload_req_t;

/* One pending request per slot at most -- repeated bakes into the same slot coalesce to the last
   path -- so REGISTRY_MAX entries can never overflow. */

static font_reload_req_t s_reload_q[ GUI_FONT_REGISTRY_MAX ];

/* Queue (or re-target) a deferred reload of slot `id`.  A slot already queued keeps its place and
   takes the newest path, collapsing a burst of re-bakes into one swap. */

static void
font_reload_enqueue( u32 id, const char* path )
{
    i32 slot = -1;
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
    {
        if ( s_reload_q[ i ].used && s_reload_q[ i ].id == id )
        {
            fmt_snprintf( s_reload_q[ i ].path, sizeof( s_reload_q[ i ].path ), "%s", path );
            return;
        }
        if ( slot < 0 && !s_reload_q[ i ].used )
            slot = (i32)i;
    }
    if ( slot < 0 )
        return;   /* every slot already queued -- cannot happen (one entry per slot id) */

    s_reload_q[ slot ].used = true;
    s_reload_q[ slot ].id   = id;
    fmt_snprintf( s_reload_q[ slot ].path, sizeof( s_reload_q[ slot ].path ), "%s", path );
}

/*==============================================================================================
    font_internal_load_into -- the one piece of load logic with more than one gui_font.c caller
    (font_load_into AND font_load_builtin both need the enqueue-vs-synchronous-load branch).

    Load a font into an existing id (id 0 swaps the default).  Returns false on bad id; a deferred
    request always reports success (committed later by font_flush_pending).  A slot that already
    holds a font stays valid on screen until the swap; a fresh slot loads synchronously.
==============================================================================================*/

static bool
font_internal_load_into( u32 id, const char* path )
{
    font_slot_t* slot = font_slot_ptr( id );
    if ( !slot )
        return false;

    if ( slot->used )
    {
        font_reload_enqueue( id, path );
        return true;
    }

    if ( !font_slot_load( slot, path ) )
        return false;
    if ( font_active_id() == id )
        font_activate( id );            // metrics rebuilt in place; refresh active pointers
    return true;
}

/*==============================================================================================
    BACKEND-INTERNAL -- module lifecycle, called from gui_draw.c (gui_draw_boot / shutdown).
==============================================================================================*/

static void
font_shutdown( void )
{
    /* Drop any deferred reloads that never reached a frame_begin flush, then clear the CPU
       registry (text/ leaf).  Fonts own no GPU resource of their own -- glyph pixels live in the
       shared resource atlas, torn down once by res_atlas_shutdown (gui_backend_exit). */
    memset( s_reload_q, 0, sizeof( s_reload_q ) );
    font_registry_reset();
}

static bool
font_init( void )
{
    /* Deliberately a no-op, not a placeholder: font_init exists as the paired bookend to
       font_shutdown but has nothing to allocate.  A font atlas needs actual glyph pixels from an
       .orb_font, which only font_load / font_load_into supply.  Slot 0 starts empty; font_valid()
       (text/ leaf) reports that until the host's own load activates one. */
    return true;
}

/*==============================================================================================
    BACKEND-INTERNAL -- shared-atlas redirects consumed within this unit (canvas texture preview)
    and named font_* so the tessellation hot path reads one shared atlas for text, fills and icons.
==============================================================================================*/

static u32  font_atlas_idx  ( void )               { return res_atlas_idx();   }
static void font_white_uv   ( f32* u, f32* v )     { res_atlas_white_uv( u, v ); }
static f32  font_dash_v     ( f32 duty )           { return res_atlas_dash_v( duty ); }

/* Total GPU bytes held by the shared resource atlas (R8_UNORM, 1 byte/pixel) -- one texture now,
   not one per font. */
static u32
font_atlas_bytes( void )
{
    return res_atlas_bytes();
}

// clang-format on
/*============================================================================================*/
