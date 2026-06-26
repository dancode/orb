/*==============================================================================================

    runtime_service/gui/gui_icon.c -- Runtime icon atlas.

    A second R8 coverage texture, built at runtime from raw monochrome bitmaps.  Where the font
    atlases are fixed and baked offline, this one is dynamic: callers register icon pixels at any
    time (icon_register), the atlas packs them into free space with stb_rect_pack, and hands back
    an gui_icon_id_t.  Drawing reuses the existing textured-quad primitive (draw_push_rect_filled)
    with the atlas's own bindless tex_idx, so icons batch in the same flush as text and tint by the
    vertex color -- monochrome coverage in, any color out.

    Pixel SOURCING is intentionally out of scope: callers supply raw R8 coverage bytes (row-major,
    w*h, 0..255).  Whoever has the bytes -- procedural code today, the asset/image pipeline later --
    feeds them in.

    GPU upload is deferred: register_icon only touches the resident CPU buffer and sets `dirty`;
    icon_atlas_flush_upload (called from frame_begin) re-uploads once per frame when needed.  This
    mirrors the deferred cursor flush and keeps registration safe to call mid-frame.

    Included by gui_backend.c after gui_font.c.

==============================================================================================*/
// clang-format off

/* stb_rect_pack is vendored under font_tool; this TU owns the runtime implementation.  font_tool
   compiles its own copy into a separate executable, so there is no duplicate-symbol conflict. */
#define STB_RECT_PACK_IMPLEMENTATION
#include "tools/font_tool/stb_rect_pack.h"

/*----------------------------------------------------------------------------------------------
    Sizes
----------------------------------------------------------------------------------------------*/

#define ICON_ATLAS_W   512u     // atlas width  in pixels (also stbrp node count)
#define ICON_ATLAS_H   512u     // atlas height in pixels
#define ICON_MAX       256u     // max distinct icons
#define ICON_PAD       1u       // 1px gap between packed rects, stops bilinear bleed

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* One packed icon: its pixel rect and pre-computed atlas UVs. */
typedef struct
{
    char name[ 32 ];        // lookup key (NUL-terminated, truncated to 31 chars)
    u16  x, y, w, h;        // pixel rect in the atlas
    f32  u0, v0, u1, v1;    // cached UVs (rect / atlas dims)

} icon_entry_t;

typedef struct
{
    rhi_texture_t atlas;                    // GPU texture handle
    u32           atlas_idx;                // bindless index (0 = not registered)
    u8*           pixels;                   // resident CPU staging (ICON_ATLAS_W * ICON_ATLAS_H, R8)

    icon_entry_t  entries[ ICON_MAX ];      // id - 1 indexes here
    u32           count;

    stbrp_context pack;                     // persistent packer; rects appended incrementally
    stbrp_node    nodes[ ICON_ATLAS_W ];

    bool          dirty;                    // CPU atlas changed -> needs a GPU re-upload
    bool          ready;                    // texture created and registered

} icon_atlas_t;

static icon_atlas_t s_icons;

/*----------------------------------------------------------------------------------------------
    icon_atlas_init / icon_atlas_shutdown
----------------------------------------------------------------------------------------------*/

bool
icon_atlas_init( void )
{
    memset( &s_icons, 0, sizeof( s_icons ) );

    /* Resident CPU copy: cleared to 0 (transparent) so unpacked space samples as empty. */
    s_icons.pixels = (u8*)calloc( ICON_ATLAS_W * ICON_ATLAS_H, 1 );
    if ( !s_icons.pixels )
        return false;

    stbrp_init_target( &s_icons.pack, ICON_ATLAS_W, ICON_ATLAS_H, s_icons.nodes, ICON_ATLAS_W );

    s_icons.atlas = rhi()->texture_create( &( rhi_texture_desc_t ){
        .width        = ICON_ATLAS_W,
        .height       = ICON_ATLAS_H,
        .depth        = 1,
        .mip_levels   = 1,
        .array_layers = 1,
        .format       = RHI_FORMAT_R8_UNORM,
        .usage        = RHI_TEXTURE_USAGE_SAMPLED | RHI_TEXTURE_USAGE_TRANSFER_DST,
        .memory       = RHI_MEMORY_GPU_ONLY,
        .debug_name   = "gui_icon_atlas",
    } );
    if ( !rhi_handle_valid( s_icons.atlas ) ) { free( s_icons.pixels ); s_icons.pixels = NULL; return false; }

    /* Upload the cleared atlas once so the texture has defined contents before any icon lands. */
    if ( !rhi()->upload_texture( s_icons.atlas, s_icons.pixels, ICON_ATLAS_W * ICON_ATLAS_H, 0, 0 ) )
    {
        rhi()->texture_destroy( s_icons.atlas );
        s_icons.atlas = ( rhi_texture_t ){ 0 };
        free( s_icons.pixels ); s_icons.pixels = NULL;
        return false;
    }

    s_icons.atlas_idx = rhi()->register_texture( s_icons.atlas );
    if ( s_icons.atlas_idx == 0 )
    {
        rhi()->texture_destroy( s_icons.atlas );
        s_icons.atlas = ( rhi_texture_t ){ 0 };
        free( s_icons.pixels ); s_icons.pixels = NULL;
        return false;
    }

    s_icons.ready = true;
    return true;
}

void
icon_atlas_shutdown( void )
{
    if ( s_icons.atlas_idx != 0 )
    {
        rhi()->unregister_texture( s_icons.atlas_idx );
        s_icons.atlas_idx = 0;
    }
    if ( rhi_handle_valid( s_icons.atlas ) )
    {
        rhi()->texture_destroy( s_icons.atlas );
        s_icons.atlas = ( rhi_texture_t ){ 0 };
    }
    free( s_icons.pixels );
    s_icons.pixels = NULL;
    s_icons.ready  = false;
    s_icons.count  = 0;
}

/*----------------------------------------------------------------------------------------------
    icon_atlas_flush_upload -- deferred GPU upload, called once per frame from frame_begin.
----------------------------------------------------------------------------------------------*/

void
icon_atlas_flush_upload( void )
{
    if ( !s_icons.ready || !s_icons.dirty )
        return;

    rhi()->upload_texture( s_icons.atlas, s_icons.pixels, ICON_ATLAS_W * ICON_ATLAS_H, 0, 0 );
    s_icons.dirty = false;
}

/*----------------------------------------------------------------------------------------------
    icon_register -- pack one raw R8 bitmap into the atlas; returns its id (0 on failure).
----------------------------------------------------------------------------------------------*/

gui_icon_id_t
icon_register( const char* name, u32 w, u32 h, const u8* coverage )
{
    if ( !s_icons.ready || !coverage || w == 0 || h == 0 )
        return GUI_ICON_NONE;
    if ( s_icons.count >= ICON_MAX || w > ICON_ATLAS_W || h > ICON_ATLAS_H )
        return GUI_ICON_NONE;

    /* Pack one rect (padded) into the running packer.  stb_rect_pack supports incremental calls;
       the only cost is slightly looser packing for later batches -- fine for an editor icon set. */
    stbrp_rect rc = { .id = (int)s_icons.count,
                      .w  = (stbrp_coord)( w + ICON_PAD ),
                      .h  = (stbrp_coord)( h + ICON_PAD ) };
    if ( !stbrp_pack_rects( &s_icons.pack, &rc, 1 ) || !rc.was_packed )
    {
        printf( "[gui] icon atlas full -- '%s' (%ux%u) rejected\n", name ? name : "?", w, h );
        return GUI_ICON_NONE;
    }

    /* Blit the coverage rows into the resident CPU atlas. */
    for ( u32 r = 0; r < h; ++r )
        memcpy( &s_icons.pixels[ ( (u32)rc.y + r ) * ICON_ATLAS_W + (u32)rc.x ],
                &coverage[ r * w ], w );

    icon_entry_t* e = &s_icons.entries[ s_icons.count ];
    memset( e, 0, sizeof( *e ) );
    if ( name )
    {
        u32 i = 0;
        for ( ; i < sizeof( e->name ) - 1 && name[ i ]; ++i )
            e->name[ i ] = name[ i ];
        e->name[ i ] = '\0';
    }
    e->x = (u16)rc.x;
    e->y = (u16)rc.y;
    e->w = (u16)w;
    e->h = (u16)h;

    f32 iw = 1.0f / (f32)ICON_ATLAS_W;
    f32 ih = 1.0f / (f32)ICON_ATLAS_H;
    e->u0 = (f32)rc.x * iw;
    e->v0 = (f32)rc.y * ih;
    e->u1 = e->u0 + (f32)w * iw;
    e->v1 = e->v0 + (f32)h * ih;

    s_icons.dirty = true;
    return (gui_icon_id_t)( ++s_icons.count );   /* id = (index + 1); 0 stays reserved for none */
}

/*----------------------------------------------------------------------------------------------
    Lookup / query
----------------------------------------------------------------------------------------------*/

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
        if ( strcmp( s_icons.entries[ i ].name, name ) == 0 )
            return (gui_icon_id_t)( i + 1 );
    return GUI_ICON_NONE;
}

bool
icon_get( gui_icon_id_t id, f32* u0, f32* v0, f32* u1, f32* v1, u32* w, u32* h )
{
    const icon_entry_t* e = icon_entry( id );
    if ( !e )
        return false;
    if ( u0 ) *u0 = e->u0;
    if ( v0 ) *v0 = e->v0;
    if ( u1 ) *u1 = e->u1;
    if ( v1 ) *v1 = e->v1;
    if ( w )  *w  = e->w;
    if ( h )  *h  = e->h;
    return true;
}

/*----------------------------------------------------------------------------------------------
    draw_push_icon -- emit one icon quad through the existing textured-rect path.
----------------------------------------------------------------------------------------------*/

void
draw_push_icon( f32 x, f32 y, f32 w, f32 h, gui_icon_id_t id, u32 abgr )
{
    const icon_entry_t* e = icon_entry( id );
    if ( !e )
        return;
    draw_push_rect_filled( x, y, w, h, e->u0, e->v0, e->u1, e->v1, s_icons.atlas_idx, abgr );
}

// clang-format on
/*============================================================================================*/
