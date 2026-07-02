/*==============================================================================================

    runtime_service/gui/backend/gui_font_internal.c -- Font registry state and the .orb_font
    loader.

    Nothing here is declared in gui_backend.h -- none of it crosses to gui.c.  gui_font.c (included
    right after this file) is the ENTIRE public surface and is allowed to call straight into the
    statics and functions defined here (same TU).  Only font_internal_load_into lives here as a
    named function despite having no public name of its own -- it's shared by two gui_font.c
    callers (font_load_into, font_load_builtin).  Everything else gui_font.c needs is either a
    building block used from more than one place (the font_slot_* loader ops, font_activate,
    font_alloc_slot) or state (s_fonts, s_reload_q, ...); logic used by exactly one public
    function lives directly in that function, in gui_font.c.

    Owns the id-addressed registry (s_fonts[]), the active-font pointers (s_active / s_font), the
    deferred reload queue, the shared atlas finalize, and the .orb_font file loader.  .orb_font is
    a proportional font baked offline by font_tool (FreeType-rasterized, not an stb runtime bake):
    an R8 atlas of packed glyph bitmaps plus per-glyph records (UV rect, bearing, advance).  It is
    currently the only font source format gui loads.

    Slot 0 is the default.  It starts empty (used == false) until the host's first font_load /
    font_load_into( 0, path ) / font_load_builtin() call -- every caller of gui_init() loads a
    font immediately after, before any frame renders, so no frame ever needs a fallback font.

    Also holds the BACKEND-INTERNAL accessors (font_atlas_idx / font_white_uv / font_dash_v /
    font_atlas_bytes / font_init / font_shutdown) consumed by other backend/ files later in the
    unity build (gui_build_tess.c, gui_debug_overlay.c, gui_render.c) -- those
    aren't public either, they just have a wider audience than "this file only".

    Included by gui_backend.c after gui_font.h, before gui_font.c.

==============================================================================================*/
// clang-format off

static font_slot_t      s_fonts     [ GUI_FONT_REGISTRY_MAX ];  // font registry; slot 0 is the default
static font_slot_t*     s_active    = NULL;                     // active slot (s_font == &s_active->metrics)
static u32              s_active_id = 0;                        // active slot id (0 = default, 1..MAX-1 = user-loaded)
static font_metrics_t*  s_font      = NULL;                     // active font's metrics (read by every accessor)

/* On-fraction (dash / period) of each baked dash row; a dashed line picks the nearest at tess time. */
static const f32        s_dash_duty[ GUI_DASH_PATTERN_COUNT ] = { 0.12f, 0.35f, 0.5f, 0.7f };

/*==============================================================================================
    Atlas finalize -- the white texel + dash rows every font carries.
==============================================================================================*/

/* Paint the dash pattern rows into `pixels` (R8, width `w`) starting at pixel row `row0`. */
static void
font_paint_dash_rows( u8* pixels, u32 w, u32 row0 )
{
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
    {
        u8* row = &pixels[ ( row0 + p ) * w ];
        u32 on  = (u32)( s_dash_duty[ p ] * (f32)w + 0.5f );
        if ( on < 1 ) on = 1;
        if ( on > w ) on = w;
        memset( row, 0x00, w  );   /* gap    */
        memset( row, 0xFF, on );   /* on-run */
    }
}

/* Fill dash_v[]: pattern row p sits at pixel row row0 + p in a `tex_h`-tall upload. */
static void
font_dash_row_v( f32* dash_v, u32 row0, u32 tex_h )
{
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
        dash_v[ p ] = ( (f32)( row0 + p ) + 0.5f ) / (f32)tex_h;
}

/* Uploaded atlas height for a glyph region of `glyph_h` rows: one white texel row plus the dash
   pattern rows are appended on top.  The loader sizes its staging buffer to this. */
static u32
font_atlas_tex_h( u32 glyph_h )
{
    return glyph_h + 1u + GUI_DASH_PATTERN_COUNT;
}

/* Finalize a staged R8 atlas: append the white texel row and the dash pattern rows on top of the
   `glyph_h`-row glyph region, then resolve the metrics fields that describe them (white UV,
   per-glyph UV scale, dash row V coords).  `tex_h` must equal font_atlas_tex_h( glyph_h ). */

static void
font_finalize_atlas( u8* pixels, u32 atlas_w, u32 glyph_h, u32 tex_h, font_metrics_t* m )
{
    /* White texel strip: fill the first appended row opaque, then the dash pattern rows. */
    memset( &pixels[ glyph_h * atlas_w ], 0xFF, atlas_w );
    font_paint_dash_rows( pixels, atlas_w, glyph_h + 1u );

    /* White texel: center of the first appended row (pixel row glyph_h). */
    m->atlas.white_u = 0.5f / (f32)atlas_w;
    m->atlas.white_v = ( (f32)glyph_h + 0.5f ) / (f32)tex_h;

    /* Per-glyph UV scale, constant per font -- folds the divide out of the glyph path.  V divides
       by tex_h (the padded height), so the appended rows stay outside every glyph's UV range. */
    m->atlas.inv_atlas_w = 1.0f / (f32)atlas_w;
    m->atlas.inv_atlas_h = 1.0f / (f32)tex_h;

    /* Dash pattern rows follow the white row at pixel row glyph_h + 1. */
    font_dash_row_v( m->atlas.dash_v, glyph_h + 1u, tex_h );
}

/*==============================================================================================
    The .orb_font loader.
==============================================================================================*/

/* Release a slot's owned GPU atlas. */
static void
font_slot_free_gpu( font_slot_t* slot )
{
    gui_atlas_destroy( &slot->atlas );
}

/*----------------------------------------------------------------------------------------------
    font_slot_load -- load a .orb_font from disk into `slot`.  Does not activate the slot.

    On success the slot owns a freshly created GPU atlas and metrics describe a proportional font;
    any atlas the slot previously owned is released only after the new one is fully built, so a
    failed load leaves the slot's previous font intact.
----------------------------------------------------------------------------------------------*/

static bool
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

static f32
font_slot_char_advance( const font_slot_t* slot, u8 ch )
{
    if ( ch < 32 || ch > 126 ) ch = (u8)'?';
    return (f32)slot->lookup[ ch - 32 ].advance;
}

static void
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

/*----------------------------------------------------------------------------------------------
    Deferred reload queue.

    A live font swap (font_load_into on a slot that already holds a font) builds a fresh GPU atlas
    and tears the old one down.  Done mid-build that GPU churn -- create / upload / register, plus
    the deferred destroy of the outgoing atlas -- interleaves with a frame the host is still
    assembling and, across the multi-context floater pass, with frames still in flight, which can
    fault the device (VK_ERROR_DEVICE_LOST).  Instead such a (re)load is queued here and committed
    once per frame from font_flush_pending(), which the UI unit calls at frame_begin -- a clean
    point between frames, before any context renders.  The slot keeps showing its current font
    until the swap lands, so there is no half-loaded slot to draw.
----------------------------------------------------------------------------------------------*/

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
            snprintf( s_reload_q[ i ].path, sizeof( s_reload_q[ i ].path ), "%s", path );
            return;
        }
        if ( slot < 0 && !s_reload_q[ i ].used )
            slot = (i32)i;
    }
    if ( slot < 0 )
        return;   /* every slot already queued -- cannot happen (one entry per slot id) */

    s_reload_q[ slot ].used = true;
    s_reload_q[ slot ].id   = id;
    snprintf( s_reload_q[ slot ].path, sizeof( s_reload_q[ slot ].path ), "%s", path );
}

/*==============================================================================================
    Slot activation.
==============================================================================================*/

/* Point the active-font pointers at slot `id`. */

static void
font_activate( u32 id )
{
    s_active_id = id;
    s_active    = &s_fonts[ id ];
    s_font      = &s_active->metrics;
}

/* First free slot id in 1..MAX-1, or 0 when the registry is full (0 is reserved for the default). */

static u32
font_alloc_slot( void )
{
    for ( u32 i = 1; i < GUI_FONT_REGISTRY_MAX; ++i )
        if ( !s_fonts[ i ].used )
            return i;
    return 0;
}

/*==============================================================================================
    font_internal_load_into -- the one piece of registry logic with more than one gui_font.c
    caller (font_load_into AND font_load_builtin both need the enqueue-vs-synchronous-load
    branch), so it earns a real internal function instead of being inlined into either.

    Load a font into an existing id (id 0 swaps the default).  Returns false on bad id; a deferred
    request always reports success (it is committed later by font_flush_pending).

    A slot that already holds a font stays valid and on screen until the swap, so the GPU atlas
    rebuild is deferred to the next frame_begin latch -- the create/upload/register and the deferred
    destroy of the old atlas then land between frames, never mid-build (see s_reload_q).  A fresh
    slot has no font to show, so it loads synchronously; on failure the slot keeps what it had.
==============================================================================================*/

static bool
font_internal_load_into( u32 id, const char* path )
{
    if ( id >= GUI_FONT_REGISTRY_MAX )
        return false;

    if ( s_fonts[ id ].used )
    {
        font_reload_enqueue( id, path );
        return true;
    }

    if ( !font_slot_load( &s_fonts[ id ], path ) )
        return false;
    if ( s_active_id == id )
        font_activate( id );            // metrics rebuilt in place; refresh active pointers
    return true;
}

/*==============================================================================================
    BACKEND-INTERNAL -- module lifecycle, called from gui_render.c
    (gui_render_init/shutdown).
==============================================================================================*/

static void
font_shutdown( void )
{
    /* Drop any deferred reloads that never reached a frame_begin flush. */
    memset( s_reload_q, 0, sizeof( s_reload_q ) );

    /* Release every slot's owned atlas. */
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
        font_slot_free_gpu( &s_fonts[ i ] );
    memset( s_fonts, 0, sizeof( s_fonts ) );
    s_active    = NULL;
    s_active_id = 0;
    s_font      = NULL;
}

static bool
font_init( void )
{
    /* Deliberately a no-op, not a placeholder: font_init exists as the paired bookend to
       font_shutdown (called from gui_render_init/shutdown) but has nothing to allocate.  Creating
       a font atlas needs actual glyph pixels from an .orb_font, which only font_load /
       font_load_into supply -- gui_render_init's job is standing up the GPU bindings those loads
       will later fill (pipeline, font sampler), not conjuring an atlas with nothing in it.  Slot 0
       starts empty; font_valid() reports that until the host's own font_load_builtin / font_load
       call activates one -- see gui_init's font_valid() gate in gui_frame.c for the consumer side
       of that contract.  The icon atlas is a separate, optional layer -- gui_backend_init stands it
       up (gated on s_caps.icons), not this font-only function. */
    return true;
}

/*==============================================================================================
    BACKEND-INTERNAL -- consumed by gui_build_tess.c (atlas index / white texel / dash rows)
    and gui_debug_overlay.c (atlas index / white texel), and gui_render.c (memory
    stats).
==============================================================================================*/

static u32 
font_atlas_idx( void ) { return s_font->atlas.atlas_idx; }

/* UV of the active atlas's white texel (appended bottom row) for solid-color draws. */

static void 
font_white_uv( f32* u, f32* v ) { *u = s_font->atlas.white_u; *v = s_font->atlas.white_v; }

/* Center V of the dash pattern row whose baked on-fraction is closest to `duty`.  Tessellated
   dashed lines sample this row, tiling it along the line via REPEAT addressing on U. */

static f32
font_dash_v( f32 duty )
{
    u32 best  = 0;
    f32 bestd = 1e30f;
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
    {
        f32 d = s_dash_duty[ p ] - duty;
        if ( d < 0.0f ) d = -d;
        if ( d < bestd ) { bestd = d; best = p; }
    }
    return s_font->atlas.dash_v[ best ];
}

/* Total bytes of GPU memory held by font atlas textures (R8_UNORM, 1 byte/pixel): each loaded
   font's owned atlas in the registry. */

static u32
font_atlas_bytes( void )
{
    u32 bytes = 0;
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
        if ( s_fonts[ i ].used )
            bytes += s_fonts[ i ].atlas.atlas_w * s_fonts[ i ].atlas.atlas_h;
    return bytes;
}

// clang-format on
/*============================================================================================*/
