/*==============================================================================================

    runtime_service/gui/draw/gui_sprite.c -- Sprite registry + nine-slice authoring.

    The colour sibling of gui_icon.c, and the difference between them is the whole point: an ICON
    is a coverage mask that the vertex colour paints, so it takes a theme's ink and belongs with
    the glyphs; a SPRITE carries its own colours and the vertex colour only tints it, so it is a
    picture and belongs in its own atlas.  One is a symbol, the other is art.

    What a sprite adds beyond "an RGBA quad" is the SLICE: four inset counts, in source pixels,
    that split it into nine pieces.  Fill any rect with a sliced sprite and the four corners stay
    at their authored size while the edges and centre stretch (or tile) to fit -- which is what
    turns a 32x32 PNG into a window frame that looks right at every size, and what lets a panel
    skin, a button face, or a health-bar trough be authored instead of coded.  The slice is a
    property of the ART, not of the draw, so it is set once at registration and every fill of that
    sprite inherits it.

    This file is bookkeeping only -- the name table, the slice, and the mapping from
    gui_sprite_id_t to a sprite-atlas tenant.  The expansion into quads lives at tessellation time
    (render/pipeline/gui_build_tess.c, tess_sprite), reached through the sprite source contract
    (sprite_get) the same way glyph metrics reach the tessellator.  Pixel SOURCING is split the way
    icons split it: sprite_register takes raw RGBA8 from whoever has it (procedural code, a host,
    the asset pipeline later) and sprite_load_res is the from-content caller on top.

    Included by gui_draw.c after gui_icon_load.c, whose stb_image implementation and resource
    read (image_read) it shares -- one decoder in this TU, not two.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Sizes
==============================================================================================*/

#define SPRITE_MAX     128u     // max distinct sprites

/*==============================================================================================
    State
==============================================================================================*/

/* One registered sprite: its name, source dimensions, nine-slice insets, and its sprite-atlas
   tenant handle.  UVs are NOT cached -- they are derived live from the tenant's origin
   (sprite_get), since an atlas repack can move the tenant. */
typedef struct
{
    u16       name_off;        // lookup key -- offset into the shared name pool (gui_names.h)
    u16       w, h;           // source pixel dimensions
    u16       tenant;         // handle into the sprite atlas (0 = unused)
    gui_pad_t slice;          // nine-slice insets in SOURCE pixels ({0,0,0,0} = not sliced)

} sprite_entry_t;

typedef struct
{
    sprite_entry_t entries[ SPRITE_MAX ];   // id - 1 indexes here
    u32            count;

} sprite_set_t;

static sprite_set_t s_sprites;

/*==============================================================================================
    Lifecycle -- the sprite layer holds no GPU resource of its own and needs no init: the atlas
    creates itself on the first registration (res_sprite_add), so a build that never registers a
    sprite never pays for one.  Only the teardown has anything to do.
==============================================================================================*/

void
sprite_registry_shutdown( void )
{
    /* The shared name pool (gui_names.h) is reset once, at true gui_shutdown -- not here, since
       sprites are only one of several registries interning into it. */
    memset( &s_sprites, 0, sizeof( s_sprites ) );
}

/*==============================================================================================
    sprite_register -- pack one raw RGBA8 image into the sprite atlas; returns its id (0 on
    failure).  `rgba` is row-major, w*h*4 bytes, straight (non-premultiplied) alpha -- which is
    what stb_image decodes to and what the gui's blend mode expects.
==============================================================================================*/

gui_sprite_id_t
sprite_register( const char* name, u32 w, u32 h, const u8* rgba )
{
    if ( !rgba || w == 0 || h == 0 )
        return GUI_SPRITE_NONE;
    if ( s_sprites.count >= SPRITE_MAX )
    {
        gui_log( GUI_LOG_WARN, "sprite registry full (%u) -- '%s' rejected",
                 SPRITE_MAX, name ? name : "?" );
        return GUI_SPRITE_NONE;
    }

    /* Adding the pixels is what brings the sprite atlas into existence on the first call; after
       that it is an incremental pack, with repack-on-full handled inside res_sprite_add. */
    u32 tenant = res_sprite_add( rgba, w, h );
    if ( tenant == 0 )
    {
        gui_log( GUI_LOG_WARN, "sprite atlas full -- '%s' (%ux%u) rejected",
                 name ? name : "?", w, h );
        return GUI_SPRITE_NONE;
    }

    sprite_entry_t* e = &s_sprites.entries[ s_sprites.count ];
    memset( e, 0, sizeof( *e ) );
    e->name_off = gui_names_intern( name );
    e->w      = (u16)w;
    e->h      = (u16)h;
    e->tenant = (u16)tenant;

    return (gui_sprite_id_t)( ++s_sprites.count );   /* id = (index + 1); 0 stays reserved for none */
}

/*==============================================================================================
    Lookup / query
==============================================================================================*/

/* Resolve an id (index + 1) to its entry, or NULL if out of range. */
static sprite_entry_t*
sprite_entry( gui_sprite_id_t id )
{
    if ( id == GUI_SPRITE_NONE || id > s_sprites.count )
        return NULL;
    return &s_sprites.entries[ id - 1 ];
}

gui_sprite_id_t
sprite_find( const char* name )
{
    if ( !name )
        return GUI_SPRITE_NONE;
    for ( u32 i = 0; i < s_sprites.count; ++i )
        if ( strcmp( gui_names_cstr( s_sprites.entries[ i ].name_off ), name ) == 0 )
            return (gui_sprite_id_t)( i + 1 );
    return GUI_SPRITE_NONE;
}

/* Declare the sprite's nine-slice insets, in SOURCE pixels.  Clamped so opposite insets can never
   exceed the art between them -- a slice wider than the sprite would give the expansion a negative
   middle track, and the honest answer to that is a degenerate (zero-width) middle, not a rejected
   call the author has to discover. */
bool
sprite_set_slice( gui_sprite_id_t id, gui_pad_t slice )
{
    sprite_entry_t* e = sprite_entry( id );
    if ( !e )
        return false;

    if ( slice.l < 0.0f ) slice.l = 0.0f;
    if ( slice.r < 0.0f ) slice.r = 0.0f;
    if ( slice.t < 0.0f ) slice.t = 0.0f;
    if ( slice.b < 0.0f ) slice.b = 0.0f;

    if ( slice.l + slice.r > (f32)e->w )
    {
        f32 k = (f32)e->w / ( slice.l + slice.r );
        slice.l *= k; slice.r *= k;
    }
    if ( slice.t + slice.b > (f32)e->h )
    {
        f32 k = (f32)e->h / ( slice.t + slice.b );
        slice.t *= k; slice.b *= k;
    }

    e->slice = slice;
    return true;
}

gui_pad_t
sprite_slice( gui_sprite_id_t id )
{
    const sprite_entry_t* e = sprite_entry( id );
    return e ? e->slice : ( gui_pad_t ){ 0 };
}

gui_vec2_t
sprite_size( gui_sprite_id_t id )
{
    const sprite_entry_t* e = sprite_entry( id );
    return e ? ( gui_vec2_t ){ (f32)e->w, (f32)e->h } : ( gui_vec2_t ){ 0.0f, 0.0f };
}

/*==============================================================================================
    THE SPRITE SOURCE CONTRACT -- what the tessellator resolves at flush time (declared in
    render/gui_render.h, the render server's surface).  The same shape as font_glyph and icon_get:
    the server renders from an atlas it was handed and asks this table where a thing landed in it.
==============================================================================================*/

bool
sprite_get( gui_sprite_id_t id, f32* u0, f32* v0, f32* u1, f32* v1,
            u32* w, u32* h, gui_pad_t* slice )
{
    const sprite_entry_t* e = sprite_entry( id );
    if ( !e )
        return false;

    /* Derive UVs live from the tenant's current origin (a repack can move it), scaled by the
       sprite atlas dimensions -- the same rebase glyphs and icons use against theirs. */
    u32 ox, oy;
    res_sprite_origin( e->tenant, &ox, &oy );
    f32 iw = res_sprite_inv_w();
    f32 ih = res_sprite_inv_h();
    if ( u0 )    *u0    = (f32)ox * iw;
    if ( v0 )    *v0    = (f32)oy * ih;
    if ( u1 )    *u1    = (f32)( ox + e->w ) * iw;
    if ( v1 )    *v1    = (f32)( oy + e->h ) * ih;
    if ( w )     *w     = e->w;
    if ( h )     *h     = e->h;
    if ( slice ) *slice = e->slice;
    return true;
}

/*==============================================================================================
    sprite_load_res -- decode an image resource to RGBA8 and register it as a sprite.

    The icon loader's twin, minus the channel decision: an icon has to pick between alpha and
    luminance because it is collapsing colour into one coverage byte, and a sprite keeps every
    channel it was authored with, so there is nothing to decide.  Shares this TU's stb_image
    implementation and image_read (gui_icon_load.c, included just above): `res` is a resource
    name, RID( "ui/skin/frame" ), read through the fs mounts.

    Returns the new sprite id, or GUI_SPRITE_NONE if no mount serves the name, the bytes are
    undecodable, or the atlas is full.  A missing resource is a quiet failure (the caller decides
    whether that matters); a present-but-broken image logs, since it signals a real asset problem.
==============================================================================================*/

gui_sprite_id_t
sprite_load_res( const char* name, const char* res )
{
    if ( !name || !res )
        return GUI_SPRITE_NONE;

    fs_blob_t file = image_read( res );
    if ( !file.ok )
        return GUI_SPRITE_NONE;   // no mount serves it -- quiet

    int      w = 0, h = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_memory( (const stbi_uc*)file.data, (int)file.size, &w, &h, &comp, STBI_rgb_alpha );
    fs()->free( &file );
    if ( !rgba )
    {
        gui_log( GUI_LOG_WARN, "sprite '%s' decode failed ('%s'): %s",
                 name, res, stbi_failure_reason() );
        return GUI_SPRITE_NONE;
    }

    gui_sprite_id_t id = sprite_register( name, (u32)w, (u32)h, (const u8*)rgba );
    stbi_image_free( rgba );
    return id;
}

// clang-format on
/*============================================================================================*/
