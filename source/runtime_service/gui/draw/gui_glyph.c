/*==============================================================================================

    runtime_service/gui/draw/gui_glyph.c -- the render-side glyph surface over the font resource.

    The GUI's render-touching use of the font/ resource: it syncs resident glyph pixels into the
    shared atlas (font_atlas_sync, once per frame at a between-frames latch), dispatches a glyph to
    its atlas UV rect (font_glyph, the render server's reverse-seam consumer), and answers atlas
    queries for a font id.  The parse, metrics, pixel storage, and preset loading are the font/
    resource (font_load / font_load_into / font_load_builtin + the metric readers) -- this file
    never reads a file; it drives the atlas.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Atlas sync -- pack every (re)loaded font's resident pixels into the shared atlas.

    Called once per frame by the UI unit at frame_begin -- a safe point between frames -- so the GPU
    atlas swap (register / upload) never interleaves with an in-flight frame, nor, across the
    multi-context floater pass, with frames still in flight (the VK_ERROR_DEVICE_LOST hazard a
    mid-frame atlas rebuild used to risk).  A font is loaded render-free by the font/ resource
    (parsed, metrics ready, needs_upload set); here its pixels reach the GPU.  Returns true when the
    active font's pixels were among those uploaded -- its metrics may have changed with a reload, so
    the caller rebuilds layout this frame.
==============================================================================================*/

bool
font_atlas_sync( void )
{
    bool active_changed = false;

    for ( u32 id = 0; id < GUI_FONT_REGISTRY_MAX; ++id )
    {
        font_slot_t* slot = font_slot_ptr( id );
        if ( !slot->used || !slot->needs_upload )
            continue;

        if ( !font_slot_upload( slot ) )   /* slot keeps its previous atlas tenant; say so */
        {
            printf( "[gui] WARNING: font atlas upload failed for slot %u\n", id );
            slot->needs_upload = false;    // don't retry a doomed upload every frame
            continue;
        }

        if ( id == font_active_id() )
            active_changed = true;
    }

    return active_changed;
}

/*==============================================================================================
    Atlas queries -- the shared atlas backing a font id (a texture preview reads these).
==============================================================================================*/

/* Bindless index of the atlas backing font id `id` (0 for an empty / out-of-range slot).  Every
   loaded font shares the one resource atlas, so this is the shared bindless slot for any used font.
   A texture preview (sb_gui) draws this index -- it shows the whole shared atlas. */
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
