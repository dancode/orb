/*==============================================================================================

    runtime_service/gui/render/resource/gui_res_atlas.h -- The shared GUI resource atlas.

    ONE owned R8 coverage texture (GUI_RES_ATLAS_W x GUI_RES_ATLAS_H) with a single bindless slot.
    Every core UI draw resource -- font glyph atlases, the runtime icon set, and the solid/dash
    drawing assists -- is packed into this one texture as a rectangular "tenant".  Because they all
    resolve to the same bindless index (res_atlas_idx), the tessellator's tex_idx-adjacency batcher
    (tess_ensure_gpu_cmd) merges text, solid fills, dashed lines and icons into one draw call per
    clip/viewport scope instead of one per resource.  User RGBA images stay their own tex_idx (they
    are not tenants here) -- the atlas is the DEFAULT that covers core UI, not the only texture.

    Layout: a fixed full-width assist band (white texel + GUI_DASH_PATTERN_COUNT dash rows) is
    reserved at the very bottom of the texture, exactly as each font atlas used to carry per-atlas
    (font_finalize_atlas), but now once for the whole GUI and independent of any loaded font.  The
    remaining top region is an incremental stb_rect_pack area: fonts and icons are packed as they
    are registered (res_atlas_add) -- adding one rect is a single incremental pack call, no repack.
    A repack (res_atlas_repack, driven from res_atlas_add / res_atlas_update) only runs when a rect
    no longer fits or a tenant is resized; it re-blits every tenant from its retained CPU source, so
    the atlas owns a copy of each tenant's pixels.  Tenant origins can move across a repack, so
    callers read placement live via res_atlas_origin rather than caching absolute UVs long-term.

    GPU upload is deferred: add/update/repack only touch the resident CPU buffer and set `dirty`;
    res_atlas_flush_upload (called from frame_begin) re-uploads once per frame when needed.  The GPU
    texture and its bindless slot are created once and never destroyed until shutdown, so a live font
    reload mutates pixels within the persistent texture rather than churning the bindless slot -- the
    VK_ERROR_DEVICE_LOST hazard the per-font-atlas rebuild used to guard against does not arise.

    res_atlas_generation bumps on every structural (UV-affecting) change so the retained render cache
    can fold it into its per-window hash and re-tessellate geometry whose baked UVs went stale.

    Included by gui_render.c before the pipeline stages that sample it, and by gui_draw.c, whose
    fonts and icons are this atlas's tenants -- they pack in from outside the render unit.

==============================================================================================*/
#pragma once

#include "runtime_service/gui/render/resource/gui_atlas.h" /* gui_atlas_t -- the owned GPU texture */

// clang-format off

/* One owned R8 texture, 1 byte / pixel.  512x512 = 256 KiB resident CPU + 256 KiB GPU: fits the
   default UI font(s) plus the editor icon set with room to spare.  Bump to 1024 (or larger) if a
   build packs many large fonts/icons and res_atlas_add starts reporting the atlas full. */
#define GUI_RES_ATLAS_W            512u
#define GUI_RES_ATLAS_H            512u

/* Max distinct packed rects: font slots (<= GUI_FONT_REGISTRY_MAX) + icons (<= ICON_MAX). */
#define GUI_RES_ATLAS_MAX_TENANTS  320u

/* 1px gutter around every packed rect -- keeps a rect's edge texels from being reached by a
   neighbour under any future non-nearest sampling, and matches the old icon-atlas ICON_PAD. */
#define GUI_RES_ATLAS_PAD          1u

/* Dash-pattern rows in the assist band (was in gui_font.h; assists are atlas-level now). */
#define GUI_DASH_PATTERN_COUNT     4

/*==============================================================================================
    Lifecycle (called from backend_init / backend_exit and frame_begin).
==============================================================================================*/

bool res_atlas_init          ( void );   // create the texture + resident buffer, paint the assist band
void res_atlas_shutdown      ( void );   // destroy the texture, free the resident buffer + tenant sources
bool res_atlas_flush_upload  ( void );   // re-upload if dirty (deferred); true when pixels were sent

/*==============================================================================================
    Tenant registration -- fonts (draw/gui_glyph_internal.c) and icons (gui_icon.c) pack through here.
==============================================================================================*/

/* Copy w*h R8 coverage into the atlas and return a 1-based tenant handle (0 = out of room / bad
   args).  Packs incrementally; on a full packer it attempts one repack that folds this tenant in. */
u32  res_atlas_add           ( const u8* src, u32 w, u32 h );

/* Replace tenant `handle`'s pixels.  Same w,h -> in-place re-blit; different -> full repack (tenant
   origins may move).  Returns false on a bad handle or when a resized tenant no longer fits. */
bool res_atlas_update        ( u32 handle, const u8* src, u32 w, u32 h );

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
u32  res_atlas_bytes         ( void );          // GPU bytes held (W*H, R8) -- memory accounting

// clang-format on
/*============================================================================================*/
