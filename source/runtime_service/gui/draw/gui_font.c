/*==============================================================================================

    runtime_service/gui/draw/gui_font.c -- the font unit's load + atlas + glyph API.

    The render-touching public surface: load / reload fonts, the shared-atlas queries, and glyph
    UV dispatch (font_glyph, the render server's reverse-seam consumer).  It drives the registry
    through the text/ leaf (font_alloc_slot / font_slot_ptr / font_activate / font_active_id /
    font_active_slot) and packs pixels through gui_font_internal.c, included right before this file.

    The MEASUREMENT readers (font_char_advance / font_text_w / font_line_h / ...) and the registry
    itself are the text/ leaf (text/gui_font.c) -- measuring text is sizes-and-math, not drawing,
    so it lives at the bottom of the stack, readable by every layer including the interact server.

==============================================================================================*/
// clang-format off

#include "engine/sys/sys_host.h"    // sys_root_dir -- built-in presets resolve root-relative

/*==============================================================================================
    Registry API -- load fonts by id (the registry storage + selection live in the text/ leaf).
==============================================================================================*/

/* Load a font into a new id and activate it.  Returns the id, or 0 on failure (registry full, or
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
    [ GUI_FONT_NONE ]               = NULL,
    [ GUI_FONT_JETBRAINS_16 ]       = "assets/font/JetBrainsMonoNL-Regular_16px.orb_font",
    [ GUI_FONT_ROBOTO_16 ]          = "assets/font/Roboto-Regular_16px.orb_font",
    [ GUI_FONT_CASCADIA_MONO_12 ]   = "assets/font/CascadiaMono_12px.orb_font",
    [ GUI_FONT_CASCADIA_MONO_16 ]   = "assets/font/CascadiaMono_16px.orb_font",
    [ GUI_FONT_CASCADIA_MONO_20 ]   = "assets/font/CascadiaMono_20px.orb_font",
    [ GUI_FONT_CASCADIA_CODE_16 ]   = "assets/font/CascadiaCode_16px.orb_font",
};

/* Relative asset path of a built-in preset; NULL for GUI_FONT_NONE / out-of-range.  The public
   font_load_builtin (gui_frame_font.c) resolves it against the root and loads into a fresh id. */

const char*
font_builtin_rel_path( gui_builtin_font_t font )
{
    if ( font >= ARRAY_COUNT( s_builtin_font_path ) )
        return NULL;
    return s_builtin_font_path[ font ];
}

/* Load a built-in font preset into slot 0 and activate it. A no-op success for GUI_FONT_NONE.
   Called from gui_init() when the host passes a preset. */

bool
font_load_builtin( gui_builtin_font_t font )
{
    if ( font == GUI_FONT_NONE )
        return true;

    const char* rel = font_builtin_rel_path( font );
    if ( rel != NULL )
    {
        /* Built-in presets are engine assets at <root>/assets/font -- resolve against
           sys_root_dir() so hosts work from any working directory. */
        char path[ 576 ];
        fmt_snprintf( path, sizeof( path ), "%s/%s", sys_root_dir(), rel );

        return font_internal_load_into( 0, path );   // slot 0 = the default font
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
        bool ok = font_slot_load( font_slot_ptr( id ), s_reload_q[ i ].path );
        if ( !ok )   /* slot keeps its previous font; say so instead of failing silently */
            printf( "[gui] WARNING: deferred font reload failed for slot %u ('%s')\n",
                    id, s_reload_q[ i ].path );
        s_reload_q[ i ] = ( font_reload_req_t ){ 0 };

        if ( ok && font_active_id() == id )
        {
            font_activate( id );        // metrics rebuilt in place; refresh active pointers
            active_reloaded = true;
        }
    }

    return active_reloaded;
}

/*==============================================================================================
    Atlas queries + glyph dispatch -- the render-side surface (the shared atlas + UV lookup).
==============================================================================================*/

/* Bindless index of the atlas backing font id `id` (0 for an empty / out-of-range slot).  Every
   loaded font shares the one resource atlas, so this is the shared bindless slot for any used
   font.  A texture preview (sb_gui) draws this index -- it shows the whole shared atlas. */
u32
font_slot_atlas_idx( u32 id )
{
    font_slot_t* slot = font_slot_ptr( id );
    if ( !slot || !slot->used )
        return 0;
    return res_atlas_idx();
}

/* Pixel dimensions of the atlas backing font id `id` (0,0 for an empty / out-of-range slot) -- the
   shared resource atlas dimensions.  A texture preview needs size alongside font_slot_atlas_idx. */
gui_vec2_t
font_slot_atlas_size( u32 id )
{
    font_slot_t* slot = font_slot_ptr( id );
    if ( !slot || !slot->used )
        return ( gui_vec2_t ){ 0.0f, 0.0f };
    return ( gui_vec2_t ){ (f32)GUI_RES_ATLAS_W, (f32)GUI_RES_ATLAS_H };
}

/*==============================================================================================
    font_glyph -- per-character draw parameters for the active font (the render server's reverse
    seam, declared in render/gui_render.h): atlas UV rect, bearing offsets, glyph size, advance.
==============================================================================================*/

void
font_glyph( u8 ch,
            f32* u0, f32* v0, f32* u1, f32* v1,
            f32* ox, f32* oy, f32* gw, f32* gh,
            f32* advance )
{
    font_slot_glyph( font_active_slot(), ch, u0, v0, u1, v1, ox, oy, gw, gh, advance );
}

// clang-format on
/*============================================================================================*/
