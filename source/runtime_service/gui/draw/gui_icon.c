/*==============================================================================================

    runtime_service/gui/draw/gui_icon.c -- Runtime icon set.

    Raw monochrome bitmaps registered at runtime (icon_register) and packed into the shared resource
    atlas (gui_res_atlas.c) as tenants, so icons draw from the same texture -- and batch in the same
    draw call -- as text and solid fills.  This file is resource bookkeeping only: the name table and
    the mapping from gui_icon_id_t to a shared-atlas tenant.  The draw entry point, draw_push_icon,
    lives in pipeline/gui_emit_draw.c and reads icon_get (UVs) + icon_atlas_idx (bindless slot) below.

    Pixel SOURCING is intentionally out of scope: callers supply raw R8 coverage bytes (row-major,
    w*h, 0..255).  Whoever has the bytes -- procedural code today, the asset/image pipeline later --
    feeds them in.  The shared atlas owns the resident copy and the deferred GPU upload, so
    registration stays safe to call mid-frame (the upload lands at the next frame_begin flush).

    Included by gui_draw.c after the glyph pair, both being tenants of the same atlas.  The atlas
    itself is the render server's (gui_res_atlas.c, another TU), reached through its header.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Sizes
==============================================================================================*/

#define ICON_MAX       256u     // max distinct icons

/*==============================================================================================
    State
==============================================================================================*/

/* One registered icon: its name, stored dimensions, and its tenant handle.  UVs are NOT cached --
   they are derived live from the tenant's origin (icon_get), since a repack can move the tenant.

   `sdf` is the fork, and it is per ICON rather than a mode the whole set runs in.  The two kinds
   are genuinely different tools and both are wanted: coverage stays right for pixel-precise art
   (a 16 px symbol tuned to the grid, anything with 1 px detail or deliberately hard corners, which
   a field would round off), while a field is right for anything drawn at a size other than the one
   it was baked at, rotated, or wanting an outline.  Mixing them costs nothing because the sampling
   model rides the VERTEX -- a coverage icon, an SDF icon and an RGBA sprite share one draw call. */
typedef struct
{
    char name[ 32 ];        // lookup key (NUL-terminated, truncated to 31 chars)
    u16  w, h;              // STORED pixel dimensions (the field's, for an SDF icon)
    u32  tenant;            // handle into its atlas (0 = unused)
    bool sdf;               // false = shared coverage atlas, true = the SDF atlas

} icon_entry_t;

typedef struct
{
    icon_entry_t  entries[ ICON_MAX ];      // id - 1 indexes here
    u32           count;
    bool          ready;                    // registration enabled (shared atlas stood up)

} icon_set_t;

static icon_set_t s_icons;

/*==============================================================================================
    icon_atlas_init / icon_atlas_shutdown -- the icon layer holds no GPU resource of its own; it
    only gates registration.  The shared resource atlas (res_atlas_init/shutdown) owns the texture.
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
    memset( &s_icons, 0, sizeof( s_icons ) );
}

/*==============================================================================================
    icon_register / icon_register_sdf -- pack one icon and return its id (0 on failure).

    Two entry points over one body.  They differ in which atlas the tenant lands in and what its
    bytes MEAN, which is the only thing about an icon that forks -- everything downstream (lookup,
    UV derivation, the draw quad) is shared, because the sampling model travels in the vertex.
==============================================================================================*/

/* Record a packed tenant under `name`.  Both registrars end here; the caller has already chosen
   the atlas and packed into it. */
static gui_icon_id_t
icon_record( const char* name, u32 w, u32 h, u32 tenant, bool sdf )
{
    icon_entry_t* e = &s_icons.entries[ s_icons.count ];
    memset( e, 0, sizeof( *e ) );
    if ( name )
    {
        u32 i = 0;
        for ( ; i < sizeof( e->name ) - 1 && name[ i ]; ++i )
            e->name[ i ] = name[ i ];
        e->name[ i ] = '\0';
    }
    e->w      = (u16)w;
    e->h      = (u16)h;
    e->tenant = tenant;
    e->sdf    = sdf;

    return (gui_icon_id_t)( ++s_icons.count );   /* id = (index + 1); 0 stays reserved for none */
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
    u32 tenant = res_atlas_add( coverage, w, h );
    if ( tenant == 0 )
    {
        gui_log( GUI_LOG_WARN, "icon atlas full -- '%s' (%ux%u) rejected", name ? name : "?", w, h );
        return GUI_ICON_NONE;
    }
    return icon_record( name, w, h, tenant, false );
}

/*==============================================================================================
    icon_register_sdf -- convert coverage to a distance field and pack it into the SDF atlas.

    `out_max` is the longest edge of the STORED field (0 takes ICON_SDF_SIZE_DEFAULT).  It is never
    allowed above the source's longest edge: upsampling invents no detail, it only spends atlas.
    The source SHOULD be several times larger than that -- see gui_icon_sdf.c for why the whole
    point is transforming at high resolution and storing the field low.

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

    if ( icon_sdf_touches_border( coverage, w, h ) )
    {
        gui_log( GUI_LOG_WARN,
                 "icon '%s' reaches the edge of its own bitmap -- its distance field cannot "
                 "fall off there, so that edge draws hard and takes no outline; add a transparent "
                 "margin to the source art", name ? name : "?" );
    }

    u8* field = (u8*)malloc( (size_t)ow * oh );
    if ( !field )
        return GUI_ICON_NONE;
    if ( !icon_sdf_build( coverage, w, h, field, ow, oh ) )
    {
        free( field );
        return GUI_ICON_NONE;
    }

    /* The SDF atlas is created lazily by its first add -- an SDF font today, an SDF icon now.  It
       is a separate texture from the coverage atlas for one reason: the sampler is chosen per DRAW
       from the model, and a field must filter LINEAR while coverage must stay NEAREST. */
    u32 tenant = res_sdf_add( field, ow, oh );
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
    /* Compare within the stored capacity: names register truncated to 31 chars, so a longer
       query must match by the same rule or a registered icon becomes unfindable by its own name. */
    for ( u32 i = 0; i < s_icons.count; ++i )
        if ( strncmp( s_icons.entries[ i ].name, name, sizeof( s_icons.entries[ i ].name ) - 1 ) == 0 )
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
    if ( e->sdf )
    {
        res_sdf_origin( e->tenant, &ox, &oy );
        iw = res_sdf_inv_w();
        ih = res_sdf_inv_h();
    }
    else
    {
        res_atlas_origin( e->tenant, &ox, &oy );
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

    Consumed by pipeline/gui_emit_draw.c (draw_push_icon), and deliberately the exact shape of
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
    if ( !e->sdf )
        return res_atlas_idx();

    u32 idx = res_sdf_idx();
    return idx ? ( idx | GUI_TEX_MODE( GUI_TEX_SDF ) ) : 0u;   /* 0 = atlas not up yet; draw skips */
}

// clang-format on
/*============================================================================================*/
