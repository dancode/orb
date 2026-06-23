/*==============================================================================================

    runtime_service/imgui/backend/imgui_font.c -- Neutral font registry and glyph dispatch.

    Source-agnostic core of the font unit.  It owns the id-addressed registry (s_fonts[]), the
    active-font pointers (s_active / s_font), and the shared atlas finalize.  Per-source detail
    lives in imgui_font_bmp.c (bmp grid atlases) and imgui_font_ttf.c (proportional .orb_font);
    this unit only dispatches to them by the slot's src tag.

    Slot 0 is the default / fallback.  It starts as a built-in bmp font and is rebuilt by
    font_set_bitmap() / font_set_bmp_scale().  It can also be swapped to a loaded ttf font via
    font_load_into( 0, path ).

        font_load()      -- load a font into a fresh id, activate it, return the id (0 = fail).
        font_load_into() -- load a font into an existing id (e.g. swap the default, id 0).
        font_use()       -- make an already-loaded id the active font.

    Included by imgui_backend.c after imgui_font_bmp.c and imgui_font_ttf.c.

==============================================================================================*/
// clang-format off

static font_slot_t      s_fonts     [ IMGUI_FONT_REGISTRY_MAX ];    // font registry; slot 0 is the default
static font_slot_t*     s_active    = NULL;                         // active slot (s_font == &s_active->metrics)
static u32              s_active_id = 0;                            // id of the active slot
static font_metrics_t*  s_font      = NULL;                         // active font's metrics (read by every accessor)

/* On-fraction (dash / period) of each baked dash row; a dashed line picks the nearest at tess time. */
static const f32 s_dash_duty[ IMGUI_DASH_PATTERN_COUNT ] = { 0.12f, 0.35f, 0.5f, 0.7f };

/*==============================================================================================
    Shared atlas finalize -- the white texel + dash rows every font carries.
==============================================================================================*/

/* Paint the dash pattern rows into `pixels` (R8, width `w`) starting at pixel row `row0`. */
static void
font_paint_dash_rows( u8* pixels, u32 w, u32 row0 )
{
    for ( u32 p = 0; p < IMGUI_DASH_PATTERN_COUNT; ++p )
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
    for ( u32 p = 0; p < IMGUI_DASH_PATTERN_COUNT; ++p )
        dash_v[ p ] = ( (f32)( row0 + p ) + 0.5f ) / (f32)tex_h;
}

/* Uploaded atlas height for a glyph region of `glyph_h` rows: one white texel row plus the dash
   pattern rows are appended on top.  Builders size their staging buffer to this. */
u32
font_atlas_tex_h( u32 glyph_h )
{
    return glyph_h + 1u + IMGUI_DASH_PATTERN_COUNT;
}

/* Finalize a staged R8 atlas: append the white texel row and the dash pattern rows on top of the
   `glyph_h`-row glyph region, then resolve the metrics fields that describe them (white UV,
   per-glyph UV scale, dash row V coords).  `tex_h` must equal font_atlas_tex_h( glyph_h ).

   Every font builder (bmp and ttf) routes through here, so every atlas -- whatever its source --
   is a self-sufficient backing texture for solid fills, dashed strokes, and text. */

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

/* Release a slot's GPU atlas, but only when the slot owns it.  bmp slots reference an atlas owned
   by imgui_font_bmp.c and must not destroy it. */

void
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

/* First free slot id in 1..MAX-1, or 0 when the registry is full (0 is reserved for the default). */
static u32
font_alloc_slot( void )
{
    for ( u32 i = 1; i < IMGUI_FONT_REGISTRY_MAX; ++i )
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

/* Load a font into an existing id (id 0 swaps the default).  Returns false on bad id or load
   failure -- on failure the slot keeps whatever font it had. */

bool
font_load_into( u32 id, const char* path )
{
    if ( id >= IMGUI_FONT_REGISTRY_MAX )
        return false;
    if ( !ttf_load_file( &s_fonts[ id ], path ) )
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

u32 
font_active_id( void ) 
{ 
    return s_active_id; 
}

/* Set the default (slot 0) to a built-in bmp font and activate it. */

void 
font_set_bitmap( imgui_font_t font )
{
    bmp_select( font );                 // resolve metrics into the resident bmp
    bmp_fill_slot( &s_fonts[ 0 ] );     // copy it into the default slot (referencing, not owning)
    font_activate( 0 );
}

/* Integer upscale for the built-in bmps.  Refreshes the default slot if it is still bmp-backed. */

void
font_set_bmp_scale( u32 scale )
{
    bmp_scale_set( scale );             // recompute the resident bmp metrics at the new scale
    if ( s_fonts[ 0 ].src == FONT_SRC_BMP )
    {
        bmp_fill_slot( &s_fonts[ 0 ] );
        if ( s_active_id == 0 )
            font_activate( 0 );
    }
}

/*==============================================================================================
    font_init / font_shutdown
==============================================================================================*/

static void
font_shutdown( void )
{
    icon_atlas_shutdown();

    /* Release every slot's owned atlas, then the resident built-in bmp atlases. */
    for ( u32 i = 0; i < IMGUI_FONT_REGISTRY_MAX; ++i )
        font_slot_free_gpu( &s_fonts[ i ] );
    memset( s_fonts, 0, sizeof( s_fonts ) );
    s_active    = NULL;
    s_active_id = 0;

    bmp_shutdown();
    s_font = NULL;
}

static bool
font_init( void )
{
    /* bmp_shutdown is safe on uninitialized fonts, so any failure here can delegate to
       font_shutdown for a single cleanup path. */
    if ( !bmp_init() ) { font_shutdown(); return false; }

    /* Seed slot 0 (the default / fallback) with the starting built-in bmp and activate it. */
    font_set_bitmap( IMGUI_FONT_BITMAP_16_JETBOLD );

    /* Runtime icon atlas shares the font lifecycle: created after rhi is up, torn down with fonts. */
    if ( !icon_atlas_init() ) { font_shutdown(); return false; }

    return true;
}

/*==============================================================================================
    Dispatch helpers -- all read from s_font / s_active, set by font_activate().
==============================================================================================*/

static f32  font_char_w      ( void ) { return s_font->char_w;    }
f32         font_char_h      ( void ) { return s_font->char_h;    }
f32         font_line_h      ( void ) { return s_font->line_h;    }
f32         font_em          ( void ) { return s_font->size;      }   // nominal type size (em) -- layout base
static u32  font_atlas_idx   ( void ) { return s_font->atlas_idx; }

/* Whether the active font is a ttf font (vs. a built-in bmp).  The UI unit's font API
   (imgui_set_bmp_scale) keys off this -- a bmp-scale change only re-derives layout when the active
   font is bmp-backed. */
bool font_is_tt( void ) { return s_active->src == FONT_SRC_TTF; }

/* Log the active font (id, type, name, metrics). */
void
font_print_active( void )
{
    const char* name = ( s_active->src == FONT_SRC_BMP ) ? s_active->bmp.def->debug_name : "<loaded>";
    printf( "[imgui] set font [%u] '%s : %s' (char_h=%.1f line_h=%.1f)\n",
            s_active_id, ( s_active->src == FONT_SRC_TTF ) ? "TrueType" : "Bitmap",
            name, s_font->char_h, s_font->line_h );
}

/* Horizontal advance of one character in the active font.  Used by the text edit engine to
   measure glyph positions without emitting draw geometry (cursor placement, click-to-offset). */
f32
font_char_advance( u8 ch )
{
    if ( s_active->src == FONT_SRC_TTF )
        return ttf_char_advance( s_active, ch );
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

/* Total bytes of GPU memory held by font atlas textures (R8_UNORM, 1 byte/pixel): the resident
   built-in bmp atlases plus each loaded font's owned atlas in the registry. */
static u32
font_atlas_bytes( void )
{
    u32 bytes = bmp_atlas_bytes();
    for ( u32 i = 0; i < IMGUI_FONT_REGISTRY_MAX; ++i )
        if ( s_fonts[ i ].owns_atlas )
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
    if ( s_active->src == FONT_SRC_TTF )
    {
        for ( u32 i = 0; i < n && str[ i ]; ++i )
        {
            u8 ch = (u8)str[ i ];
            if ( ch >= 32 && ch <= 126 )
                w += (f32)s_active->ttf.lookup[ ch - 32 ].advance;
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
    font_glyph -- per-character draw parameters; dispatches to the active slot's source.

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
    if ( s_active->src == FONT_SRC_TTF )
        ttf_glyph( s_active, ch, u0, v0, u1, v1, ox, oy, gw, gh, advance );
    else
        bmp_glyph( s_active, ch, u0, v0, u1, v1, ox, oy, gw, gh, advance );
}

// clang-format on
/*============================================================================================*/
