/*==============================================================================================

    runtime_service/gui/render/resource/gui_res_atlas.c -- The shared GUI resource atlas.

    See gui_res_atlas.h for the rationale.  This file owns the one R8 texture, the resident CPU
    buffer, the master stb_rect_pack area, the fixed assist band, and the tenant table (one retained
    pixel copy per packed font/icon so a repack can re-blit without going back to disk).

    Included by gui_render.c after resource/gui_atlas.c (whose create/upload/destroy it wraps) and
    before every pipeline stage, which resolve their UVs out of what is packed here.  The tenants
    themselves -- fonts and icons -- are the DRAW unit's, one level up: they pack in from outside
    through the res_atlas_* entry points this file exports via gui_res_atlas.h.

==============================================================================================*/
// clang-format off

/* stb_rect_pack is vendored under dev_font (the single canonical copy); this TU owns the runtime
   implementation (the icon atlas used to, before packing moved here).  dev_font compiles its own
   implementation with STBRP_STATIC into a separate static lib, so there is no symbol conflict. */
#define STB_RECT_PACK_IMPLEMENTATION
#include "developer/dev_font/stb_rect_pack.h"

/* Assist band: one white row + the dash rows, full-width, pinned to the bottom of the texture so
   the packer (which works the top region) never touches it and its UVs never move on a repack. */
#define RES_ASSIST_ROWS   ( 1u + GUI_DASH_PATTERN_COUNT )

/* Usable packing height: everything above the assist band. */
#define RES_PACK_H        ( GUI_RES_ATLAS_H - RES_ASSIST_ROWS )

/* On-fraction (dash / period) of each baked dash row; a dashed line picks the nearest at tess time.
   Moved here from the font registry -- the dash rows are an atlas-level asset now, not per-font. */
static const f32 s_dash_duty[ GUI_DASH_PATTERN_COUNT ] = { 0.12f, 0.35f, 0.5f, 0.7f };

/* One packed tenant: its retained R8 source and its current pixel rect in the atlas. */
typedef struct
{
    bool used;
    u8*  src;         // owned copy of the coverage bytes (w*h), kept for re-blit on repack
    u32  w, h;        // source pixel dimensions
    u32  ox, oy;      // current pixel origin (top-left) in the atlas

} res_tenant_t;

static struct
{
    gui_atlas_t    atlas;                            // owned GPU texture + bindless index
    u8*            pixels;                           // resident CPU staging (W*H, R8)

    stbrp_context  pack;                             // master packer over the top RES_PACK_H region
    stbrp_node     nodes[ GUI_RES_ATLAS_W ];

    res_tenant_t   tenants[ GUI_RES_ATLAS_MAX_TENANTS ];

    f32            white_u, white_v;                 // opaque assist texel (fixed; never moves)
    f32            dash_v[ GUI_DASH_PATTERN_COUNT ]; // center V of each dash row (fixed)

    u32            generation;                       // bumps on every UV-affecting structural change
    bool           dirty;                            // resident buffer changed -> needs GPU re-upload
    bool           ready;                            // texture created and registered

} s_res;

/*==============================================================================================
    Assist band -- paint the white texel row + dash rows into the bottom RES_ASSIST_ROWS rows and
    resolve their (fixed) UVs.  Called at init and after every repack (which clears the buffer).
==============================================================================================*/

static void
res_paint_assist( void )
{
    u32 white_row = GUI_RES_ATLAS_H - RES_ASSIST_ROWS;   // first band row
    u32 dash_row0 = white_row + 1u;                      // dash rows follow the white row

    /* White texel strip: fill the white row opaque so any texel in it samples r=1.0. */
    memset( &s_res.pixels[ white_row * GUI_RES_ATLAS_W ], 0xFF, GUI_RES_ATLAS_W );

    /* Each dash row encodes ONE period: the leftmost duty*W texels opaque, the rest zero.  A dashed
       line samples the row with REPEAT-U so the full-width period tiles along the line. */
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
    {
        u8* row = &s_res.pixels[ ( dash_row0 + p ) * GUI_RES_ATLAS_W ];
        u32 on  = (u32)( s_dash_duty[ p ] * (f32)GUI_RES_ATLAS_W + 0.5f );
        if ( on < 1 )                on = 1;
        if ( on > GUI_RES_ATLAS_W )  on = GUI_RES_ATLAS_W;
        memset( row, 0x00, GUI_RES_ATLAS_W );   // gap
        memset( row, 0xFF, on );                // on-run
    }

    s_res.white_u = 0.5f / (f32)GUI_RES_ATLAS_W;
    s_res.white_v = ( (f32)white_row + 0.5f ) / (f32)GUI_RES_ATLAS_H;
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
        s_res.dash_v[ p ] = ( (f32)( dash_row0 + p ) + 0.5f ) / (f32)GUI_RES_ATLAS_H;
}

/* Blit a tenant's retained source into the resident buffer at its current origin. */
static void
res_blit_tenant( const res_tenant_t* t )
{
    for ( u32 r = 0; r < t->h; ++r )
        memcpy( &s_res.pixels[ ( t->oy + r ) * GUI_RES_ATLAS_W + t->ox ], &t->src[ r * t->w ], t->w );
}

/* Pack one (w+PAD, h+PAD) rect into the master packer; on success reports its origin. */
static bool
res_pack_one( u32 w, u32 h, u32* ox, u32* oy )
{
    stbrp_rect rc = { .w = (stbrp_coord)( w + GUI_RES_ATLAS_PAD ),
                      .h = (stbrp_coord)( h + GUI_RES_ATLAS_PAD ) };
    if ( !stbrp_pack_rects( &s_res.pack, &rc, 1 ) || !rc.was_packed )
        return false;
    *ox = (u32)rc.x;
    *oy = (u32)rc.y;
    return true;
}

/*==============================================================================================
    res_repack -- re-init the packer, re-place every live tenant (tallest first for a tighter
    skyline), clear + repaint the resident buffer, and re-blit each tenant at its new origin.
    Returns false only if some tenant no longer fits (atlas genuinely over capacity).
==============================================================================================*/

static bool
res_repack( void )
{
    stbrp_init_target( &s_res.pack, GUI_RES_ATLAS_W, RES_PACK_H, s_res.nodes, GUI_RES_ATLAS_W );

    /* Place tallest tenants first: stb_rect_pack's skyline packs tighter that way, and an
       incremental burst of registrations can otherwise leave later (larger) rects homeless. */
    u32 order[ GUI_RES_ATLAS_MAX_TENANTS ];
    u32 n = 0;
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
        if ( s_res.tenants[ i ].used )
            order[ n++ ] = i;
    for ( u32 a = 0; a + 1 < n; ++a )
        for ( u32 b = a + 1; b < n; ++b )
            if ( s_res.tenants[ order[ b ] ].h > s_res.tenants[ order[ a ] ].h )
            {
                u32 tmp = order[ a ]; order[ a ] = order[ b ]; order[ b ] = tmp;
            }

    /* Pack into scratch origins first and commit only if EVERY tenant fits.  A mid-loop failure
       (atlas genuinely over capacity) must not leave some tenants' live origins pointing at a
       layout the pixel buffer was never re-blitted to. */
    u32 nx[ GUI_RES_ATLAS_MAX_TENANTS ];
    u32 ny[ GUI_RES_ATLAS_MAX_TENANTS ];
    for ( u32 k = 0; k < n; ++k )
        if ( !res_pack_one( s_res.tenants[ order[ k ] ].w, s_res.tenants[ order[ k ] ].h,
                            &nx[ k ], &ny[ k ] ) )
            return false;

    memset( s_res.pixels, 0, GUI_RES_ATLAS_W * GUI_RES_ATLAS_H );
    res_paint_assist();
    for ( u32 k = 0; k < n; ++k )
    {
        res_tenant_t* t = &s_res.tenants[ order[ k ] ];
        t->ox = nx[ k ];
        t->oy = ny[ k ];
        res_blit_tenant( t );
    }

    s_res.dirty = true;
    return true;
}

/*==============================================================================================
    Lifecycle
==============================================================================================*/

bool
res_atlas_init( void )
{
    memset( &s_res, 0, sizeof( s_res ) );

    /* Resident CPU copy: cleared to 0 (transparent) so unpacked space samples as empty. */
    s_res.pixels = (u8*)calloc( GUI_RES_ATLAS_W * GUI_RES_ATLAS_H, 1 );
    if ( !s_res.pixels )
        return false;

    /* Packer works the region ABOVE the assist band; the band itself is fixed and never packed. */
    stbrp_init_target( &s_res.pack, GUI_RES_ATLAS_W, RES_PACK_H, s_res.nodes, GUI_RES_ATLAS_W );
    res_paint_assist();

    if ( !gui_atlas_create( &s_res.atlas, GUI_RES_ATLAS_W, GUI_RES_ATLAS_H, s_res.pixels, "gui_res_atlas" ) )
    {
        free( s_res.pixels ); s_res.pixels = NULL;
        return false;
    }

    s_res.generation = 1;
    s_res.ready      = true;
    return true;
}

void
res_atlas_shutdown( void )
{
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
    {
        free( s_res.tenants[ i ].src );
        s_res.tenants[ i ].src = NULL;
    }
    gui_atlas_destroy( &s_res.atlas );
    free( s_res.pixels );
    s_res.pixels = NULL;
    s_res.ready  = false;
}

/* Returns true when pixels actually reached the GPU this call.  The frame loop turns that into a
   forced rebuild: new resident art (an icon registered between frames) only becomes visible once a
   widget emits it, and on a clean frame no widget runs at all. */
bool
res_atlas_flush_upload( void )
{
    if ( !s_res.ready || !s_res.dirty )
        return false;
    gui_atlas_upload( &s_res.atlas, s_res.pixels );
    s_res.dirty = false;
    return true;
}

/*==============================================================================================
    Tenant registration
==============================================================================================*/

u32
res_atlas_add( const u8* src, u32 w, u32 h )
{
    if ( !s_res.ready || !src || w == 0 || h == 0 )
        return 0;
    if ( w + GUI_RES_ATLAS_PAD > GUI_RES_ATLAS_W || h + GUI_RES_ATLAS_PAD > RES_PACK_H )
        return 0;

    /* Claim a tenant slot and take our own copy of the coverage (needed to re-blit on a repack). */
    u32 idx = GUI_RES_ATLAS_MAX_TENANTS;
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
        if ( !s_res.tenants[ i ].used ) { idx = i; break; }
    if ( idx == GUI_RES_ATLAS_MAX_TENANTS )
        return 0;

    u8* copy = (u8*)malloc( (size_t)w * h );
    if ( !copy )
        return 0;
    memcpy( copy, src, (size_t)w * h );

    res_tenant_t* t = &s_res.tenants[ idx ];
    t->used = true;
    t->src  = copy;
    t->w    = w;
    t->h    = h;

    /* Fast path: one incremental pack call.  On a full packer, fold this tenant into a repack. */
    if ( res_pack_one( w, h, &t->ox, &t->oy ) )
    {
        res_blit_tenant( t );
        s_res.dirty = true;
    }
    else if ( !res_repack() )   /* repack places every used tenant, including this one */
    {
        free( t->src );
        *t = ( res_tenant_t ){ 0 };
        return 0;
    }

    ++s_res.generation;
    return idx + 1;   /* 1-based handle; 0 reserved for "none" */
}

bool
res_atlas_update( u32 handle, const u8* src, u32 w, u32 h )
{
    if ( !s_res.ready || handle == 0 || handle > GUI_RES_ATLAS_MAX_TENANTS || !src || w == 0 || h == 0 )
        return false;

    res_tenant_t* t = &s_res.tenants[ handle - 1 ];
    if ( !t->used )
        return false;

    if ( w == t->w && h == t->h )
    {
        /* Same footprint: replace pixels and re-blit in place -- no repack, origin unchanged.  Bump
           generation anyway: glyph shapes / metrics changed, so cached geometry must re-tessellate. */
        memcpy( t->src, src, (size_t)w * h );
        res_blit_tenant( t );
        s_res.dirty = true;
        ++s_res.generation;
        return true;
    }

    if ( w + GUI_RES_ATLAS_PAD > GUI_RES_ATLAS_W || h + GUI_RES_ATLAS_PAD > RES_PACK_H )
        return false;

    /* Different footprint: swap the source and repack (this tenant's old rect is freed, origins may
       move).  On failure the tenant keeps its old (still-blitted) pixels -- restore its source. */
    u8* copy = (u8*)malloc( (size_t)w * h );
    if ( !copy )
        return false;
    memcpy( copy, src, (size_t)w * h );

    u8* old_src = t->src;
    u32 old_w = t->w, old_h = t->h;
    t->src = copy;
    t->w   = w;
    t->h   = h;

    if ( !res_repack() )
    {
        free( t->src );
        t->src = old_src;
        t->w   = old_w;
        t->h   = old_h;
        res_repack();   /* restore the previous layout so the atlas stays consistent */
        return false;
    }

    free( old_src );
    ++s_res.generation;
    return true;
}

void
res_atlas_origin( u32 handle, u32* ox, u32* oy )
{
    if ( handle == 0 || handle > GUI_RES_ATLAS_MAX_TENANTS || !s_res.tenants[ handle - 1 ].used )
    {
        if ( ox ) *ox = 0;
        if ( oy ) *oy = 0;
        return;
    }
    const res_tenant_t* t = &s_res.tenants[ handle - 1 ];
    if ( ox ) *ox = t->ox;
    if ( oy ) *oy = t->oy;
}

/*==============================================================================================
    Sampling accessors
==============================================================================================*/

u32  res_atlas_idx        ( void ) { return s_res.atlas.atlas_idx; }
f32  res_atlas_inv_w      ( void ) { return 1.0f / (f32)GUI_RES_ATLAS_W; }
f32  res_atlas_inv_h      ( void ) { return 1.0f / (f32)GUI_RES_ATLAS_H; }
u32  res_atlas_generation ( void ) { return s_res.generation; }
u32  res_atlas_bytes      ( void ) { return GUI_RES_ATLAS_W * GUI_RES_ATLAS_H; }

void
res_atlas_white_uv( f32* u, f32* v )
{
    *u = s_res.white_u;
    *v = s_res.white_v;
}

/* Center V of the dash row whose baked on-fraction is closest to `duty`. */
f32
res_atlas_dash_v( f32 duty )
{
    u32 best  = 0;
    f32 bestd = 1e30f;
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
    {
        f32 d = s_dash_duty[ p ] - duty;
        if ( d < 0.0f ) d = -d;
        if ( d < bestd ) { bestd = d; best = p; }
    }
    return s_res.dash_v[ best ];
}

// clang-format on
/*============================================================================================*/
