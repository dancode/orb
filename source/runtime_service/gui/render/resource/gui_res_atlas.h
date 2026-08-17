/*==============================================================================================

    runtime_service/gui/render/resource/gui_res_atlas.h -- The GUI resource atlases.

    THREE atlases over one packer implementation, split by what a texel MEANS -- which is the only
    axis that matters, because it is what the fragment shader branches on (gui_tex_mode_t) and what
    decides the sampler:

        the COVERAGE atlas (res_atlas_*)  -- R8, always resident.  Glyphs, icons, and the
            solid / dash drawing assists.  The texel's R channel is alpha, the vertex colour is
            the RGB, so everything in it tints and everything in it batches together.
        the SPRITE atlas  (res_sprite_*)  -- RGBA8, created LAZILY on the first registration.
            Authored art: sprite quads and nine-slice frames.  The texel IS the colour and the
            vertex colour tints it (GUI_TEX_RGBA in the command's tex_idx mode field selects
            that sampling model).  A build that registers no sprite pays nothing -- no CPU buffer,
            no GPU texture, no bindless slot.
        the SDF atlas     (res_sdf_*)     -- R8, created LAZILY on the first distance-field font.
            The byte is a SIGNED DISTANCE (128 = the outline, orb_font.h), so it must be sampled
            LINEAR; that is the one thing it could not share with the coverage atlas, which must
            stay NEAREST or bitmap glyphs stop being crisp.  Scalable text lives here.

    No two can share a texture -- a pixel format apart, or a sampler apart -- but they DO share a
    draw call: the bindless slot and its sampling model ride the style record (gui.h,
    gui_prim_t), so a window's nine-slice frame, its bitmap labels and an SDF heading go out
    together and only a clip change cuts the batch.  Each is still internally ONE texture with ONE
    bindless slot, which is what keeps the vertex word constant across a whole kind.  They share
    this file, the packer, and the tenant/repack machinery -- the instance record below is the only
    thing there are three of.

    Everything from here down describes all three, with the coverage atlas as the example.

    Every resource -- font glyph atlases, the runtime icon set, the drawing assists, and sprites --
    is packed into its atlas as a rectangular "tenant".  They all resolve to the same bindless index
    (res_atlas_idx), which does not decide batching -- the tessellator cuts a draw call on a
    clip/viewport change alone -- but does decide how much of the texture cache one draw touches:
    text, solid fills, dashed lines and icons read one 1024-square R8 image between them.  A user's
    OWN RGBA image (a scene render target handed to draw_texture_in) stays its own tex_idx and is
    not a tenant of either atlas -- these cover the resources gui itself owns, not every texture
    that can reach the draw list.

    Layout: the coverage atlas reserves a fixed full-width assist band (white texel +
    GUI_DASH_PATTERN_COUNT dash rows) once, at the very bottom of the texture, shared across the
    whole GUI and independent of any loaded font -- not duplicated per font atlas the way a
    per-font assist band would be.  The sprite atlas has no assists and packs its full height.  The
    remaining region is an incremental stb_rect_pack area: tenants are packed as they
    are registered (res_atlas_add) -- adding one rect is a single incremental pack call, no repack.
    A repack (res_atlas_repack, driven from res_atlas_add / res_atlas_update) only runs when a rect
    no longer fits or a tenant is resized; it re-blits every tenant from its retained CPU source, so
    the atlas owns a copy of each tenant's pixels.  Tenant origins can move across a repack, so
    callers read placement live via res_atlas_origin rather than caching absolute UVs long-term.

    GPU upload is deferred: add/update/repack only touch the resident CPU buffer and set `dirty`;
    res_atlas_flush_upload (called from frame_begin) re-uploads once per frame when needed.  The GPU
    texture is destroyed only when the atlas GROWS -- at the frame_begin latch, under the RHI's
    deferred-destroy epoch (vk_garbage), so an in-flight frame sampling the old texture is safe --
    and otherwise lives until shutdown: a live font reload mutates pixels within the persistent
    texture rather than churning the bindless slot per reload.

    GROWTH: an atlas starts at its boot dimensions and grows only as a LAST resort: when the
    tenant set stops fitting, stale fonts (immediate-mode retention lapsed, gui_frame_resolve.c)
    are retired one at a time and the set re-trialed at the current size first; only when
    nothing stale remains does the atlas double (smaller dimension first), up to
    GUI_RES_ATLAS_DIM_CAP (RGBA sprite: GUI_SPR_ATLAS_DIM_CAP).
    Every repack is TRANSACTIONAL -- placements are trialed in a scratch packer first, and on any
    failure the master packer, every tenant origin and the pixel buffer are untouched.  Only a
    tenant set that cannot fit the fully-grown atlas fails, loudly, with occupancy numbers.
    FUTURE WORK: pages pack whole, so a 256-wide page monopolizes a full skyline band; per-glyph
    packing is the real fix for that width waste and is deliberately not built yet.

    res_atlas_generation / res_sprite_generation bump on every structural (UV-affecting) change so
    the retained render cache can fold them into its per-window hash and re-tessellate geometry
    whose baked UVs went stale.

    Included by gui_render.c before the pipeline stages that sample it, and by gui_draw.c, whose
    fonts, icons and sprites are these atlases' tenants -- they pack in from outside the render unit.

==============================================================================================*/
#pragma once

#include "runtime_service/gui/render/resource/gui_atlas.h" /* gui_atlas_t -- the owned GPU texture */

// clang-format off

/* BOOT dimensions of the coverage atlas, 1 byte / pixel.  512x512 = 256 KiB resident CPU +
   256 KiB GPU: fits the default UI font(s) plus the editor icon set.  The atlas GROWS on demand
   from here (doubling up to GUI_RES_ATLAS_DIM_CAP), so these only set the floor a minimal build
   pays.  #ifndef-guarded so a stress target can shrink them (orb.targets `define`) to force the
   growth path.  Dimensions are PER INSTANCE (see the record in the .c). */
#ifndef GUI_RES_ATLAS_W
#define GUI_RES_ATLAS_W            512u
#endif
#ifndef GUI_RES_ATLAS_H
#define GUI_RES_ATLAS_H            512u
#endif

/* The SDF atlas is WIDER, and that is a property of the data rather than a tuning knob.  A
   distance-field glyph carries `spread` px of field on all four sides, so it costs several times
   the area of its coverage twin, and the baker's skyline fills WIDTH first: a 16px face at spread 8
   packs 512 wide.  A font's whole baked page arrives as ONE tenant, so a 512-wide page cannot go
   into a 512-wide atlas at any occupancy -- the tenant plus its ring is simply larger than the
   texture.  That failure is what these dimensions exist to prevent, and its only symptom is one
   upload warning: the atlas stays empty and every glyph samples zero.
   Height is modest because the baker crops the page to the rows it actually packed
   (dev_font_bake_write), so a 16px face is ~512x153 and three fit here.  512 KiB, paid only by a
   build that loads a distance-field font -- the instance is created lazily.  Grows like the
   coverage atlas; only a page too large for the fully-grown atlas is rejected, loudly. */
#ifndef GUI_SDF_ATLAS_W
#define GUI_SDF_ATLAS_W            1024u
#endif
#ifndef GUI_SDF_ATLAS_H
#define GUI_SDF_ATLAS_H            512u
#endif

/* Growth caps, by what a texel costs: the R8 atlases (coverage, SDF) may double up to
   2048x2048 (4 MiB CPU mirror + 4 MiB GPU each, worst case); the RGBA sprite atlas caps at
   1024x1024 (4 MiB) because colour costs 4x per texel. */
#define GUI_RES_ATLAS_DIM_CAP      2048u
#define GUI_SPR_ATLAS_DIM_CAP      1024u

/* Widest any instance may GROW to -- sizes the per-instance packer node scratch. */
#define GUI_RES_ATLAS_MAX_W        GUI_RES_ATLAS_DIM_CAP

/* Max distinct packed rects: font slots (<= GUI_FONT_REGISTRY_MAX) + icons (<= ICON_MAX).  The
   sprite atlas shares the instance record and therefore this bound; it needs far fewer. */
#define GUI_RES_ATLAS_MAX_TENANTS  320u

/* The sprite atlas: same dimensions, 4 bytes / pixel = 1 MiB resident CPU + 1 MiB GPU.  It is
   created LAZILY on the first res_sprite_add, so that megabyte is only paid by a build that
   actually registers authored art. */
#define GUI_SPR_ATLAS_BPP          4u

/* 1px gutter around every packed rect -- keeps a rect's edge texels from being reached by a
   neighbour under any future non-nearest sampling. */
#define GUI_RES_ATLAS_PAD          1u

/* Dash-pattern rows in the assist band, which lives at the atlas level rather than per font. */
#define GUI_DASH_PATTERN_COUNT     4

/*==============================================================================================
    Lifecycle (called from backend_init / backend_exit and frame_begin).
==============================================================================================*/

bool res_atlas_init          ( void );   // create the texture + resident buffer, paint the assist band
void res_atlas_shutdown      ( void );   // destroy BOTH atlases, free resident buffers + tenant sources
bool res_atlas_flush_upload  ( void );   // re-upload either atlas if dirty; true when pixels were sent

/*==============================================================================================
    Tenant registration -- fonts (draw/gui_glyph_internal.c) and icons (gui_icon.c) pack through here.
==============================================================================================*/

/* Copy w*h R8 coverage into the atlas and return a 1-based tenant handle (0 = out of room / bad
   args).  Packs incrementally; on a full packer it attempts one repack that folds this tenant in. */
u32  res_atlas_add           ( const u8* src, u32 w, u32 h );

/* Replace tenant `handle`'s pixels.  Same w,h -> in-place re-blit; different -> full repack (tenant
   origins may move).  Returns false on a bad handle or when a resized tenant no longer fits. */
bool res_atlas_update        ( u32 handle, const u8* src, u32 w, u32 h );

/* Release tenant `handle`: frees its retained source and abandons its rect in place (reclaimed by
   the next repack).  Exists for the one owner that can switch atlases -- a font reload changing
   kind (coverage <-> SDF) -- since a handle only indexes the atlas it was created in. */
void res_atlas_remove        ( u32 handle );

/* Live pixel origin (top-left) of a tenant in the atlas -- valid across repacks.  0,0 if invalid. */
void res_atlas_origin        ( u32 handle, u32* ox, u32* oy );

/*==============================================================================================
    Sampling accessors -- what the tessellator reads (via the font_/icon_ accessor redirects).
==============================================================================================*/

u32  res_atlas_idx           ( void );          // the single bindless texture slot (0 = not ready)
void res_atlas_white_uv      ( f32* u, f32* v );// UV of the opaque assist texel (solid-color draws)
f32  res_atlas_dash_v        ( f32 duty );      // center V of the assist dash row closest to `duty`
f32  res_atlas_inv_w         ( void );          // 1 / atlas pixel width  (per-glyph/icon UV scale)
f32  res_atlas_inv_h         ( void );          // 1 / atlas pixel height
u32  res_atlas_generation    ( void );          // bumps on every UV-affecting structural change
u32  res_atlas_bytes         ( void );          // GPU bytes held (current w*h, R8) -- memory accounting
u32  res_atlas_cpu_bytes     ( void );          // CPU heap held by ALL THREE atlases (mirrors + tenant copies)
void res_atlas_size          ( u32* w, u32* h );// current texture dimensions (grow-aware)

/* Occupancy of the packable region -- percent covered by packed cells (tenant + pad), live tenant
   count, current dimensions.  The sprite / SDF twins answer zeros until their atlas exists.
   Diagnostics only (mem stats, dashboards); nothing in the mechanism reads these. */
void res_atlas_occupancy     ( f32* pct, u32* tenants, u32* w, u32* h );
void res_sprite_occupancy    ( f32* pct, u32* tenants, u32* w, u32* h );
void res_sdf_occupancy       ( f32* pct, u32* tenants, u32* w, u32* h );

/*==============================================================================================
    The SPRITE atlas (RGBA8) -- same six verbs, one texel meaning apart.

    Every entry point mirrors its coverage twin above with one difference: `src` is w*h*4 bytes of
    RGBA8 rather than w*h bytes of coverage.  The first res_sprite_add creates the texture; before
    that res_sprite_idx() reports 0 and a sprite draw is a no-op, so nothing has to be ordered
    against a sprite atlas that may never exist.
==============================================================================================*/

u32  res_sprite_add          ( const u8* rgba, u32 w, u32 h );   // 1-based tenant handle (0 = full)
bool res_sprite_update       ( u32 handle, const u8* rgba, u32 w, u32 h );
void res_sprite_origin       ( u32 handle, u32* ox, u32* oy );

u32  res_sprite_idx          ( void );          // bindless slot (0 = never created / not ready)
f32  res_sprite_inv_w        ( void );          // 1 / atlas pixel width  (per-sprite UV scale)
f32  res_sprite_inv_h        ( void );          // 1 / atlas pixel height
u32  res_sprite_generation   ( void );          // bumps on every UV-affecting structural change
u32  res_sprite_bytes        ( void );          // GPU bytes held (0 until created)

/*==============================================================================================
    The SDF ATLAS (R8 distance field) -- the same six verbs again, one texel meaning apart.

    Byte-for-byte the same shape as the coverage atlas; the difference is that 128 means "on the
    outline" rather than "half covered" (orb_font.h, sdf_range), so it must be sampled LINEAR for
    the fragment to take a derivative across it.  Since a sampler is chosen per DRAW, that is
    exactly why distance-field glyphs need their own texture instead of a flag on the shared one --
    the coverage atlas must stay NEAREST or bitmap text stops being crisp.

    Created by the first res_sdf_add (an SDF font loading), and like the sprite atlas every other
    verb answers the not-ready value until then, so nothing has to be ordered against it.
==============================================================================================*/

u32  res_sdf_add             ( const u8* src, u32 w, u32 h );    // 1-based tenant handle (0 = full)
bool res_sdf_update          ( u32 handle, const u8* src, u32 w, u32 h );
void res_sdf_remove          ( u32 handle );                     // release (see res_atlas_remove)
void res_sdf_origin          ( u32 handle, u32* ox, u32* oy );

u32  res_sdf_idx             ( void );          // bindless slot (0 = never created / not ready)
f32  res_sdf_inv_w           ( void );          // 1 / atlas pixel width  (per-glyph UV scale)
f32  res_sdf_inv_h           ( void );          // 1 / atlas pixel height
u32  res_sdf_generation      ( void );          // bumps on every UV-affecting structural change
u32  res_sdf_bytes           ( void );          // GPU bytes held (0 until created)
void res_sdf_size            ( u32* w, u32* h );// current dimensions (boot constants until created)

// clang-format on
/*============================================================================================*/
