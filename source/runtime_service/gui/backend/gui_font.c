/*==============================================================================================

    runtime_service/gui/backend/gui_font.c -- The font unit's public API (gui_backend.h).

    Every function here is what gui.c (the UI unit) is allowed to call.  Each one is either a
    trivial read of the active-font state (s_font / s_active, set by font_activate() in
    gui_font_internal.c) or a one-line forward to a font_internal_* function.  All the real logic
    -- the registry, the reload queue, the .orb_font loader -- lives in gui_font_internal.c,
    included right before this file so its statics and font_internal_* functions are already in
    scope here.

    Included by gui_backend.c after gui_font_internal.c.

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
    return font_internal_load( path );
}

/* Load a font into an existing id (id 0 swaps the default; a slot already in use defers the swap
   to the next frame_begin flush).  Returns false on bad id. */
bool
font_load_into( u32 id, const char* path )
{
    return font_internal_load_into( id, path );
}

/* Load a built-in font preset (gui_builtin_font_t, gui.h) into slot 0.  True no-op for
   GUI_FONT_NONE; called from gui_init() when the host passes a preset. */
bool
font_load_builtin( gui_builtin_font_t font )
{
    return font_internal_load_builtin( font );
}

/* Commit every queued deferred reload.  Called once per frame by the UI unit at frame_begin.
   Returns true when a committed load changed the active font. */
bool
font_flush_pending( void )
{
    return font_internal_flush_pending();
}

/* Make an already-loaded id the active font.  Ignored if the id is empty or out of range. */
void
font_use( u32 id )
{
    font_internal_use( id );
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

/* Pixel width of the first n characters of str (see font_internal_text_w_n). */
f32
font_text_w_n( const char* str, u32 n )
{
    return font_internal_text_w_n( str, n );
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
