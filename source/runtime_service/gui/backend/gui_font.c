/*==============================================================================================

    runtime_service/gui/backend/gui_font.c -- Font registry, glyph dispatch, and the .orb_font
    loader.

    Owns the id-addressed registry (s_fonts[]), the active-font pointers (s_active / s_font), the
    shared atlas finalize, and the .orb_font file loader.  .orb_font is a proportional font baked
    offline by font_tool (FreeType-rasterized, not an stb runtime bake): an R8 atlas of packed
    glyph bitmaps plus per-glyph records (UV rect, bearing, advance).  It is currently the only
    font source format gui loads.

    Slot 0 is the default.  It starts empty (used == false) until the host's first font_load /
    font_load_into( 0, path ) / font_load_builtin() call -- every caller of gui_init() loads a
    font immediately after, before any frame renders, so no frame ever needs a fallback font.

        font_load()         -- load a font into a fresh id, activate it, return the id (0 = fail).
        font_load_into()    -- load a font into an existing id (e.g. swap the default, id 0).
        font_load_builtin() -- load one of the engine's built-in presets into slot 0.
        font_use()          -- make an already-loaded id the active font.

    Included by gui_backend.c after gui_font.h.

    Visibility tiers below (this is a unity build -- "static" only means "does not cross the
    gui.c / gui_backend.c seam", not "used once"):

        PUBLIC            -- declared in gui_backend.h; called from gui.c (the UI unit).
        BACKEND-INTERNAL  -- static; called from other backend/ files (gui_02_build_tess.c,
                             gui_04_debug_overlay.c, gui_03_submit_render.c) later in the unity
                             build.
        FILE-LOCAL        -- static; used only inside this file.

==============================================================================================*/
// clang-format off

static font_slot_t      s_fonts     [ GUI_FONT_REGISTRY_MAX ];  // font registry; slot 0 is the default
static font_slot_t*     s_active    = NULL;                     // active slot (s_font == &s_active->metrics)
static u32              s_active_id = 0;                        // active slot id (0 = default, 1..MAX-1 = user-loaded)
static font_metrics_t*  s_font      = NULL;                     // active font's metrics (read by every accessor)

/* On-fraction (dash / period) of each baked dash row; a dashed line picks the nearest at tess time. */
static const f32        s_dash_duty[ GUI_DASH_PATTERN_COUNT ] = { 0.12f, 0.35f, 0.5f, 0.7f };

/*==============================================================================================
    FILE-LOCAL -- atlas finalize: the white texel + dash rows every font carries.
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
    FILE-LOCAL -- the .orb_font loader.
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
    FILE-LOCAL -- slot activation.
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
    PUBLIC -- Registry API: load / select fonts by id (gui_backend.h).
==============================================================================================*/

/* Load a font into a new id and activate it.  Returns the id, or 0 on failure (registry full, or
   the file failed to load). */

u32
font_load( const char* path )
{
    u32  id = font_alloc_slot();
    if ( id == 0 )
         return 0;

    if ( !font_slot_load( &s_fonts[ id ], path ) )
        return 0;

    font_activate( id );
    return id;
}

/* Load a font into an existing id (id 0 swaps the default).  Returns false on bad id; a deferred
   request always reports success (it is committed later by font_flush_pending).

   A slot that already holds a font stays valid and on screen until the swap, so the GPU atlas
   rebuild is deferred to the next frame_begin latch -- the create/upload/register and the deferred
   destroy of the old atlas then land between frames, never mid-build (see s_reload_q).  A fresh
   slot has no font to show, so it loads synchronously; on failure the slot keeps what it had. */

bool
font_load_into( u32 id, const char* path )
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

/* Commit every queued deferred reload.  Called once per frame by the UI unit at frame_begin -- a
   safe point between frames -- so the GPU atlas swap never interleaves with an in-flight frame.
   Returns true when a committed load changed the active font, signalling the caller to rebuild
   layout from the new metrics. */

bool
font_flush_pending( void )
{
    bool active_reloaded = false;

    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
    {
        if ( !s_reload_q[ i ].used )
            continue;

        u32  id = s_reload_q[ i ].id;
        bool ok = font_slot_load( &s_fonts[ id ], s_reload_q[ i ].path );
        s_reload_q[ i ] = ( font_reload_req_t ){ 0 };

        if ( ok && s_active_id == id )
        {
            font_activate( id );        // metrics rebuilt in place; refresh active pointers
            active_reloaded = true;
        }
    }

    return active_reloaded;
}

/* Make an already-loaded id the active font.  Ignored if the id is empty or out of range. */
void
font_use( u32 id )
{
    if ( id >= GUI_FONT_REGISTRY_MAX || !s_fonts[ id ].used )
        return;
    font_activate( id );
}

/* Id of the active font slot -- callers save/restore this to push and pop fonts. */

u32
font_active_id( void )
{
    return s_active_id;
}

/* Bindless atlas index currently backing font id `id` (0 for an empty / out-of-range slot).

   The retained render cache folds this into its per-window hash.  A font id is a stable handle,
   but font_load_into() can swap a *different* atlas under that same id -- the id is unchanged yet
   the bindless index baked into already-tessellated geometry now names a retired atlas.  Hashing
   the live atlas index (not just the id) makes that swap register as a change, forcing the window
   to re-tessellate against the new atlas instead of replaying vertices that sample a freed one. */

u32
font_slot_atlas_idx( u32 id )
{
    if ( id >= GUI_FONT_REGISTRY_MAX )
        return 0;
    return s_fonts[ id ].metrics.atlas.atlas_idx;
}

/* Path of every gui_builtin_font_t preset (gui.h), indexed by the enum; NULL for GUI_FONT_NONE. */
static const char* s_builtin_font_path[] = {
    [ GUI_FONT_NONE ]         = NULL,
    [ GUI_FONT_JETBRAINS_16 ] = "assets/font/jetbrains_regular_16.orb_font",
};

/* Load a built-in font preset into slot 0 and activate it.  A no-op success for GUI_FONT_NONE
   (the caller loads its own font); called from gui_init() when the host passes a preset. */
bool
font_load_builtin( gui_builtin_font_t font )
{
    if ( font == GUI_FONT_NONE )
        return true;
    if ( (u32)font >= ARRAY_COUNT( s_builtin_font_path ) || !s_builtin_font_path[ font ] )
        return false;
    return font_load_into( 0, s_builtin_font_path[ font ] );  // slot 0 = the default font
}

/*==============================================================================================
    BACKEND-INTERNAL -- module lifecycle, called from gui_03_submit_render.c
    (gui_render_init/shutdown).
==============================================================================================*/

static void
font_shutdown( void )
{
    icon_atlas_shutdown();

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
    /* Slot 0 starts empty; the host loads its first font immediately after gui_init(), before any
       frame renders (see the file header note). */

    /* Runtime icon atlas shares the font lifecycle: created after rhi is up, torn down with fonts. */
    if ( !icon_atlas_init() ) { font_shutdown(); return false; }

    return true;
}

/*==============================================================================================
    PUBLIC -- Dispatch helpers (gui_backend.h): all read from s_font / s_active, set by
    font_activate().
==============================================================================================*/

f32  font_char_h      ( void ) { return s_font->type.char_h; }
f32  font_line_h      ( void ) { return s_font->type.line_h; }
f32  font_em          ( void ) { return s_font->type.size;   }   // nominal type size (em) -- layout base

/* Log the active font (id, name, metrics). */
void
font_print_active( void )
{
    printf( "[gui] set font [%u] '<loaded>' (char_h=%.1f line_h=%.1f)\n",
            s_active_id, s_font->type.char_h, s_font->type.line_h );
}

/* Horizontal advance of one character in the active font.  Used by the text edit engine to
   measure glyph positions without emitting draw geometry (cursor placement, click-to-offset). */
f32
font_char_advance( u8 ch )
{
    return font_slot_char_advance( s_active, ch );
}

/* Width of the first n bytes of str (stops early at a NUL).  Labels measure only their visible
   span this way -- the bytes before a "##" marker -- so reserved label space matches what draws.
   Non-printable bytes contribute nothing (they are never emitted as glyphs). */
f32
font_text_w_n( const char* str, u32 n )
{
    f32 w = 0.0f;
    for ( u32 i = 0; i < n && str[ i ]; ++i )
    {
        u8 ch = (u8)str[ i ];
        if ( ch >= 32 && ch <= 126 )
            w += (f32)s_active->lookup[ ch - 32 ].advance;
    }
    return w;
}

f32
font_text_w( const char* str )
{
    return font_text_w_n( str, 0xFFFFFFFFu );
}

/*----------------------------------------------------------------------------------------------
    font_glyph -- per-character draw parameters.

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
    font_slot_glyph( s_active, ch, u0, v0, u1, v1, ox, oy, gw, gh, advance );
}

/*==============================================================================================
    BACKEND-INTERNAL -- consumed by gui_02_build_tess.c (atlas index / white texel / dash rows)
    and gui_04_debug_overlay.c (atlas index / white texel), and gui_03_submit_render.c (memory
    stats).
==============================================================================================*/

static u32 font_atlas_idx( void ) { return s_font->atlas.atlas_idx; }

/* UV of the active atlas's white texel (appended bottom row) for solid-color draws. */
static void font_white_uv( f32* u, f32* v ) { *u = s_font->atlas.white_u; *v = s_font->atlas.white_v; }

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
