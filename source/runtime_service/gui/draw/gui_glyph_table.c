/*==============================================================================================

    runtime_service/gui/draw/gui_glyph_table.c -- the resident glyph UV table.

    One entry per glyph the font registry can address, holding the atlas UV rect the tessellator
    used to compute per character.  The tessellator emits a stable ID instead; the vertex stage
    reads the rect from this table.

    Two things fall out of that, and both are the point:

      * Per-glyph UV math leaves the tessellator.  font_slot_glyph resolves a tenant page origin
        and does four multiplies against the atlas inverse dimensions for EVERY character of EVERY
        re-tessellated run.  Those are a pure function of (registry slot, glyph, atlas generation),
        so they belong here, computed once per atlas change.

      * Cached geometry survives an atlas repack.  A quad holding a baked UV goes stale the moment
        a tenant's page origin moves; a quad holding an ID does not, because the table rewrites in
        place and IDs never move.  That is why an ID is (slot, glyph) only -- it must not encode
        anything about where the pixels currently sit.

    ID layout: `slot_id * GUI_GLYPH_SLOT_STRIDE + glyph_index`, a FIXED stride rather than packed
    per-slot bases.  Packing would be smaller, and would shift every later slot's base when a font
    loads or is released -- invalidating IDs already baked into retained window geometry.  A fixed
    stride costs ~48 KB and makes an ID depend on nothing that moves.

    Included by gui_draw.c after gui_glyph_internal.c: it needs font_slot_glyph's sibling lookup
    rule (font_slot_cp) and the shared atlas readers.

==============================================================================================*/
// clang-format off

/* GUI_GLYPH_SLOT_STRIDE / GUI_GLYPH_TABLE_MAX are in gui.h beside gui_glyph_uv_t, because the
   render unit sizes its upload region with them.  The registry bound they assume is checked here,
   where the fill loop that depends on it lives. */
ORB_STATIC_ASSERT( GUI_GLYPH_TABLE_MAX == GUI_FONT_REGISTRY_MAX * GUI_GLYPH_SLOT_STRIDE,
                   "glyph table must hold exactly one stride per font registry slot" );

/* Extended codepoints that fit past the dense ASCII block in one slot's stride. */
#define GUI_GLYPH_EXT_MAX       ( GUI_GLYPH_SLOT_STRIDE - ORB_FONT_CP_COUNT )

static struct
{
    gui_glyph_uv_t uv[ GUI_GLYPH_TABLE_MAX ];   /* the table itself, ID-indexed        */
    u32            generation;                  /* bumps on every rebuild (0 = never built) */
    u32            atlas_gen, sdf_gen;          /* atlas generations the table was built against */
    u32            used;                        /* entries through the highest resident slot's
                                                   stride -- HIGH-WATER, never shrinks in a run:
                                                   IDs baked into retained geometry may outlive
                                                   the font that minted them, and must stay
                                                   inside whatever buffer the render unit sized
                                                   from this */
    bool           dirty;                       /* a font's tenant changed since the last build  */

} s_glyph_tab;

/*==============================================================================================
    glyph_table_index -- a codepoint's slot-local table index.

    The SAME resolution rule as font_slot_cp, which is why it lives beside it rather than being
    re-derived at the call site: a glyph whose UVs come from one record and whose ID names another
    renders as the wrong character, silently.  ASCII maps to its dense position, extended maps
    past the dense block in ext[] order, and a miss lands on '?' exactly as the record lookup does.
==============================================================================================*/

static u32
glyph_table_index( const font_slot_t* slot, u32 cp )
{
    if ( cp - ORB_FONT_CP_FIRST < ORB_FONT_CP_COUNT )
        return cp - ORB_FONT_CP_FIRST;

    u32 lo = 0, hi = slot->ext_count;
    while ( lo < hi )
    {
        u32 mid = ( lo + hi ) >> 1;
        u32 c   = slot->ext[ mid ].codepoint;
        if ( c == cp )
            return ( mid < GUI_GLYPH_EXT_MAX ) ? ORB_FONT_CP_COUNT + mid
                                               : (u32)'?' - ORB_FONT_CP_FIRST;
        if ( c < cp ) lo = mid + 1;
        else          hi = mid;
    }
    return (u32)'?' - ORB_FONT_CP_FIRST;
}

/* Fill one slot's block.  A slot with no tenant leaves its entries zeroed: font_slot_glyph
   already draws nothing in that state (the pixels are not up yet), and the tessellator skips
   those glyphs, so a zero rect is never sampled. */
static void
glyph_table_fill_slot( u32 slot_id )
{
    gui_glyph_uv_t* out  = &s_glyph_tab.uv[ slot_id * GUI_GLYPH_SLOT_STRIDE ];
    font_slot_t*    slot = font_slot_ptr( slot_id );

    memset( out, 0, GUI_GLYPH_SLOT_STRIDE * sizeof( gui_glyph_uv_t ) );

    if ( !slot || !slot->used || slot->atlas_tenant == 0 )
        return;

    /* Which atlas this slot's pixels live in -- the same split font_slot_upload made when it
       packed, and the same one font_slot_glyph reads. */
    u32 px, py;
    f32 iw, ih;
    if ( slot->sdf_range )
    {
        res_sdf_origin( slot->atlas_tenant, &px, &py );
        iw = res_sdf_inv_w();
        ih = res_sdf_inv_h();
    }
    else
    {
        res_atlas_origin( slot->atlas_tenant, &px, &py );
        iw = res_atlas_inv_w();
        ih = res_atlas_inv_h();
    }

    u32 ext_n = slot->ext_count < GUI_GLYPH_EXT_MAX ? slot->ext_count : GUI_GLYPH_EXT_MAX;
    if ( slot->ext_count > GUI_GLYPH_EXT_MAX )
        GUI_WARN_ONCE( "glyph table: font slot %u has %u extended glyphs, stride holds %u -- "
                       "the overflow renders as '?'\n", slot_id, slot->ext_count, GUI_GLYPH_EXT_MAX );

    for ( u32 i = 0; i < ORB_FONT_CP_COUNT + ext_n; ++i )
    {
        const orb_font_glyph_t* g = ( i < ORB_FONT_CP_COUNT )
                                  ? &slot->lookup[ i ]
                                  : &slot->ext[ i - ORB_FONT_CP_COUNT ];

        f32 u0 = (f32)( px + g->atlas_x ) * iw;
        f32 v0 = (f32)( py + g->atlas_y ) * ih;

        out[ i ].uv0 = gui_uv_pack( u0, v0 );
        out[ i ].uv1 = gui_uv_pack( u0 + (f32)g->w * iw, v0 + (f32)g->h * ih );
    }
}

/*==============================================================================================
    Public seam -- the tessellator resolves IDs, the render unit uploads the table.
==============================================================================================*/

/* A font's pixels entered, moved between atlases, or left.  Cheaper than comparing every slot's
   tenant each frame, and the upload path is the only thing that can change one. */
void
glyph_table_mark_dirty( void )
{
    s_glyph_tab.dirty = true;
}

/*  Rebuild if anything the entries depend on moved: a tenant change (dirty) or an atlas
    generation bump, which is what a repack or a grow raises.  Called once per frame before
    tessellation, so the IDs a run emits and the rects behind them come from one build. */
void
glyph_table_sync( void )
{
    u32 ag = res_atlas_generation();
    u32 sg = res_sdf_generation();

    if ( s_glyph_tab.generation != 0 && !s_glyph_tab.dirty
      && s_glyph_tab.atlas_gen == ag && s_glyph_tab.sdf_gen == sg )
        return;

    u32 top = 0;
    for ( u32 i = 0; i < GUI_FONT_REGISTRY_MAX; ++i )
    {
        glyph_table_fill_slot( i );
        const font_slot_t* slot = font_slot_ptr( i );
        if ( slot && slot->used && slot->atlas_tenant )
            top = i + 1;
    }
    if ( top * GUI_GLYPH_SLOT_STRIDE > s_glyph_tab.used )
        s_glyph_tab.used = top * GUI_GLYPH_SLOT_STRIDE;

    s_glyph_tab.atlas_gen  = ag;
    s_glyph_tab.sdf_gen    = sg;
    s_glyph_tab.dirty      = false;
    ++s_glyph_tab.generation;
}

/* Table ID for a codepoint in the ACTIVE font slot -- what the tessellator bakes into a glyph
   quad in place of its UV rect. */
u32
font_glyph_id( u32 cp )
{
    return font_active_id() * GUI_GLYPH_SLOT_STRIDE
         + glyph_table_index( font_active_slot(), cp );
}

/*==============================================================================================
    font_glyph_placed -- the table path's per-character dispatch: ID plus placement, ONE lookup.

    What font_glyph does minus the part the table now owns.  A table-addressed quad needs the
    glyph's box and advance but not its atlas rect, and it needs the record's INDEX -- which the
    same search already produced, so resolving the two separately would walk ext[] twice per
    character on the hottest loop in the tessellator.

    gw/gh come back 0 when the slot has no tenant, the same "draw nothing, keep the layout" answer
    font_glyph gives: the pixels are not up yet, and the tessellator's size gate drops the quad.
==============================================================================================*/

void
font_glyph_placed( u32 cp, u32* id, f32* ox, f32* oy, f32* gw, f32* gh, f32* advance )
{
    const font_slot_t*      slot = font_active_slot();
    u32                     gi   = glyph_table_index( slot, cp );
    const orb_font_glyph_t* g    = ( gi < ORB_FONT_CP_COUNT )
                                 ? &slot->lookup[ gi ]
                                 : &slot->ext[ gi - ORB_FONT_CP_COUNT ];

    *id      = font_active_id() * GUI_GLYPH_SLOT_STRIDE + gi;
    *ox      = (f32)g->bearing_x;
    *oy      = (f32)( slot->ascent - (i32)g->bearing_y );
    *gw      = slot->atlas_tenant ? (f32)g->w : 0.0f;
    *gh      = slot->atlas_tenant ? (f32)g->h : 0.0f;
    *advance = (f32)g->advance;
}

/* The table as the render unit uploads it: a flat ID-indexed array of packed UV pairs.  The
   generation is the upload's staleness key -- it changes only on a rebuild, so a surface that
   already holds this generation has nothing to send.  glyph_table_used is what the render unit
   SIZES its buffer by: the high-water extent, one stride minimum so the buffer exists before
   the first font resolves.  It can only grow on a rebuild, so a generation match also means the
   sized buffer still covers every ID in play. */
const gui_glyph_uv_t* glyph_table_data      ( void ) { return s_glyph_tab.uv; }
u32                   glyph_table_count     ( void ) { return GUI_GLYPH_TABLE_MAX; }
u32                   glyph_table_generation( void ) { return s_glyph_tab.generation; }
u32                   glyph_table_used      ( void )
{
    return s_glyph_tab.used ? s_glyph_tab.used : (u32)GUI_GLYPH_SLOT_STRIDE;
}

// clang-format on
/*============================================================================================*/
