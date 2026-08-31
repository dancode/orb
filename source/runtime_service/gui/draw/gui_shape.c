/*==============================================================================================

    runtime_service/gui/draw/gui_shape.c -- Baked SDF shapes.

    Authored art turned into a distance FIELD and packed into the SDF atlas, so the effect 
    band reaches silhouettes no closed-form expression describes.  A keyhole, a gear, a badge
    outline wearing the same border, glow, inset, swell and subtraction a rounded box wears 
    -- which is the whole point, because the record's own subtraction tops out at one 
    rounded-box cut and no stack of quads can un-paint ink.

    This file is resource bookkeeping only: the name table, and the mapping from gui_shape_id_t
    to an SDF-atlas tenant plus the geometry the draw side needs.  The conversion is 
    gui_sdf_bake.c's and the draw entry is the ambient shape lane on GUI_CMD_FX_BOX 
    (pipeline/gui_emit_fx.c).

    THE SPLIT THAT MATTERS is which of the two accessors a caller reaches for, because they 
    differ in what a repack does to them:

        shape_metrics  DIMENSIONS -- the tenant's size and where the ink sits in it.  Stable 
                       across a repack, so the EMIT side reads it to inflate the caller's ink 
                       rect into the padded box the quad must cover.

        shape_uv       PLACEMENT -- derived live from the tenant origin, which a repack does 
                       move. The TESSELLATOR reads it, the way sprite_get is read, and 
                       res_sdf_generation folds into the window hash to force that re-resolve.

    Pixel SOURCING is out of scope, exactly as it is for icons: callers supply R8 coverage bytes
    (row-major, w*h, 0..255) from a rasterizer, a PNG, or procedural code.

    Included by gui_draw.c after gui_icon_load.c -- it needs gui_sdf_bake.c ahead of it and
    nothing else from the unit.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Sizes
==============================================================================================*/

#define SHAPE_MAX      128u     // max distinct baked shapes

/*==============================================================================================
    State
==============================================================================================*/

/* One registered shape.  UVs are NOT cached -- they are derived live from the tenant's origin
   (shape_uv), since a repack can move it.  Everything here is in stored TEXELS; the draw side
   scales by whatever rect it is asked to fill. */

typedef struct
{
    u16  name_off;      // lookup key -- offset into the shared name pool (gui_names.h)
    u16  w, h;          // the padded tenant: ink plus its margin on all four sides
    u16  ink_x, ink_y;  // the art's box inside that tenant
    u16  ink_w, ink_h;  // the art's size inside that tenant
    u16  tenant;        // handle into the SDF atlas (0 = unused)
    f32  spread;        // texels of field either side of the outline the tenant ACTUALLY holds
                        //   -- the KEEP policy can cap this below what was asked for

} shape_entry_t;

static struct
{
    shape_entry_t   entries[ SHAPE_MAX ];
    u32             count;
    bool            ready;  // true after shape_init, false after shape_shutdown

} s_shapes;

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static void
shape_init( void )
{
    memset( &s_shapes, 0, sizeof( s_shapes ) );
    s_shapes.ready = true;
}

static void
shape_shutdown( void )
{
    /* The tenants belong to the atlas and go with it (res_atlas_shutdown); the shared name pool
       (gui_names.h) is reset once, at true gui_shutdown, since shapes are only one of several
       registries interning into it -- so forgetting the entries is the whole teardown here. */

    memset( &s_shapes, 0, sizeof( s_shapes ) );
    s_shapes.ready = false;
}

/*==============================================================================================
    shape_register -- bake one coverage bitmap into the SDF atlas and return its id (0 on failure).

    `bake` NULL takes every default: GUI_SHAPE_SIZE_DEFAULT texels of ink, GUI_SHAPE_SPREAD texels
    of margin, and the GROW policy that makes that margin rather than hoping the art brought one.
==============================================================================================*/

gui_shape_id_t
shape_register( const char* name, u32 w, u32 h, const u8* coverage, const gui_shape_bake_t* bake )
{
    if ( !s_shapes.ready || !coverage || w == 0 || h == 0 )
        return GUI_SHAPE_NONE;
    if ( s_shapes.count >= SHAPE_MAX )
    {
        gui_log( GUI_LOG_WARN, "shape table full (%u) -- '%s' rejected",
                 (unsigned)SHAPE_MAX, name ? name : "?" );
        return GUI_SHAPE_NONE;
    }

    u32           out_max = ( bake && bake->out_max ) ? bake->out_max : (u32)GUI_SHAPE_SIZE_DEFAULT;
    f32           spread  = ( bake && bake->spread > 0.0f ) ? bake->spread : GUI_SHAPE_SPREAD;
    gui_sdf_pad_t policy  = bake ? bake->pad : GUI_SDF_PAD_GROW;

    sdf_bake_out_t out;
    if ( !sdf_bake_shape( coverage, w, h, out_max, spread, policy, &out ) )
    {
        gui_log( GUI_LOG_WARN, "shape '%s' has no ink, or its bake ran out of memory",
                 name ? name : "?" );
        return GUI_SHAPE_NONE;
    }

    /* A KEEP import whose art sits closer to its own edge than the spread asked for: the field is
       only as deep as the margin, so say so with the number rather than letting shape_reach quote
       a reach the texels do not hold. */
    if ( out.spread < spread )
    {
        gui_log( GUI_LOG_WARN,
                 "shape '%s' imported with %.1f texels of margin but %.1f were asked for -- "
                 "borders and glows stop at %.1f; pad the source art or bake with GUI_SDF_PAD_GROW",
                 name ? name : "?", (double)out.spread, (double)spread, (double)out.spread );
    }

    /* The SDF atlas is created lazily by its first add -- an SDF font, an SDF icon, a shape.  It is
       a separate texture from the coverage atlas for one reason: the sampler is chosen per DRAW
       from the model, and a field must filter LINEAR while coverage must stay NEAREST. */
    u32 tenant = res_sdf_add( out.field, out.full_w, out.full_h, RES_TENANT_SHAPE );
    free( out.field );
    if ( tenant == 0 )
    {
        gui_log( GUI_LOG_WARN, "sdf atlas full -- shape '%s' (%ux%u) rejected",
                 name ? name : "?", out.full_w, out.full_h );
        return GUI_SHAPE_NONE;
    }

    shape_entry_t* e = &s_shapes.entries[ s_shapes.count ];
    memset( e, 0, sizeof( *e ) );
    e->name_off = gui_names_intern( name );
    e->w      = (u16)out.full_w;
    e->h      = (u16)out.full_h;
    e->ink_x  = (u16)out.ink_x;
    e->ink_y  = (u16)out.ink_y;
    e->ink_w  = (u16)out.ink_w;
    e->ink_h  = (u16)out.ink_h;
    e->spread = out.spread;
    e->tenant = (u16)tenant;

    return (gui_shape_id_t)( ++s_shapes.count );   /* id = (index + 1); 0 stays reserved for none */
}

/*==============================================================================================
    Lookup / query
==============================================================================================*/

/* Resolve an id (index + 1) to its entry, or NULL if out of range. */
static const shape_entry_t*
shape_entry( gui_shape_id_t id )
{
    if ( id == GUI_SHAPE_NONE || id > s_shapes.count )
        return NULL;
    return &s_shapes.entries[ id - 1u ];
}

gui_shape_id_t
shape_find( const char* name )
{
    if ( !name )
        return GUI_SHAPE_NONE;
    for ( u32 i = 0; i < s_shapes.count; ++i )
        if ( strcmp( gui_names_cstr( s_shapes.entries[ i ].name_off ), name ) == 0 )
            return (gui_shape_id_t)( i + 1 );
    return GUI_SHAPE_NONE;
}

/*==============================================================================================
    shape_metrics -- the tenant's DIMENSIONS and the ink's box inside it, in stored texels.

    Read at EMIT time.  None of these move under an atlas repack (only the origin does), so a
    command that bakes a rect derived from them stays correct however the atlas is reshuffled --
    which is exactly why the split with shape_uv below exists.  A NULL out-param is skipped.
==============================================================================================*/

bool
shape_metrics( gui_shape_id_t id, u32* ink_x, u32* ink_y, u32* ink_w, u32* ink_h,
               u32* full_w, u32* full_h, f32* spread )
{
    const shape_entry_t* e = shape_entry( id );
    if ( !e )
        return false;

    if ( ink_x  ) *ink_x  = e->ink_x;
    if ( ink_y  ) *ink_y  = e->ink_y;
    if ( ink_w  ) *ink_w  = e->ink_w;
    if ( ink_h  ) *ink_h  = e->ink_h;
    if ( full_w ) *full_w = e->w;
    if ( full_h ) *full_h = e->h;
    if ( spread ) *spread = e->spread;
    return true;
}

/*==============================================================================================
    shape_uv / shape_tex -- the PLACEMENT half, resolved at flush time.

    The uv spans the WHOLE padded tenant, not the ink: the fragment needs the margin to be
    reachable or every effect stops at the outline, and the quad the emit side built covers exactly
    that span.  Derived live from the tenant origin because a repack moves it (res_sdf_generation
    folds into the window hash to force the re-resolve, gui_build_diff.c).
==============================================================================================*/

bool
shape_uv( gui_shape_id_t id, f32* u0, f32* v0, f32* u1, f32* v1 )
{
    const shape_entry_t* e = shape_entry( id );
    if ( !e )
        return false;

    u32 ox, oy;
    res_sdf_origin( e->tenant, &ox, &oy );
    f32 iw = res_sdf_inv_w();
    f32 ih = res_sdf_inv_h();

    if ( u0 ) *u0 = (f32)ox * iw;
    if ( v0 ) *v0 = (f32)oy * ih;
    if ( u1 ) *u1 = (f32)( ox + e->w ) * iw;
    if ( v1 ) *v1 = (f32)( oy + e->h ) * ih;
    return true;
}

/* The tex_idx a shape quad must carry: the SDF atlas's bindless slot with the sampling model
   already in the mode field.  0 means the atlas is not up yet and the draw skips, the same
   contract font_slot_tex and icon_tex answer with. */
u32
shape_tex( gui_shape_id_t id )
{
    const shape_entry_t* e = shape_entry( id );
    if ( !e )
        return 0u;

    u32 idx = res_sdf_idx();
    return idx ? ( idx | GUI_TEX_MODE( GUI_TEX_SDF ) ) : 0u;
}

/*==============================================================================================
    shape_reach -- how far, in PIXELS, an effect on this shape can travel before the field runs out.

    The bake stored `spread` TEXELS of field either side of the outline; drawing the ink into `r`
    scales that by the same factor the ink was scaled by.  So a 64-texel shape baked at spread 8 and
    drawn at 128 px reaches 16 px, and a border, glow or swell asking for more saturates and stops.

    Callers size their effects against this instead of discovering the ceiling visually.
==============================================================================================*/

f32
shape_reach( gui_shape_id_t id, gui_rect_t r )
{
    const shape_entry_t* e = shape_entry( id );
    if ( !e || e->ink_w == 0 || e->ink_h == 0 )
        return 0.0f;

    /* The uniform aspect-fit the draw side applies, restated: effects are isotropic, so the reach
       follows the one scale both axes actually get. */
    f32 sx = r.w / (f32)e->ink_w;
    f32 sy = r.h / (f32)e->ink_h;
    f32 s  = ( sx < sy ) ? sx : sy;
    return e->spread * s;
}

// clang-format on
/*============================================================================================*/
