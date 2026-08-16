/*==============================================================================================

    runtime_service/gui/render/resource/gui_res_atlas.c -- The GUI resource atlases.

    See gui_res_atlas.h for the rationale.  This file owns the two textures, their resident CPU
    buffers, the master stb_rect_pack area of each, the coverage atlas's fixed assist band, and the
    tenant tables (one retained pixel copy per packed tenant so a repack can re-blit without going
    back to disk).

    ONE mechanism, three instances.  Everything below the "Instances" banner takes a res_atlas_t*
    and is blind to which atlas it is working on; what differs is `bpp` (1 = coverage or distance,
    4 = colour), `assist` (whether the bottom band is reserved), `extrude` (whether a tenant gets a
    private edge-replicated ring, which anything sampled LINEAR needs) and the DIMENSIONS -- a
    distance-field page is several times the area of its coverage twin, so the SDF atlas has to be
    larger or a font's page cannot be a tenant of it at all.  The public res_atlas_* / res_sprite_*
    / res_sdf_* sets at the foot are the three bindings of that one mechanism -- which is why each
    new atlas cost a struct and a set of wrappers rather than another packer.

    Included by gui_render.c after resource/gui_atlas.c (whose create/upload/destroy it wraps) and
    before every pipeline stage, which resolve their UVs out of what is packed here.  The tenants
    themselves -- fonts, icons and sprites -- are the DRAW unit's, one level up: they pack in from
    outside through the entry points this file exports via gui_res_atlas.h.

==============================================================================================*/
// clang-format off

/* stb_rect_pack is vendored under dev_font (the single canonical copy); this TU owns the runtime
   implementation (the icon atlas used to, before packing moved here).  dev_font compiles its own
   implementation with STBRP_STATIC into a separate static lib, so there is no symbol conflict. */
#define STB_RECT_PACK_IMPLEMENTATION
#include "developer/dev_font/stb_rect_pack.h"

/* Assist band: one white row + the dash rows, full-width, pinned to the bottom of the texture so
   the packer (which works the top region) never touches it and its UVs never move on a repack.
   The coverage atlas alone carries it -- the assists ARE coverage. */
#define RES_ASSIST_ROWS   ( 1u + GUI_DASH_PATTERN_COUNT )

/* On-fraction (dash / period) of each baked dash row; a dashed line picks the nearest at tess time.
   Moved here from the font registry -- the dash rows are an atlas-level asset now, not per-font. */
static const f32 s_dash_duty[ GUI_DASH_PATTERN_COUNT ] = { 0.12f, 0.35f, 0.5f, 0.7f };

/* One packed tenant: its retained source pixels and its current pixel rect in the atlas. */
typedef struct
{
    bool used;
    u8*  src;         // owned copy of the source bytes (w*h*bpp), kept for re-blit on repack
    u32  w, h;        // source pixel dimensions
    u32  ox, oy;      // current pixel origin (top-left) in the atlas

} res_tenant_t;

/* One atlas instance.  `bpp` and `assist` are the entire difference between the two. */
typedef struct
{
    gui_atlas_t    atlas;                            // owned GPU texture + bindless index
    u8*            pixels;                           // resident CPU staging (w*h*bpp)

    /* Per instance, not global: a distance-field page is several times the area of its coverage
       twin, so the SDF atlas must be larger or a font's page cannot be a tenant of it at all.
       w/h are the CURRENT dimensions; the atlas grows toward max_w/max_h under pressure. */
    u32            w, h;                             // current texture dimensions in pixels
    u32            max_w, max_h;                     // growth ceiling (GUI_*_DIM_CAP)

    u32            bpp;                              // 1 = R8 coverage, 4 = RGBA8 colour
    bool           assist;                           // reserve + paint the bottom assist band
    const char*    debug_name;                       // GPU-side label, also the "atlas full" tag

    /* Gutter policy.  `pad` is what the packer adds to every rect; `inset` is how far inside its
       packed cell the tenant actually sits, so pad - inset is what remains on the far edges.
       Coverage packs (w+1, h+1) at the cell origin: one gutter row/column shared between
       neighbours, which is all NEAREST sampling ever needs.  The sprite atlas packs (w+2, h+2) at
       cell origin + 1, giving each tenant a private 1px ring on ALL FOUR sides, and `extrude`
       fills that ring with a copy of the tenant's own edge -- because a sprite is sampled LINEAR,
       and a bilinear tap at the outer edge of a sprite would otherwise pull in the transparent
       gutter and fringe every piece of art in the atlas. */
    u32            pad;                              // packer margin added to each rect
    u32            inset;                            // tenant origin offset inside its packed cell
    bool           extrude;                          // replicate the tenant's edge into the ring

    stbrp_context  pack;                             // master packer over the packable region
    stbrp_node     nodes[ GUI_RES_ATLAS_MAX_W ];     // sized for the widest instance

    res_tenant_t   tenants[ GUI_RES_ATLAS_MAX_TENANTS ];

    f32            white_u, white_v;                 // opaque assist texel (fixed; never moves)
    f32            dash_v[ GUI_DASH_PATTERN_COUNT ]; // center V of each dash row (fixed)

    u32            generation;                       // bumps on every UV-affecting structural change
    bool           dirty;                            // resident buffer changed -> needs GPU re-upload
    bool           ready;                            // texture created and registered

} res_atlas_t;

/*==============================================================================================
    Instances

    s_res is stood up by res_atlas_init at backend boot and lives for the process.  s_spr is
    created by the first res_sprite_add and not before: a build that registers no authored art
    pays neither the megabyte nor the bindless slot.
==============================================================================================*/

static res_atlas_t s_res;   // COVERAGE: glyphs, icons, the drawing assists
static res_atlas_t s_spr;   // SPRITE:   authored RGBA art (sprite quads, nine-slice frames)
static res_atlas_t s_sdf;   // SDF:      distance-field glyphs (scalable text)

/* Rows this atlas leaves to the packer -- everything above its (optional) assist band. */
static u32
res_pack_h( const res_atlas_t* a )
{
    return a->assist ? ( a->h - RES_ASSIST_ROWS ) : a->h;
}

/*==============================================================================================
    Assist band -- paint the white texel row + dash rows into the bottom RES_ASSIST_ROWS rows and
    resolve their (fixed) UVs.  Called at init and after every repack (which clears the buffer).
    A no-op on an atlas that carries no assists.
==============================================================================================*/

static void
res_paint_assist( res_atlas_t* a )
{
    if ( !a->assist )
        return;

    u32 white_row = a->h - RES_ASSIST_ROWS;              // first band row
    u32 dash_row0 = white_row + 1u;                      // dash rows follow the white row

    /* White texel strip: fill the white row opaque so any texel in it samples r=1.0. */
    memset( &a->pixels[ white_row * a->w ], 0xFF, a->w );

    /* Each dash row encodes ONE period: the leftmost duty*W texels opaque, the rest zero.  A dashed
       line samples the row with REPEAT-U so the full-width period tiles along the line. */
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
    {
        u8* row = &a->pixels[ ( dash_row0 + p ) * a->w ];
        u32 on  = (u32)( s_dash_duty[ p ] * (f32)a->w + 0.5f );
        if ( on < 1 )      on = 1;
        if ( on > a->w )   on = a->w;
        memset( row, 0x00, a->w );              // gap
        memset( row, 0xFF, on );                // on-run
    }

    a->white_u = 0.5f / (f32)a->w;
    a->white_v = ( (f32)white_row + 0.5f ) / (f32)a->h;
    for ( u32 p = 0; p < GUI_DASH_PATTERN_COUNT; ++p )
        a->dash_v[ p ] = ( (f32)( dash_row0 + p ) + 0.5f ) / (f32)a->h;
}

/* One pixel of the resident buffer, as bytes. */
static u8*
res_px( res_atlas_t* a, u32 x, u32 y )
{
    return &a->pixels[ ( (size_t)y * a->w + x ) * a->bpp ];
}

/* Replicate a tenant's outermost row / column one pixel outward on all four sides (corners
   included).  Only meaningful on an atlas whose tenants carry a private ring (inset >= 1); see the
   gutter-policy note on the instance record for why the sprite atlas needs it and the coverage
   atlas does not. */
static void
res_extrude_tenant( res_atlas_t* a, const res_tenant_t* t )
{
    u32 bpp = a->bpp, x0 = t->ox, y0 = t->oy, x1 = t->ox + t->w - 1, y1 = t->oy + t->h - 1;

    for ( u32 x = x0; x <= x1; ++x )                 /* top / bottom edges */
    {
        memcpy( res_px( a, x, y0 - 1 ), res_px( a, x, y0 ), bpp );
        memcpy( res_px( a, x, y1 + 1 ), res_px( a, x, y1 ), bpp );
    }
    for ( u32 y = y0; y <= y1; ++y )                 /* left / right edges */
    {
        memcpy( res_px( a, x0 - 1, y ), res_px( a, x0, y ), bpp );
        memcpy( res_px( a, x1 + 1, y ), res_px( a, x1, y ), bpp );
    }
    memcpy( res_px( a, x0 - 1, y0 - 1 ), res_px( a, x0, y0 ), bpp );   /* corners */
    memcpy( res_px( a, x1 + 1, y0 - 1 ), res_px( a, x1, y0 ), bpp );
    memcpy( res_px( a, x0 - 1, y1 + 1 ), res_px( a, x0, y1 ), bpp );
    memcpy( res_px( a, x1 + 1, y1 + 1 ), res_px( a, x1, y1 ), bpp );
}

/* Blit a tenant's retained source into the resident buffer at its current origin.  Row-major, and
   `bpp` scales both strides -- the one place the two pixel formats differ inside the mechanism. */
static void
res_blit_tenant( res_atlas_t* a, const res_tenant_t* t )
{
    u32 bpp = a->bpp;
    for ( u32 r = 0; r < t->h; ++r )
        memcpy( res_px( a, t->ox, t->oy + r ), &t->src[ (size_t)r * t->w * bpp ], (size_t)t->w * bpp );

    if ( a->extrude )
        res_extrude_tenant( a, t );
}

/* Pack one (w+pad, h+pad) rect into an explicit packer context; on success reports the tenant's
   origin -- the packed cell's, moved in by `inset` so the tenant sits inside its own gutter ring.
   Trial and master replay both come through here so they share one packing rule byte-for-byte. */
static bool
res_pack_one_ctx( stbrp_context* ctx, u32 pad, u32 inset, u32 w, u32 h, u32* ox, u32* oy )
{
    stbrp_rect rc = { .w = (stbrp_coord)( w + pad ),
                      .h = (stbrp_coord)( h + pad ) };
    if ( !stbrp_pack_rects( ctx, &rc, 1 ) || !rc.was_packed )
        return false;
    *ox = (u32)rc.x + inset;
    *oy = (u32)rc.y + inset;
    return true;
}

/* Same, into the instance's master packer. */
static bool
res_pack_one( res_atlas_t* a, u32 w, u32 h, u32* ox, u32* oy )
{
    return res_pack_one_ctx( &a->pack, a->pad, a->inset, w, h, ox, oy );
}

/*==============================================================================================
    Repack, TRANSACTIONAL -- trial then commit.

    The trial packs every live tenant (tallest first for a tighter skyline) into a LOCAL packer
    over shared scratch nodes, at whatever dimensions the caller is considering.  It touches
    nothing in the instance, so a failed trial leaves the master packer, every tenant origin and
    the pixel buffer exactly as they were -- the caller may then try larger dimensions or give
    up cleanly.  The commit re-initializes the master packer and REPLAYS the identical sequence:
    stb_rect_pack's skyline is deterministic (no randomness, fixed BL heuristic, and our one-
    rect-per-call pattern leaves its internal sort nothing to reorder), so the same fresh
    context and the same (w,h) sequence give the same placements -- asserted below as a tripwire
    against a future stb upgrade changing that.
==============================================================================================*/

/* Trial scratch, shared by all three instances (the GUI is single-threaded).  Sized for the
   growth cap: a trial may probe dimensions larger than any current atlas. */
static stbrp_node s_trial_nodes[ GUI_RES_ATLAS_DIM_CAP ];

/* Collect + sort the live tenant set and trial-pack it at (try_w x try_pack_h).  Writes the
   tenant order and their would-be origins; returns false when the set does not fit. */
static bool
res_repack_trial( res_atlas_t* a, u32 try_w, u32 try_pack_h,
                  u32* order, u32* n_out, u32* nx, u32* ny )
{
    /* Place tallest tenants first: stb_rect_pack's skyline packs tighter that way, and an
       incremental burst of registrations can otherwise leave later (larger) rects homeless. */
    u32 n = 0;
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
        if ( a->tenants[ i ].used )
            order[ n++ ] = i;
    for ( u32 x = 0; x + 1 < n; ++x )
        for ( u32 y = x + 1; y < n; ++y )
            if ( a->tenants[ order[ y ] ].h > a->tenants[ order[ x ] ].h )
            {
                u32 tmp = order[ x ]; order[ x ] = order[ y ]; order[ y ] = tmp;
            }
    *n_out = n;

    stbrp_context trial;
    stbrp_init_target( &trial, (int)try_w, (int)try_pack_h, s_trial_nodes, (int)try_w );
    for ( u32 k = 0; k < n; ++k )
        if ( !res_pack_one_ctx( &trial, a->pad, a->inset,
                                a->tenants[ order[ k ] ].w, a->tenants[ order[ k ] ].h,
                                &nx[ k ], &ny[ k ] ) )
            return false;
    return true;
}

/* Commit a successful trial taken at the instance's CURRENT dimensions: rebuild the master
   packer by replaying the sequence, clear + repaint the buffer, re-blit every tenant. */
static void
res_repack_commit( res_atlas_t* a, const u32* order, u32 n, const u32* nx, const u32* ny )
{
    stbrp_init_target( &a->pack, (int)a->w, (int)res_pack_h( a ), a->nodes, (int)a->w );

    memset( a->pixels, 0, (size_t)a->w * a->h * a->bpp );
    res_paint_assist( a );
    for ( u32 k = 0; k < n; ++k )
    {
        res_tenant_t* t = &a->tenants[ order[ k ] ];
        u32 ox, oy;
        if ( res_pack_one( a, t->w, t->h, &ox, &oy ) )
        {
            /* Master placements are the truth the skyline will build future fast-path adds on;
               the assert pins them to the trial's (the determinism contract). */
            ORB_ASSERT_MSG( ox == nx[ k ] && oy == ny[ k ],
                            "atlas repack replay diverged from its trial -- stb_rect_pack "
                            "determinism contract broken (vendored copy upgraded?)" );
            t->ox = ox;
            t->oy = oy;
        }
        else
        {
            /* Replay refusing what the trial placed should be impossible; fall back to the
               trial's origin -- collision-free by construction -- and let a later pressure
               repack straighten the (now conservative) master skyline. */
            ORB_ASSERT_MSG( false, "atlas repack replay failed a rect its trial placed" );
            t->ox = nx[ k ];
            t->oy = ny[ k ];
        }
        res_blit_tenant( a, t );
    }

    a->dirty = true;
}

/*==============================================================================================
    Growth -- the pressure valve.  res_place makes the current tenant set fit, cheapest remedy
    first: trial at the current dimensions; on failure retire STALE FONTS one at a time (their
    freed tenants often make the set fit without growing at all); only when nothing stale
    remains take a growth rung (doubling the smaller dimension) and trial again.  Commit at the
    first size that fits -- swapping in a larger texture when that size is not the current one.
    Fully transactional: on false NOTHING has moved, and the one failure left is a tenant set
    the fully-grown atlas cannot hold, reported loudly with occupancy numbers.
==============================================================================================*/

/* Upward seam: the font resolver's stale-font eviction (frame/gui_frame_resolve.c).  Retiring
   a stale font re-enters this unit only through res_atlas_remove / res_sdf_remove -- safe
   between trials, since the abandoned rect is reclaimed by the very next trial repack. */
bool font_resolve_evict_stale( void );

/* Percent of the packable region covered by packed cells (tenant + pad), and the live count. */
static void
res_occupancy( const res_atlas_t* a, f32* pct, u32* tenants )
{
    u64 covered = 0;
    u32 n       = 0;
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
        if ( a->tenants[ i ].used )
        {
            covered += (u64)( a->tenants[ i ].w + a->pad ) * ( a->tenants[ i ].h + a->pad );
            ++n;
        }
    u64 area = (u64)a->w * res_pack_h( a );
    if ( pct )     *pct = area ? 100.0f * (f32)covered / (f32)area : 0.0f;
    if ( tenants ) *tenants = n;
}

/* Swap in a larger texture: new CPU buffer + new GPU texture BEFORE the old is destroyed, so a
   failure leaves everything as it was.  The old texture dies under the RHI's deferred-destroy
   epoch -- in-flight frames sampling it are safe.  Pixels are blank until the caller commits a
   repack into the new buffer; the normal dirty -> flush upload carries them to the GPU. */
static bool
res_resize( res_atlas_t* a, u32 nw, u32 nh )
{
    u8* np = (u8*)calloc( (size_t)nw * nh * a->bpp, 1 );
    if ( !np )
        return false;

    gui_atlas_t na;
    if ( !gui_atlas_create( &na, nw, nh, a->bpp, np, a->debug_name ) )
    {
        free( np );
        return false;
    }

    gui_atlas_destroy( &a->atlas );
    a->atlas = na;
    free( a->pixels );
    a->pixels = np;
    a->w      = nw;
    a->h      = nh;
    return true;
}

static bool
res_place( res_atlas_t* a, u32 want_w, u32 want_h )
{
    u32 order[ GUI_RES_ATLAS_MAX_TENANTS ];
    u32 nx   [ GUI_RES_ATLAS_MAX_TENANTS ];
    u32 ny   [ GUI_RES_ATLAS_MAX_TENANTS ];
    u32 n = 0;

    u32 try_w = a->w, try_h = a->h;
    for ( ;; )
    {
        u32 try_pack_h = a->assist ? try_h - RES_ASSIST_ROWS : try_h;
        if ( res_repack_trial( a, try_w, try_pack_h, order, &n, nx, ny ) )
            break;

        /* Cheapest remedy first: retire one stale font (immediate-mode retention lapsed) and
           re-trial at the SAME dimensions -- growth is the last resort, not the first.  Only
           the R8 atlases host fonts (coverage + SDF); the sprite atlas has nothing to retire.
           Bounded: each true releases one registry slot, so the ladder runs dry in <= 15. */
        if ( a->bpp == 1 && font_resolve_evict_stale() )
            continue;

        /* Next rung: double the smaller dimension (width first when square). */
        if ( try_w <= try_h && try_w < a->max_w )
            try_w *= 2;
        else if ( try_h < a->max_h )
            try_h *= 2;
        else
        {
            f32 pct; u32 tn;
            res_occupancy( a, &pct, &tn );
            gui_log( GUI_LOG_WARN,
                     "%s: FULL -- a %ux%u tenant cannot be placed at %ux%u (growth capped at "
                     "%ux%u); %u tenants, %.0f%% occupied",
                     a->debug_name, want_w, want_h, a->w, a->h, a->max_w, a->max_h, tn, pct );
            return false;
        }
    }

    if ( try_w != a->w || try_h != a->h )
    {
        u32 ow = a->w, oh = a->h;
        if ( !res_resize( a, try_w, try_h ) )
            return false;
        f32 pct; u32 tn;
        res_occupancy( a, &pct, &tn );
        gui_log( GUI_LOG_INFO, "%s: grown %ux%u -> %ux%u (%u tenants, %.0f%% full)",
                 a->debug_name, ow, oh, a->w, a->h, tn, pct );
    }

    res_repack_commit( a, order, n, nx, ny );
    return true;
}

/*==============================================================================================
    Instance lifecycle -- create / destroy one atlas.  res_init is what res_atlas_init calls at
    boot for the coverage atlas and what the first res_sprite_add calls for the sprite atlas.
==============================================================================================*/

static bool
res_init( res_atlas_t* a, u32 w, u32 h, u32 max_w, u32 max_h,
          u32 bpp, bool assist, bool extrude, const char* debug_name )
{
    memset( a, 0, sizeof( *a ) );
    a->w          = w;
    a->h          = h;
    a->max_w      = max_w > w ? max_w : w;   /* a shrunken boot (stress define) still grows */
    a->max_h      = max_h > h ? max_h : h;
    a->bpp        = bpp;
    a->assist     = assist;
    a->extrude    = extrude;
    a->debug_name = debug_name;

    /* An extruding atlas needs the ring on all four sides, so it pads by two and seats the tenant
       one in; a plain one keeps the single shared gutter it always had. */
    a->inset      = extrude ? 1u : 0u;
    a->pad        = extrude ? ( 2u * GUI_RES_ATLAS_PAD ) : GUI_RES_ATLAS_PAD;

    /* Resident CPU copy: cleared to 0 (transparent) so unpacked space samples as empty. */
    a->pixels = (u8*)calloc( (size_t)w * h * bpp, 1 );
    if ( !a->pixels )
        return false;

    /* Packer works the region ABOVE the assist band; the band itself is fixed and never packed. */
    stbrp_init_target( &a->pack, (int)a->w, (int)res_pack_h( a ), a->nodes, (int)a->w );
    res_paint_assist( a );

    if ( !gui_atlas_create( &a->atlas, w, h, bpp, a->pixels, debug_name ) )
    {
        free( a->pixels ); a->pixels = NULL;
        return false;
    }

    a->generation = 1;
    a->ready      = true;
    return true;
}

static void
res_destroy( res_atlas_t* a )
{
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
    {
        free( a->tenants[ i ].src );
        a->tenants[ i ].src = NULL;
    }
    gui_atlas_destroy( &a->atlas );
    free( a->pixels );
    a->pixels = NULL;
    a->ready  = false;
}

/* Returns true when pixels actually reached the GPU this call.  The frame loop turns that into a
   forced rebuild: new resident art (an icon registered between frames) only becomes visible once a
   widget emits it, and on a clean frame no widget runs at all. */
static bool
res_flush( res_atlas_t* a )
{
    if ( !a->ready || !a->dirty )
        return false;
    gui_atlas_upload( &a->atlas, a->pixels );
    a->dirty = false;
    return true;
}

/*==============================================================================================
    Tenant registration -- the mechanism, over either instance.
==============================================================================================*/

static u32
res_add( res_atlas_t* a, const u8* src, u32 w, u32 h )
{
    if ( !a->ready || !src || w == 0 || h == 0 )
        return 0;

    /* Oversize: the tenant is bigger than even the fully-GROWN texture, so no occupancy, repack
       or growth can ever place it.  Loud, per the codebase's overflow rule, and it names the
       numbers -- this is not a "getting full" condition that a smaller working set would fix,
       and it fails identically on frame one and frame ten thousand.  A distance-field font page
       is what found this: it is several times the area of its coverage twin, so it outgrew an
       atlas the coverage one fits in with room to spare. */
    u32 cap_pack_h = a->assist ? a->max_h - RES_ASSIST_ROWS : a->max_h;
    if ( w + a->pad > a->max_w || h + a->pad > cap_pack_h )
    {
        GUI_WARN_ONCE( "%s: tenant %ux%u does not fit even a fully grown %ux%u atlas (+%u pad, "
                       "%u usable rows) -- it is rejected whole; raise the growth cap\n",
                       a->debug_name, w, h, a->max_w, a->max_h, a->pad, cap_pack_h );
        return 0;
    }

    /* Claim a tenant slot and take our own copy of the pixels (needed to re-blit on a repack). */
    u32 idx = GUI_RES_ATLAS_MAX_TENANTS;
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
        if ( !a->tenants[ i ].used ) { idx = i; break; }
    if ( idx == GUI_RES_ATLAS_MAX_TENANTS )
    {
        gui_log( GUI_LOG_WARN, "%s: tenant table full (%u) -- registration rejected",
                 a->debug_name, GUI_RES_ATLAS_MAX_TENANTS );
        return 0;
    }

    size_t bytes = (size_t)w * h * a->bpp;
    u8*    copy  = (u8*)malloc( bytes );
    if ( !copy )
        return 0;
    memcpy( copy, src, bytes );

    res_tenant_t* t = &a->tenants[ idx ];
    t->used = true;
    t->src  = copy;
    t->w    = w;
    t->h    = h;

    /* Fast path: one incremental pack call.  Under pressure, res_place repacks the whole set --
       growing the texture if it must -- or fails with NOTHING moved (transactional). */
    if ( res_pack_one( a, w, h, &t->ox, &t->oy ) )
    {
        res_blit_tenant( a, t );
        a->dirty = true;
    }
    else if ( !res_place( a, w, h ) )   /* the trial set includes this just-claimed tenant */
    {
        free( t->src );
        *t = ( res_tenant_t ){ 0 };
        return 0;
    }

    ++a->generation;
    return idx + 1;   /* 1-based handle; 0 reserved for "none" */
}

static bool
res_update( res_atlas_t* a, u32 handle, const u8* src, u32 w, u32 h )
{
    if ( !a->ready || handle == 0 || handle > GUI_RES_ATLAS_MAX_TENANTS || !src || w == 0 || h == 0 )
        return false;

    res_tenant_t* t = &a->tenants[ handle - 1 ];
    if ( !t->used )
        return false;

    if ( w == t->w && h == t->h )
    {
        /* Same footprint: replace pixels and re-blit in place -- no repack, origin unchanged.  Bump
           generation anyway: glyph shapes / metrics changed, so cached geometry must re-tessellate. */
        memcpy( t->src, src, (size_t)w * h * a->bpp );
        res_blit_tenant( a, t );
        a->dirty = true;
        ++a->generation;
        return true;
    }

    u32 cap_pack_h = a->assist ? a->max_h - RES_ASSIST_ROWS : a->max_h;
    if ( w + a->pad > a->max_w || h + a->pad > cap_pack_h )
    {
        GUI_WARN_ONCE( "%s: resized tenant %ux%u does not fit even a fully grown %ux%u atlas -- "
                       "update rejected\n", a->debug_name, w, h, a->max_w, a->max_h );
        return false;
    }

    /* Different footprint: swap the source and re-place the whole set (this tenant's old rect is
       freed, origins may move, the texture may grow).  res_place is transactional, so on failure
       nothing has moved -- the tenant keeps its old, still-blitted pixels; just restore its
       source fields. */
    u8* copy = (u8*)malloc( (size_t)w * h * a->bpp );
    if ( !copy )
        return false;
    memcpy( copy, src, (size_t)w * h * a->bpp );

    u8* old_src = t->src;
    u32 old_w = t->w, old_h = t->h;
    t->src = copy;
    t->w   = w;
    t->h   = h;

    if ( !res_place( a, w, h ) )
    {
        free( t->src );
        t->src = old_src;
        t->w   = old_w;
        t->h   = old_h;
        return false;
    }

    free( old_src );
    ++a->generation;
    return true;
}

/* Release a tenant: free its retained source and abandon its rect in place.  The stale pixels stay
   blitted (nothing references them) and the skyline keeps the hole until the next repack reclaims
   it.  No generation bump -- no surviving tenant's origin moved, so no baked UV went stale. */
static void
res_remove( res_atlas_t* a, u32 handle )
{
    if ( handle == 0 || handle > GUI_RES_ATLAS_MAX_TENANTS || !a->tenants[ handle - 1 ].used )
        return;
    res_tenant_t* t = &a->tenants[ handle - 1 ];
    free( t->src );
    *t = ( res_tenant_t ){ 0 };
}

static void
res_origin( const res_atlas_t* a, u32 handle, u32* ox, u32* oy )
{
    if ( handle == 0 || handle > GUI_RES_ATLAS_MAX_TENANTS || !a->tenants[ handle - 1 ].used )
    {
        if ( ox ) *ox = 0;
        if ( oy ) *oy = 0;
        return;
    }
    const res_tenant_t* t = &a->tenants[ handle - 1 ];
    if ( ox ) *ox = t->ox;
    if ( oy ) *oy = t->oy;
}

/*==============================================================================================
    THE COVERAGE ATLAS -- public binding.  Stood up at backend boot; shutdown tears down BOTH
    instances, since the sprite atlas has no lifecycle entry point of its own (it is created on
    demand and lives until the backend goes down).
==============================================================================================*/

bool res_atlas_init( void )
{
    memset( &s_spr, 0, sizeof( s_spr ) );   /* not created until the first sprite registers */
    memset( &s_sdf, 0, sizeof( s_sdf ) );   /* not created until an SDF font loads          */
    return res_init( &s_res, GUI_RES_ATLAS_W, GUI_RES_ATLAS_H,
                     GUI_RES_ATLAS_DIM_CAP, GUI_RES_ATLAS_DIM_CAP,
                     1u, true, false, "gui_res_atlas" );
}

void res_atlas_shutdown( void )
{
    res_destroy( &s_res );
    res_destroy( &s_spr );
    res_destroy( &s_sdf );
}

/* Resident CPU heap one atlas owns: the staging mirror plus every tenant's retained source copy.
   Summed across the three instances for the memory accounting (gui_render_mem.c) -- the instance
   RECORDS are statics counted there by sizeof; this is the malloc'd side. */
static u32
res_cpu_heap_bytes( const res_atlas_t* a )
{
    u32 b = a->pixels ? (u32)( (size_t)a->w * a->h * a->bpp ) : 0u;
    for ( u32 i = 0; i < GUI_RES_ATLAS_MAX_TENANTS; ++i )
        if ( a->tenants[ i ].used )
            b += a->tenants[ i ].w * a->tenants[ i ].h * a->bpp;
    return b;
}

u32
res_atlas_cpu_bytes( void )
{
    return res_cpu_heap_bytes( &s_res ) + res_cpu_heap_bytes( &s_spr )
         + res_cpu_heap_bytes( &s_sdf );
}

/* Every atlas flushes here so the frame loop keeps ONE upload seam.  Not short-circuited: a dirty
   sprite or SDF atlas must upload even when the coverage atlas is clean, and the caller's "pixels
   were sent" verdict is the OR across all three. */
bool res_atlas_flush_upload( void )
{
    bool sent = res_flush( &s_res );
    sent      = res_flush( &s_spr ) || sent;
    return      res_flush( &s_sdf ) || sent;
}

u32  res_atlas_add       ( const u8* src, u32 w, u32 h )              { return res_add   ( &s_res, src, w, h ); }
bool res_atlas_update    ( u32 h_, const u8* src, u32 w, u32 h )      { return res_update( &s_res, h_, src, w, h ); }
void res_atlas_remove    ( u32 handle )                               { res_remove( &s_res, handle ); }
void res_atlas_origin    ( u32 handle, u32* ox, u32* oy )             { res_origin( &s_res, handle, ox, oy ); }

/* Dimension accessors read the INSTANCE (the atlas grows), falling back to the boot constants
   while a lazily-created atlas does not exist yet -- a caller asking a not-yet-created atlas for
   its UV scale must not get 1/0.  The coverage atlas is created at boot, so it has no fallback. */

u32  res_atlas_idx        ( void ) { return s_res.atlas.atlas_idx; }
f32  res_atlas_inv_w      ( void ) { return 1.0f / (f32)s_res.w; }
f32  res_atlas_inv_h      ( void ) { return 1.0f / (f32)s_res.h; }
u32  res_atlas_generation ( void ) { return s_res.generation; }
u32  res_atlas_bytes      ( void ) { return s_res.w * s_res.h; }

void res_atlas_size( u32* w, u32* h ) { *w = s_res.w; *h = s_res.h; }

void res_atlas_occupancy( f32* pct, u32* tenants, u32* w, u32* h )
{
    res_occupancy( &s_res, pct, tenants );
    if ( w ) *w = s_res.w;
    if ( h ) *h = s_res.h;
}

void res_sprite_occupancy( f32* pct, u32* tenants, u32* w, u32* h )
{
    if ( !s_spr.ready )
    {
        if ( pct ) *pct = 0.0f;  if ( tenants ) *tenants = 0;
        if ( w )   *w   = 0;     if ( h )       *h       = 0;
        return;
    }
    res_occupancy( &s_spr, pct, tenants );
    if ( w ) *w = s_spr.w;
    if ( h ) *h = s_spr.h;
}

void res_sdf_occupancy( f32* pct, u32* tenants, u32* w, u32* h )
{
    if ( !s_sdf.ready )
    {
        if ( pct ) *pct = 0.0f;  if ( tenants ) *tenants = 0;
        if ( w )   *w   = 0;     if ( h )       *h       = 0;
        return;
    }
    res_occupancy( &s_sdf, pct, tenants );
    if ( w ) *w = s_sdf.w;
    if ( h ) *h = s_sdf.h;
}

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

/*==============================================================================================
    THE SPRITE ATLAS -- public binding, created on demand.

    res_sprite_add is the only entry point that can bring the texture into existence; every other
    verb reads an atlas that may never have been created and answers the not-ready value (0 index,
    0 bytes, origin 0,0).  That is what lets the whole sprite path be optional with no ordering
    rule for a host to remember.
==============================================================================================*/

u32
res_sprite_add( const u8* rgba, u32 w, u32 h )
{
    if ( !s_spr.ready && !res_init( &s_spr, GUI_RES_ATLAS_W, GUI_RES_ATLAS_H,
                                    GUI_SPR_ATLAS_DIM_CAP, GUI_SPR_ATLAS_DIM_CAP,
                                    GUI_SPR_ATLAS_BPP, false, true, "gui_sprite_atlas" ) )
        return 0;
    return res_add( &s_spr, rgba, w, h );
}

bool res_sprite_update ( u32 handle, const u8* rgba, u32 w, u32 h ) { return res_update( &s_spr, handle, rgba, w, h ); }
void res_sprite_origin ( u32 handle, u32* ox, u32* oy )             { res_origin( &s_spr, handle, ox, oy ); }

u32  res_sprite_idx        ( void ) { return s_spr.atlas.atlas_idx; }
f32  res_sprite_inv_w      ( void ) { return s_spr.ready ? 1.0f / (f32)s_spr.w : 1.0f / (f32)GUI_RES_ATLAS_W; }
f32  res_sprite_inv_h      ( void ) { return s_spr.ready ? 1.0f / (f32)s_spr.h : 1.0f / (f32)GUI_RES_ATLAS_H; }
u32  res_sprite_generation ( void ) { return s_spr.generation; }
u32  res_sprite_bytes      ( void ) { return s_spr.ready ? s_spr.w * s_spr.h * GUI_SPR_ATLAS_BPP : 0u; }

/*==============================================================================================
    THE SDF ATLAS -- public binding, created on demand.

    Same R8 byte as the coverage atlas and the same packer; what differs is only what the byte
    MEANS (128 = on the outline, see orb_font.h) and therefore how it must be sampled -- LINEAR, so
    the fragment can take a derivative across it.  That single difference is the whole reason it
    cannot be a tenant of the coverage atlas: a sampler is chosen per DRAW, and the coverage atlas
    must stay NEAREST for bitmap glyphs to survive.

    It EXTRUDES, unlike its coverage twin: a tenant's outer edge is replicated into its private
    ring so a bilinear tap there cannot pull the cleared gutter's 0 -- which reads as "far outside"
    and would carve a false outline around a glyph's padding.  Created by the first res_sdf_add, so
    a build with no distance-field font pays neither the quarter-megabyte nor the bindless slot.
==============================================================================================*/

u32
res_sdf_add( const u8* src, u32 w, u32 h )
{
    if ( !s_sdf.ready && !res_init( &s_sdf, GUI_SDF_ATLAS_W, GUI_SDF_ATLAS_H,
                                    GUI_RES_ATLAS_DIM_CAP, GUI_RES_ATLAS_DIM_CAP,
                                    1u, false, true, "gui_sdf_atlas" ) )
        return 0;
    return res_add( &s_sdf, src, w, h );
}

bool res_sdf_update ( u32 handle, const u8* src, u32 w, u32 h ) { return res_update( &s_sdf, handle, src, w, h ); }
void res_sdf_remove ( u32 handle )                              { res_remove( &s_sdf, handle ); }
void res_sdf_origin ( u32 handle, u32* ox, u32* oy )            { res_origin( &s_sdf, handle, ox, oy ); }

u32  res_sdf_idx        ( void ) { return s_sdf.atlas.atlas_idx; }
/* Instance dims once created (the atlas grows); the boot constants before -- a lazily created
   atlas is asked for its UV scale before it exists, and 1/0 is not a useful answer. */
f32  res_sdf_inv_w      ( void ) { return s_sdf.ready ? 1.0f / (f32)s_sdf.w : 1.0f / (f32)GUI_SDF_ATLAS_W; }
f32  res_sdf_inv_h      ( void ) { return s_sdf.ready ? 1.0f / (f32)s_sdf.h : 1.0f / (f32)GUI_SDF_ATLAS_H; }
u32  res_sdf_generation ( void ) { return s_sdf.generation; }
u32  res_sdf_bytes      ( void ) { return s_sdf.ready ? s_sdf.w * s_sdf.h : 0u; }

void res_sdf_size( u32* w, u32* h )
{
    *w = s_sdf.ready ? s_sdf.w : GUI_SDF_ATLAS_W;
    *h = s_sdf.ready ? s_sdf.h : GUI_SDF_ATLAS_H;
}

// clang-format on
/*============================================================================================*/
