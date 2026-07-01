/*==============================================================================================

    runtime_service/gui/backend/gui_load_font.c -- Neutral font registry and glyph dispatch.

    Owns the id-addressed registry (s_fonts[]), the active-font pointers (s_active / s_font), and
    the shared atlas finalize.  Every slot is a loaded proportional .orb_font (gui_load_font_ttf.c);
    this unit only adds the registry, activation, and the deferred-reload queue around it.

    Slot 0 is the default.  It starts empty (used == false) until the host's first font_load /
    font_load_into( 0, path ) call -- every caller of gui_init() loads a font immediately after,
    before any frame renders, so no frame ever needs a fallback font.

        font_load()      -- load a font into a fresh id, activate it, return the id (0 = fail).
        font_load_into() -- load a font into an existing id (e.g. swap the default, id 0).
        font_use()       -- make an already-loaded id the active font.

    Included by gui_backend.c after gui_load_font_ttf.c.

==============================================================================================*/
// clang-format off

static font_slot_t      s_fonts     [ GUI_FONT_REGISTRY_MAX ];    // font registry; slot 0 is the default
static font_slot_t*     s_active    = NULL;                         // active slot (s_font == &s_active->metrics)
static u32              s_active_id = 0;                            // id of the active slot
static font_metrics_t*  s_font      = NULL;                         // active font's metrics (read by every accessor)

/* On-fraction (dash / period) of each baked dash row; a dashed line picks the nearest at tess time. */
static const f32 s_dash_duty[ GUI_DASH_PATTERN_COUNT ] = { 0.12f, 0.35f, 0.5f, 0.7f };

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
    Shared atlas finalize -- the white texel + dash rows every font carries.
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
   pattern rows are appended on top.  Builders size their staging buffer to this. */
u32
font_atlas_tex_h( u32 glyph_h )
{
    return glyph_h + 1u + GUI_DASH_PATTERN_COUNT;
}

/* Finalize a staged R8 atlas: append the white texel row and the dash pattern rows on top of the
   `glyph_h`-row glyph region, then resolve the metrics fields that describe them (white UV,
   per-glyph UV scale, dash row V coords).  `tex_h` must equal font_atlas_tex_h( glyph_h ). */

void
font_finalize_atlas( u8* pixels, u32 atlas_w, u32 glyph_h, u32 tex_h, font_metrics_t* m )
{
    /* White texel strip: fill the first appended row opaque, then the dash pattern rows. */
    memset( &pixels[ glyph_h * atlas_w ], 0xFF, atlas_w );
    font_paint_dash_rows( pixels, atlas_w, glyph_h + 1u );

    /* White texel: center of the first appended row (pixel row glyph_h). */
    m->white_u     = 0.5f / (f32)atlas_w;
    m->white_v     = ( (f32)glyph_h + 0.5f ) / (f32)tex_h;

    /* Per-glyph UV scale, constant per font -- folds the divide out of the glyph path.  V divides
       by tex_h (the padded height), so the appended rows stay outside every glyph's UV range. */
    m->inv_atlas_w = 1.0f / (f32)atlas_w;
    m->inv_atlas_h = 1.0f / (f32)tex_h;

    /* Dash pattern rows follow the white row at pixel row glyph_h + 1. */
    font_dash_row_v( m->dash_v, glyph_h + 1u, tex_h );
}

/*==============================================================================================
    Slot lifecycle
==============================================================================================*/

/* Release a slot's owned GPU atlas. */

void
font_slot_free_gpu( font_slot_t* slot )
{
    if ( slot->atlas_idx != 0 )
        rhi()->unregister_texture( slot->atlas_idx );
    if ( rhi_handle_valid( slot->atlas ) )
        rhi()->texture_destroy( slot->atlas );
    slot->atlas     = ( rhi_texture_t ){ 0 };
    slot->atlas_idx = 0;
}

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
    Registry API -- load / select fonts by id.
==============================================================================================*/

/* Load a font into a new id and activate it.  Returns the id, or 0 on failure (registry full, or
   the file failed to load). */

u32
font_load( const char* path )
{
    u32  id = font_alloc_slot();
    if ( id == 0 )
         return 0;

    if ( !ttf_load_file( &s_fonts[ id ], path ) )
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

    if ( !ttf_load_file( &s_fonts[ id ], path ) )
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
        bool ok = ttf_load_file( &s_fonts[ id ], s_reload_q[ i ].path );
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
    return s_fonts[ id ].metrics.atlas_idx;
}

/*==============================================================================================
    font_init / font_shutdown
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
    Dispatch helpers -- all read from s_font / s_active, set by font_activate().
==============================================================================================*/

f32  font_char_h      ( void ) { return s_font->char_h;    }
f32  font_line_h      ( void ) { return s_font->line_h;    }
f32  font_em          ( void ) { return s_font->size;      }   // nominal type size (em) -- layout base
static u32  font_atlas_idx   ( void ) { return s_font->atlas_idx; }

/* Log the active font (id, name, metrics). */
void
font_print_active( void )
{
    printf( "[gui] set font [%u] '<loaded>' (char_h=%.1f line_h=%.1f)\n",
            s_active_id, s_font->char_h, s_font->line_h );
}

/* Horizontal advance of one character in the active font.  Used by the text edit engine to
   measure glyph positions without emitting draw geometry (cursor placement, click-to-offset). */
f32
font_char_advance( u8 ch )
{
    return ttf_char_advance( s_active, ch );
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
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
    {
        f32 d = s_dash_duty[ p ] - duty;
        if ( d < 0.0f ) d = -d;
        if ( d < bestd ) { bestd = d; best = p; }
    }
    return s_font->dash_v[ best ];
}

/* Total bytes of GPU memory held by font atlas textures (R8_UNORM, 1 byte/pixel): each loaded
   font's owned atlas in the registry. */
static u32
font_atlas_bytes( void )
{
    u32 bytes = 0;
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
        if ( s_fonts[ i ].used )
            bytes += s_fonts[ i ].atlas_w * s_fonts[ i ].atlas_h;
    return bytes;
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
    ttf_glyph( s_active, ch, u0, v0, u1, v1, ox, oy, gw, gh, advance );
}

// clang-format on
/*============================================================================================*/
