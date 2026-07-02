/*==============================================================================================

    runtime_service/gui/backend/gui_font.c -- The font unit's public API (gui_backend.h).

    Every function here is what gui.c (the UI unit) is allowed to call.  gui_font_internal.c,
    included right before this file, holds the registry state and the loader; this file calls
    straight into its statics (s_fonts, s_active, s_font, ...) and building-block functions
    (font_slot_load, font_activate, font_alloc_slot, ...) since it's the same translation unit.
    Only logic shared by more than one public function (font_internal_load_into, needed by both
    font_load_into and font_load_builtin) got pulled out as a named internal function -- anything
    used by exactly one function here is written directly in that function's body.

==============================================================================================*/
// clang-format off

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

    if ( !font_slot_load( &s_fonts[ id ], path ) )
        return 0;

    font_activate( id );
    return id;
}

/* Load a font into an existing id (id 0 swaps the default; a slot already in use defers the swap
   to the next frame_begin flush).  Returns false on bad id. */

bool
font_load_into( u32 id, const char* path )
{
    return font_internal_load_into( id, path );
}

/* Path of every gui_builtin_font_t preset (gui.h), indexed by the enum; NULL for GUI_FONT_NONE. */

static const char* s_builtin_font_path[] = 
{
    [ GUI_FONT_NONE ]         = NULL,
    [ GUI_FONT_JETBRAINS_16 ] = "assets/font/jetbrains_regular_16.orb_font",
};

/* Load a built-in font preset into slot 0 and activate it. A no-op success for GUI_FONT_NONE.
   (the caller loads its own font); called from gui_init() when the host passes a preset. */

bool
font_load_builtin( gui_builtin_font_t font )
{
    if ( font == GUI_FONT_NONE )
        return true;

    bool valid_slot = font < ARRAY_COUNT( s_builtin_font_path );
    bool valud_font = s_builtin_font_path[ font ] != NULL;

    if ( valid_slot && valud_font )
    {
        // slot 0 = the default font
        return font_internal_load_into( 0, s_builtin_font_path[ font ] );          
    }

    return false;    
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

/* True once a font has been activated (s_font set by font_activate) and every metrics/glyph
   accessor below is safe to call.  False from init() until either a built-in preset or the
   caller's own font_load() succeeds -- s_font stays NULL until then, so layout code that needs
   type metrics (gui_style_apply -> layout_compute) must gate on this rather than call in blind. */
bool
font_valid( void )
{
    return s_font != NULL;
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

/*==============================================================================================
    Dispatch helpers -- read from s_font / s_active, set by font_activate() (gui_font_internal.c).
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

/* Pixel width of a NUL-terminated run. */
f32
font_text_w( const char* str )
{
    return font_text_w_n( str, 0xFFFFFFFFu );
}

/*==============================================================================================
    font_glyph -- per-character draw parameters.

    Outputs:
        u0..v1   atlas UV rect for the glyph bitmap
        ox, oy   pixel offsets from (cursor_x, text_y) to the top-left of the bitmap
        gw, gh   pixel dimensions of the bitmap to draw (0 for invisible glyphs like space)
        advance  horizontal cursor advance in pixels
==============================================================================================*/

void
font_glyph( u8 ch,
            f32* u0, f32* v0, f32* u1, f32* v1,
            f32* ox, f32* oy, f32* gw, f32* gh,
            f32* advance )
{
    font_slot_glyph( s_active, ch, u0, v0, u1, v1, ox, oy, gw, gh, advance );
}

// clang-format on
/*============================================================================================*/
