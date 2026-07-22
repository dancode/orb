/*==============================================================================================

    runtime_service/gui/draw/gui_glyph_internal.c -- glyph atlas upload + UV dispatch (render-side).

    The render-touching half of the font system: it takes a font resource's resident R8 pixels
    (font/ leaf, slot->pixels) and packs them into the shared resource atlas (gui_res_atlas.c),
    writing the opaque tenant handle back into the slot; and it maps a glyph to its atlas UV rect
    (font_slot_glyph).  The parse, metrics, and pixel storage are the font/ resource -- nothing here
    reads a file; this file only moves resident pixels to the GPU atlas and dispatches UVs.

    Included by gui_draw.c after gui_draw.h (-> font/gui_font.h types) + gui_res_atlas.h (the shared
    atlas), before gui_glyph.c.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    font_slot_upload -- pack a slot's resident glyph pixels into the shared resource atlas.

    A fresh slot adds a new tenant; a reloaded slot (already has a tenant) updates it in place -- the
    persistent texture and stable bindless slot mean no per-frame create/destroy churn and no device
    drain here (the caller runs this from frame_begin, a safe between-frames point).  Clears
    needs_upload on success; on failure the slot keeps its previous atlas tenant intact.
==============================================================================================*/

static bool
font_slot_upload( font_slot_t* slot )
{
    if ( !slot->pixels )
        return false;

    u32 tenant;
    if ( slot->atlas_tenant )
    {
        if ( !res_atlas_update( slot->atlas_tenant, slot->pixels, slot->atlas_w, slot->atlas_h ) )
            return false;   // slot keeps its previous tenant intact
        tenant = slot->atlas_tenant;
    }
    else
    {
        tenant = res_atlas_add( slot->pixels, slot->atlas_w, slot->atlas_h );
        if ( tenant == 0 )
            return false;
    }

    slot->atlas_tenant = tenant;
    slot->needs_upload = false;
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
    BACKEND-INTERNAL -- module lifecycle, called from gui_draw.c (gui_draw_boot / shutdown).
==============================================================================================*/

static void
font_shutdown( void )
{
    /* Clear the CPU font registry (font/ resource -- frees resident pixels).  Fonts own no GPU
       resource of their own -- their atlas pixel copy lives in the shared resource atlas, torn down
       once by res_atlas_shutdown (gui_backend_exit). */
    font_registry_reset();
}

static bool
font_init( void )
{
    /* Deliberately a no-op, not a placeholder: font_init exists as the paired bookend to
       font_shutdown but has nothing to allocate.  A font atlas needs actual glyph pixels from an
       .orb_font, which only font_load / font_load_into supply.  Slot 0 starts empty; font_valid()
       (font/ resource) reports that until the host's own load activates one. */
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
