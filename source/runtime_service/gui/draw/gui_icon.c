/*==============================================================================================

    runtime_service/gui/draw/gui_icon.c -- Runtime icon set.

    Raw monochrome bitmaps registered at runtime (icon_register) and packed into the shared 
    resource atlas (gui_res_atlas.c) as tenants, so icons draw from the same texture -- and 
    batch in the same draw call -- as text and solid fills.  
    
    This file is for resource bookkeeping only: the name table and mapping from gui_icon_id_t
    to a shared-atlas tenant.
    
    The draw entry point, draw_push_icon, lives in pipeline/gui_emit_shape.c and reads 
    icon_get (UVs) + icon_atlas_idx (bindless slot) below.

    Pixel SOURCING is intentionally out of scope: callers supply raw R8 coverage bytes 
    (row-major, w*h, 0..255).  The bytes can be procedural code, the asset/image pipeliner.
    
    The shared atlas owns the resident copy and the deferred GPU upload, so registration 
    stays safe to call mid-frame (the upload lands at the next frame_begin flush).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Icon Defines ---
==============================================================================================*/

#define ICON_MAX                256u     // max distinct icons

/* An SDF icon stores distance-to-edge in texels. ICON_SDF_SPREAD is the maximum
   distance encoded before values clamp to fully-inside (255) or fully-outside (0).

   This sets a balance:

   - Outline limit: Outlines are drawn using this distance field, so an outline 
     cannot be thicker than 4 texels. (Icons only need thin borders, so 4 is plenty.)
   - Edge smoothness: 1 byte provides ~127 distance steps per side. Spreading those 
     over only 4 texels gives ~32 steps per texel, ensuring smooth, band-free antialiasing.
     A larger spread would stretch those 127 steps too thin and cause visible banding. */

#define ICON_SDF_SPREAD         4.0f

/* Default max dimension (in texels) for SDF icons when no size is specified.
   
   - Quality vs. Memory: Large enough to keep sharp corners on toolbars, but small 
     enough to share atlas memory with font glyphs.
   - Exact 64x64 Atlas Tiling: 62 texels + 2px atlas padding = 64x64 cells, perfectly 
     tiling the shared atlas (16 fill 256x256, 128 fill 1024x512) with zero wasted space. */

#define ICON_SDF_SIZE_DEFAULT   62u

/*==============================================================================================
    --- Icon State ---
==============================================================================================*/

/* One registered icon: its name, stored dimensions, and its tenant handle.  UVs are NOT cached --
   they are derived live from the tenant's origin (icon_get), since a repack can move the tenant.

   `sdf` is the fork, and it is per ICON rather than a mode the whole set runs in. 

   The two kinds are genuinely different tools and both are wanted: coverage stays right for 
   pixel-precise art (a 16 px symbol tuned to the grid, anything with 1 px detail or deliberately
   hard corners, which a field would round off), while a field is right for anything drawn at a 
   size other than the one it was baked at, rotated, or wanting an outline.  Mixing them costs 
   nothing because the sampling model rides the VERTEX -- a coverage icon, an SDF icon and an 
   RGBA sprite share one draw call. */

typedef struct
{
    u16  name_off;  // lookup key -- offset into the shared name pool (gui_names.h)
    u16  w, h;      // STORED pixel dimensions (the field's, for an SDF icon)
    u16  tenant;    // packed: bits [0,15) = atlas handle (0 = unused), bit 15 = sdf flag

} icon_entry_t;

/* `tenant` packs the atlas handle and the sdf flag into one u16 instead of a u32 handle plus a
   separate bool -- GUI_RES_ATLAS_MAX_TENANTS is 320, so 15 bits of handle is generous headroom,
   and folding the flag into the spare top bit costs nothing where a standalone bool would have
   cost a full 4 bytes once struct alignment padded it out.  Go through these three helpers rather
   than hand-rolling the mask/shift at each call site. */

#define ICON_SDF_BIT      0x8000u
#define ICON_TENANT_MASK  0x7fffu

static inline u16
icon_pack_tenant( u32 tenant, bool sdf )
{
    return (u16)( tenant | ( sdf ? ICON_SDF_BIT : 0u ) );
}

static inline u32
icon_tenant_value( u16 packed )
{
    return packed & ICON_TENANT_MASK;
}

static inline bool
icon_tenant_sdf( u16 packed )
{
    return ( packed & ICON_SDF_BIT ) != 0;
}

typedef struct
{
    icon_entry_t  entries[ ICON_MAX ];      // id - 1 indexes here
    u32           count;                    // number of registered icons (0..ICON_MAX) 
    bool          ready;                    // registration enabled (shared atlas stood up)

} icon_set_t;

static icon_set_t s_icons;

/*==============================================================================================

    icon_atlas_init / icon_atlas_shutdown -- the icon layer holds no GPU resource of its own;
    it only gates registration. 
    
    The shared resource atlas (res_atlas_init/shutdown) owns the texture.

==============================================================================================*/

bool
icon_atlas_init( void )
{
    memset( &s_icons, 0, sizeof( s_icons ) );
    s_icons.ready = true;
    return true;
}

void
icon_atlas_shutdown( void )
{
    /* The shared name pool (gui_names.h) is reset once, at true gui_shutdown -- not here,
       since icons are only one of several registries interning into it. */
    memset( &s_icons, 0, sizeof( s_icons ) );
}

/*==============================================================================================

    icon_register / icon_register_sdf -- pack one icon and return its id (0 on failure).

    Two entry points over one body.  They differ in which atlas the tenant lands in and what 
    its bytes MEAN, which is the only thing about an icon that forks.
    
    Everything downstream (lookup, UV derivation, the draw quad) is shared, because the 
    sampling model travels in the vertex.

==============================================================================================*/

static gui_icon_id_t
icon_record( const char* name, u32 w, u32 h, u32 tenant, bool sdf )
{
    /* Record a packed tenant under `name`. Both registrars end here; the caller has 
       already chosen the atlas and packed into it. */

    icon_entry_t* e = &s_icons.entries[ s_icons.count ]; 
    memset( e, 0, sizeof( *e ) );
    e->name_off = gui_names_intern( name );
    e->w      = (u16)w;
    e->h      = (u16)h;
    e->tenant = icon_pack_tenant( tenant, sdf );

    /* id = (index + 1); 0 stays reserved for none */

    return (gui_icon_id_t)( ++s_icons.count );   
}

gui_icon_id_t
icon_register( const char* name, u32 w, u32 h, const u8* coverage )
{
    if ( !s_icons.ready || !coverage || w == 0 || h == 0 )
         return GUI_ICON_NONE;

    if ( s_icons.count >= ICON_MAX )
         return GUI_ICON_NONE;

    /* Add the coverage as a tenant of the shared atlas (incremental pack; repack-on-full handled
       inside res_atlas_add).  The shared atlas takes its own copy and owns the deferred upload. */

    u32  tenant = res_atlas_add( coverage, w, h, RES_TENANT_ICON );
    if ( tenant == 0 )
    {
        gui_log( GUI_LOG_WARN, "icon atlas full -- '%s' (%ux%u) rejected", name ? name : "?", w, h );
        return GUI_ICON_NONE;
    }
    return icon_record( name, w, h, tenant, false );
}

/*==============================================================================================

    icon_register_sdf -- convert coverage to a distance field and pack it into the SDF atlas.

    `out_max` is the longest edge of the STORED field (0 takes ICON_SDF_SIZE_DEFAULT). 
    It is never allowed above the source's longest edge: upsampling invents no detail, it only 
    spends atlas. The source SHOULD be several times larger than that -- see gui_icon_sdf.c 
    for why the whole point is transforming at high resolution and storing the field low.

    Note what the id then means downstream: nothing.  A caller draws an SDF icon with the same
    draw_icon_in it always used, and gets resolution independence, free rotation through the
    transform path, and GUI_OP_TEXT_EDGE outlines, without naming any of it.

==============================================================================================*/

gui_icon_id_t
icon_register_sdf( const char* name, u32 w, u32 h, const u8* coverage, u32 out_max )
{
    if ( !s_icons.ready || !coverage || w == 0 || h == 0 )
         return GUI_ICON_NONE;

    if ( s_icons.count >= ICON_MAX )
         return GUI_ICON_NONE;

    u32 longest = ( w > h ) ? w : h;
    if ( out_max == 0 )       out_max = ICON_SDF_SIZE_DEFAULT;
    if ( out_max > longest )  out_max = longest;

    /* Preserve the aspect ratio: the stored field is what icon_size reports and what draw_icon_in
       fits a rect to, so a distorted one would distort every placement of it. */
    u32 ow, oh;
    if ( w >= h )
    {
        ow = out_max;
        oh = ( h * out_max + w / 2 ) / w;
    }
    else
    {
        oh = out_max;
        ow = ( w * out_max + h / 2 ) / h;
    }
    if ( ow == 0 ) ow = 1;
    if ( oh == 0 ) oh = 1;

    if ( sdf_bake_touches_border( coverage, w, h ) )
    {
        gui_log( GUI_LOG_WARN,
                 "icon '%s' reaches the edge of its own bitmap -- its distance field cannot "
                 "fall off there, so that edge draws hard and takes no outline; add a transparent "
                 "margin to the source art", name ? name : "?" );
    }

    u8* field = (u8*)malloc( (size_t)ow * oh );
    if ( !field )
        return GUI_ICON_NONE;
    if ( !sdf_bake_build( coverage, w, h, field, ow, oh, ICON_SDF_SPREAD ) )
    {
        free( field );
        return GUI_ICON_NONE;
    }

    /* The SDF atlas is created lazily by its first add -- an SDF font today, an SDF icon now.  It
       is a separate texture from the coverage atlas for one reason: the sampler is chosen per DRAW
       from the model, and a field must filter LINEAR while coverage must stay NEAREST. */
    u32 tenant = res_sdf_add( field, ow, oh, RES_TENANT_ICON );
    free( field );
    if ( tenant == 0 )
    {
        gui_log( GUI_LOG_WARN, "sdf atlas full -- icon '%s' (%ux%u) rejected",
                 name ? name : "?", ow, oh );
        return GUI_ICON_NONE;
    }
    return icon_record( name, ow, oh, tenant, true );
}

/*==============================================================================================
    Lookup / query
==============================================================================================*/

/* Resolve an id (index + 1) to its entry, or NULL if out of range. */
static const icon_entry_t*
icon_entry( gui_icon_id_t id )
{
    if ( id == GUI_ICON_NONE || id > s_icons.count )
        return NULL;
    return &s_icons.entries[ id - 1 ];
}

gui_icon_id_t
icon_find( const char* name )
{
    if ( !name )
        return GUI_ICON_NONE;
    for ( u32 i = 0; i < s_icons.count; ++i )
        if ( strcmp( gui_names_cstr( s_icons.entries[ i ].name_off ), name ) == 0 )
            return (gui_icon_id_t)( i + 1 );
    return GUI_ICON_NONE;
}

bool
icon_get( gui_icon_id_t id, f32* u0, f32* v0, f32* u1, f32* v1, u32* w, u32* h )
{
    const icon_entry_t* e = icon_entry( id );
    if ( !e )
        return false;

    /* Derive UVs live from the tenant's current origin (a repack can move it), scaled by ITS
       atlas's dimensions -- the same rebase font glyphs use, and the same fork: which atlas holds
       the tenant is a property of the icon, and the two are not the same size. */
    u32 ox, oy;
    f32 iw, ih;
    if ( icon_tenant_sdf( e->tenant ) )
    {
        res_sdf_origin( icon_tenant_value( e->tenant ), &ox, &oy );
        iw = res_sdf_inv_w();
        ih = res_sdf_inv_h();
    }
    else
    {
        res_atlas_origin( icon_tenant_value( e->tenant ), &ox, &oy );
        iw = res_atlas_inv_w();
        ih = res_atlas_inv_h();
    }
    if ( u0 ) *u0 = (f32)ox * iw;
    if ( v0 ) *v0 = (f32)oy * ih;
    if ( u1 ) *u1 = (f32)( ox + e->w ) * iw;
    if ( v1 ) *v1 = (f32)( oy + e->h ) * ih;
    if ( w )  *w  = e->w;
    if ( h )  *h  = e->h;
    return true;
}

/*==============================================================================================
    icon_tex -- the tex_idx an icon quad must carry: its backing atlas's bindless slot with the
    sampling model already in the mode field (gui.h, gui_tex_mode_t).

    Consumed by pipeline/gui_emit_shape.c (draw_push_icon), and deliberately the exact shape of
    font_slot_tex (draw/gui_glyph_internal.c) -- the fork an icon just grew is the fork a font
    already had, so it is worth them reading the same.

    It takes an ID where it used to take nothing, and that is the whole change on this side.  The
    model cannot be a property of the SET because it is a property of the ART: a pixel-tuned 16 px
    symbol and a scalable glyph-like mark want different texel meanings, and both must still land
    in one draw call.  They do, because the batcher keys on nothing here -- the number this returns
    rides the vertex.
==============================================================================================*/

u32
icon_tex( gui_icon_id_t id )
{
    const icon_entry_t* e = icon_entry( id );
    if ( !e )
        return 0u;
    if ( !icon_tenant_sdf( e->tenant ) )
        return res_atlas_idx();

    u32 idx = res_sdf_idx();
    return idx ? ( idx | GUI_TEX_MODE( GUI_TEX_SDF ) ) : 0u;   /* 0 = atlas not up yet; draw skips */
}

/*==============================================================================================
    Debug readout -- consumed by the font overlay's icon footer (gui_frame_overlay.c), same
    per-index accessor shape as font_slot_ptr so a small, cheap-to-copy value comes back per row
    instead of exposing icon_entry_t (and s_icons) outside this file.
==============================================================================================*/

u32
icon_debug_count( void )
{
    return s_icons.count;
}

u32
icon_debug_max( void )
{
    return ICON_MAX;
}

icon_debug_entry_t
icon_debug_entry( u32 index )
{
    icon_debug_entry_t d = { "", 0, 0, false };
    if ( index < s_icons.count )
    {
        const icon_entry_t* e = &s_icons.entries[ index ];
        d.name = gui_names_cstr( e->name_off );
        d.w    = e->w;
        d.h    = e->h;
        d.sdf  = icon_tenant_sdf( e->tenant );
    }
    return d;
}

// clang-format on
/*============================================================================================*/
