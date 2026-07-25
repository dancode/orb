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

/* One registered icon: its name, source dimensions, and its shared-atlas tenant handle.  UVs are
   NOT cached -- they are derived live from the tenant's origin (icon_get), since a shared-atlas
   repack can move the tenant. */
typedef struct
{
    char name[ 32 ];        // lookup key (NUL-terminated, truncated to 31 chars)
    u16  w, h;              // source pixel dimensions
    u32  tenant;            // handle into the shared resource atlas (0 = unused)

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
    icon_register -- pack one raw R8 bitmap into the shared atlas; returns its id (0 on failure).
==============================================================================================*/

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
        printf( "[gui] icon atlas full -- '%s' (%ux%u) rejected\n", name ? name : "?", w, h );
        return GUI_ICON_NONE;
    }

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

    return (gui_icon_id_t)( ++s_icons.count );   /* id = (index + 1); 0 stays reserved for none */
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

    /* Derive UVs live from the tenant's current origin in the shared atlas (a repack can move it),
       scaled by the shared atlas dimensions -- the same rebase font glyphs use. */
    u32 ox, oy;
    res_atlas_origin( e->tenant, &ox, &oy );
    f32 iw = res_atlas_inv_w();
    f32 ih = res_atlas_inv_h();
    if ( u0 ) *u0 = (f32)ox * iw;
    if ( v0 ) *v0 = (f32)oy * ih;
    if ( u1 ) *u1 = (f32)( ox + e->w ) * iw;
    if ( v1 ) *v1 = (f32)( oy + e->h ) * ih;
    if ( w )  *w  = e->w;
    if ( h )  *h  = e->h;
    return true;
}

/*==============================================================================================
    BACKEND-INTERNAL -- consumed by pipeline/gui_emit_draw.c (draw_push_icon), the same shape as
    font_atlas_idx (draw/gui_glyph_internal.c): the bindless tex_idx a pipeline draw call binds.
    Icons live in the shared resource atlas, so this is that one shared slot -- which is exactly why
    an icon quad now batches with the text and fills around it.
==============================================================================================*/

static u32
icon_atlas_idx( void ) { return res_atlas_idx(); }

// clang-format on
/*============================================================================================*/
