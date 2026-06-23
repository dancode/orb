/*==============================================================================================

    runtime_service/imgui/imgui_font.c -- Font registry and per-glyph dispatch.

    A registry of font slots (s_fonts[]) lets several fonts be loaded at once, each addressed by
    an integer id.  One slot is the active font at any time; s_active points at it and s_font at
    its metrics.  All dispatch helpers read from s_font / s_active, so callers never inspect which
    slot or source is active.

    Slot 0 is the default / fallback.  It starts as a built-in bitmap font and is rebuilt by
    font_set_bitmap() / font_set_bmp_scale().  It can also be swapped to a loaded TrueType font
    via font_load_into( 0, path ).

        font_load()      -- load a .orb_font into a fresh id, activate it, return the id (0 = fail).
        font_load_into() -- load a .orb_font into an existing id (e.g. swap the default, id 0).
        font_use()       -- make an already-loaded id the active font.

    A slot is one of two sources, distinguished by metrics.proportional:
        Bitmap    -- references a built-in atlas (bitmap_font_t) owned by imgui_font_builtin.c;
                     the slot does not own its GPU atlas (owns_atlas == false).
        TrueType  -- a .orb_font loaded at runtime; the slot owns its GPU atlas.
                     lookup[] is indexed by (codepoint - 32); entries with advance == 0 are missing.

    Included by imgui_backend.c after imgui_font_builtin.c.

==============================================================================================*/
// clang-format off

#include "tools/font_tool/orb_font.h" /* font file formats and on-disk structure */

/*----------------------------------------------------------------------------------------------
    font_slot_t -- one entry in the font registry; bitmap-backed or a loaded TrueType font.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    font_metrics_t    metrics;          // first: resolved metrics; s_font points here when active

    bool              used;             // slot occupied
    bool              owns_atlas;       // true for loaded TrueType atlases (destroyed on free/reload)

    rhi_texture_t     atlas;            // GPU atlas (TrueType slots; bitmap slots leave this zero)
    u32               atlas_idx;        // bindless index for the atlas texture (mirrors metrics.atlas_idx)
    u32               atlas_w;          // atlas width in pixels  (memory accounting)
    u32               atlas_h;          // uploaded atlas height  (memory accounting)

    /* TrueType source (metrics.proportional == true). */

    i32               ascent;           // pixels above baseline (positive)
    i32               descent;          // pixels below baseline (negative)
    orb_font_glyph_t  lookup[ 95 ];     // codepoints 32..126

    /* Bitmap source (metrics.proportional == false). */

    const bitmap_font_def_t* def;       // non-NULL when this slot is bitmap-backed

} font_slot_t;

static font_slot_t  s_fonts[ IMGUI_FONT_REGISTRY_MAX ]; // font registry; slot 0 is the default
static font_slot_t* s_active     = NULL;                // active slot (s_font == &s_active->metrics)
static u32          s_active_id  = 0;                    // id of the active slot

/*==============================================================================================
    Font : Slot Helpers
==============================================================================================*/

/* Release a slot's GPU atlas, but only when the slot owns it.  Bitmap-backed slots reference an
   atlas owned by imgui_font_builtin.c and must not destroy it. */

static void
font_slot_free_gpu( font_slot_t* slot )
{
    if ( slot->owns_atlas )
    {
        if ( slot->atlas_idx != 0 )
            rhi()->unregister_texture( slot->atlas_idx );
        if ( rhi_handle_valid( slot->atlas ) )
            rhi()->texture_destroy( slot->atlas );
    }
    slot->atlas      = ( rhi_texture_t ){ 0 };
    slot->atlas_idx  = 0;
    slot->owns_atlas = false;
}

/* Point the active-font pointers at slot `id`. */
static void
font_activate( u32 id )
{
    s_active_id = id;
    s_active    = &s_fonts[ id ];
    s_font      = &s_active->metrics;
}

/* Resolve the currently selected built-in bitmap into a slot (used for the default, slot 0). */
static void
font_slot_set_bitmap( font_slot_t* slot )
{
    font_slot_free_gpu( slot );             // drop any owned TrueType atlas previously here

    slot->metrics    = s_bitmap_active->metrics;       // metrics carry the bitmap's atlas_idx
    slot->def        = s_bitmap_active->def;
    slot->atlas      = ( rhi_texture_t ){ 0 };          // atlas owned by imgui_font_builtin.c
    slot->atlas_idx  = s_bitmap_active->atlas_idx;
    slot->atlas_w    = s_bitmap_active->def->atlas_w;
    slot->atlas_h    = s_bitmap_active->tex_h;
    slot->owns_atlas = false;
    slot->used       = true;
}

/*----------------------------------------------------------------------------------------------
    tt_load_file -- load a .orb_font from disk into `slot`.  Does not activate the slot.

    On success the slot owns a freshly created GPU atlas and metrics describe a proportional font;
    any previously owned atlas in the slot is released first.  On failure the slot is left in a
    valid (freed) state and false is returned.
----------------------------------------------------------------------------------------------*/

static bool
tt_load_file( font_slot_t* slot, const char* path )
{
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

    orb_font_glyph_t lookup[ 95 ];
    memset( lookup, 0, sizeof( lookup ) );
    for ( u32 i = 0; i < hdr.glyph_count; ++i )
    {
        orb_font_glyph_t g;
        if ( fread( &g, sizeof( g ), 1, f ) != 1 ) { fclose( f ); return false; }
        if ( g.codepoint >= 32 && g.codepoint <= 126 )
            lookup[ g.codepoint - 32 ] = g;
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

    rhi_texture_t atlas = rhi()->texture_create( &( rhi_texture_desc_t ){
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

    /* All GPU work succeeded -- commit into the slot.  Release any atlas it held first; doing
       this only now means a failed reload leaves the previous font intact above. */

    font_slot_free_gpu( slot );

    memcpy( slot->lookup, lookup, sizeof( lookup ) );
    slot->def        = NULL;            // TrueType source: no bitmap def
    slot->ascent     = hdr.ascent;
    slot->descent    = hdr.descent;
    slot->atlas      = atlas;
    slot->atlas_idx  = atlas_idx;
    slot->atlas_w    = hdr.atlas_w;
    slot->atlas_h    = tex_h;           /* padded: glyph UV math divides by the uploaded height */
    slot->owns_atlas = true;
    slot->used       = true;

    slot->metrics = ( font_metrics_t ){
        .char_h       = (f32)( hdr.ascent - hdr.descent ),
        .line_h       = (f32)( hdr.ascent - hdr.descent + hdr.line_gap ),
        .char_w       = 0.0f,
        .size         = (f32)hdr.font_size,   // nominal type size (em) -- layout proportion base
        .atlas_idx    = atlas_idx,
        .proportional = true,
        /* White texel: center of the first appended row (pixel row hdr.atlas_h). */
        .white_u      = 0.5f / (f32)hdr.atlas_w,
        .white_v      = ( (f32)hdr.atlas_h + 0.5f ) / (f32)tex_h,
        /* Per-glyph UV scale, constant per font -- folds the divide out of font_glyph. */
        .inv_atlas_w  = 1.0f / (f32)hdr.atlas_w,
        .inv_atlas_h  = 1.0f / (f32)tex_h,
    };
    /* Dash pattern rows follow the white row at pixel row hdr.atlas_h + 1. */
    font_dash_row_v( slot->metrics.dash_v, hdr.atlas_h + 1u, tex_h );

    printf( "[imgui] loaded font '%s' (char_h=%.1f line_h=%.1f)\n",
            path, slot->metrics.char_h, slot->metrics.line_h );

    return true;
}

/*----------------------------------------------------------------------------------------------
    Registry API -- load / select fonts by id.
----------------------------------------------------------------------------------------------*/

/* First free slot id in 1..MAX-1, or 0 when the registry is full (0 is reserved for the default). */
static u32
font_alloc_slot( void )
{
    for ( u32 i = 1; i < IMGUI_FONT_REGISTRY_MAX; ++i )
        if ( !s_fonts[ i ].used )
            return i;
    return 0;
}

/* Load a TrueType font into a new id and activate it.  Returns the id, or 0 on failure
   (registry full, or the file failed to load). */
u32
font_load( const char* path )
{
    u32 id = font_alloc_slot();
    if ( id == 0 )
        return 0;
    if ( !tt_load_file( &s_fonts[ id ], path ) )
        return 0;
    font_activate( id );
    return id;
}

/* Load a TrueType font into an existing id (id 0 swaps the default).  Returns false on bad id or
   load failure -- on failure the slot keeps whatever font it had. */
bool
font_load_into( u32 id, const char* path )
{
    if ( id >= IMGUI_FONT_REGISTRY_MAX )
        return false;
    if ( !tt_load_file( &s_fonts[ id ], path ) )
        return false;
    if ( s_active_id == id )
        font_activate( id );            // metrics rebuilt in place; refresh active pointers
    return true;
}

/* Make an already-loaded id the active font.  Ignored if the id is empty or out of range. */
void
font_use( u32 id )
{
    if ( id >= IMGUI_FONT_REGISTRY_MAX || !s_fonts[ id ].used )
        return;
    font_activate( id );
}

/* Id of the active font slot -- callers save/restore this to push and pop fonts. */
u32 font_active_id( void ) { return s_active_id; }

/* Set the default (slot 0) to a built-in bitmap font and activate it. */
void
font_set_bitmap( imgui_font_t font )
{
    bitmap_font_select( font );             // resolve metrics into s_bitmap_active
    font_slot_set_bitmap( &s_fonts[ 0 ] );
    font_activate( 0 );
}

/* Integer upscale for the built-in bitmaps.  Refreshes the default slot if it is bitmap-backed. */
void
font_set_bmp_scale( u32 scale )
{
    bitmap_scale_set( scale );              // recompute s_bitmap_active->metrics at the new scale
    if ( s_fonts[ 0 ].def )                 // default still bitmap-backed -> re-resolve it
    {
        font_slot_set_bitmap( &s_fonts[ 0 ] );
        if ( s_active_id == 0 )
            font_activate( 0 );
    }
}

/*----------------------------------------------------------------------------------------------
    font_init / font_shutdown
----------------------------------------------------------------------------------------------*/

static void
font_shutdown( void )
{
    icon_atlas_shutdown();

    /* Release every slot's owned TrueType atlas, then the built-in bitmap atlases. */
    for ( u32 i = 0; i < IMGUI_FONT_REGISTRY_MAX; ++i )
        font_slot_free_gpu( &s_fonts[ i ] );
    memset( s_fonts, 0, sizeof( s_fonts ) );
    s_active    = NULL;
    s_active_id = 0;

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

    /* Seed slot 0 (the default / fallback) with the starting built-in bitmap and activate it. */
    font_set_bitmap( IMGUI_FONT_BITMAP_12 );

    /* Runtime icon atlas shares the font lifecycle: created after rhi is up, torn down with fonts. */
    if ( icon_atlas_init() == false ) { 
         font_shutdown(); return false; 
    }

    return true; /* built in fonts initialized successfully */
}

/*----------------------------------------------------------------------------------------------
    font_char_w / font_char_h / font_line_h / font_em / font_atlas_idx

    Dispatch helpers -- all read from s_font, set by font_activate().
----------------------------------------------------------------------------------------------*/

static f32  font_char_w      ( void ) { return s_font->char_w;    }
f32         font_char_h      ( void ) { return s_font->char_h;    }
f32         font_line_h      ( void ) { return s_font->line_h;    }
f32         font_em          ( void ) { return s_font->size;      }   // nominal type size (em) -- layout base
static u32  font_atlas_idx   ( void ) { return s_font->atlas_idx; }

/* Whether the active font is a TrueType font (vs. a built-in bitmap).  The UI unit's font API
   (imgui_set_bmp_scale) keys off this -- a bmp-scale change only re-derives layout when the active
   font is bitmap-backed. */
bool font_is_tt( void ) { return s_font->proportional; }

/* Log the active font (type, name, metrics).  Bitmap slots carry their def; TrueType slots do not,
   so they report by source only. */
void
font_print_active( void )
{
    const char* name = s_active->def ? s_active->def->debug_name : "<loaded>";
    printf( "[imgui] set font [%u] '%s : %s' (char_h=%.1f line_h=%.1f)\n",
            s_active_id, s_font->proportional ? "TrueType" : "Bitmap",
            name, s_font->char_h, s_font->line_h );
}

/* Horizontal advance of one character in the active font.  Used by the text edit engine to
   measure glyph positions without emitting draw geometry (cursor placement, click-to-offset). */
f32
font_char_advance( u8 ch )
{
    if ( s_font->proportional )
    {
        if ( ch < 32 || ch > 126 ) ch = (u8)'?';
        return (f32)s_active->lookup[ ch - 32 ].advance;
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
   every initialized bitmap atlas, plus each loaded TrueType atlas in the registry. */
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
    for ( u32 i = 0; i < IMGUI_FONT_REGISTRY_MAX; ++i )
        if ( s_fonts[ i ].owns_atlas )
            bytes += s_fonts[ i ].atlas_w * s_fonts[ i ].atlas_h;
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
                w += (f32)s_active->lookup[ ch - 32 ].advance;
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
        const orb_font_glyph_t* g = &s_active->lookup[ ch - 32 ];

        f32 iw = s_font->inv_atlas_w;   /* precomputed at load -- no per-glyph divide */
        f32 ih = s_font->inv_atlas_h;
        *u0 = (f32)g->atlas_x * iw;
        *v0 = (f32)g->atlas_y * ih;
        *u1 = *u0 + (f32)g->w * iw;
        *v1 = *v0 + (f32)g->h * ih;

        *ox      = (f32)g->bearing_x;
        *oy      = (f32)( s_active->ascent - (i32)g->bearing_y );
        *gw      = (f32)g->w;
        *gh      = (f32)g->h;
        *advance = (f32)g->advance;
    }
    else
    {
        /* Bitmap path: fixed-grid UV, full-cell draw, monospace advance. */
        if ( ch < 32 || ch > 127 ) ch = (u8)'?';
        const bitmap_font_def_t* def = s_active->def;

        u32 idx = (u32)( ch - 32 );
        u32 col = idx % def->glyphs_row;
        u32 row = idx / def->glyphs_row;

        /* V scales by 1/tex_h (the padded upload height), not the glyph-grid height, so the
           appended white row stays outside every glyph's UV range.  Both scales are precomputed
           per font (inv_atlas_w / inv_atlas_h), so this is multiplies, not per-glyph divides. */
        f32 iw = s_font->inv_atlas_w;
        f32 ih = s_font->inv_atlas_h;
        *u0 = (f32)( col * def->glyph_w ) * iw;
        *v0 = (f32)( row * def->glyph_h ) * ih;
        *u1 = *u0 + (f32)def->glyph_w     * iw;
        *v1 = *v0 + (f32)def->glyph_h     * ih;

        *ox      = 0.0f;
        *oy      = 0.0f;
        *gw      = s_font->char_w;
        *gh      = s_font->char_h;
        *advance = s_font->char_w;
    }
}

// clang-format on
/*============================================================================================*/
