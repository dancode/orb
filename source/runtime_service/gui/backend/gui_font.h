/*==============================================================================================

    runtime_service/gui/backend/gui_font.h -- Font types shared with gui_backend.h.

    Everything declared here is a type, not a function: gui_font.c is the font unit's only
    translation unit (registry, glyph dispatch, and the .orb_font loader all live there now), so
    there is nothing left to share across files. The font unit's actual public surface (font_load,
    font_glyph, etc.) lives in gui_backend.h; see the PUBLIC / BACKEND-INTERNAL / FILE-LOCAL
    banners inside gui_font.c for how the rest is split.

    The .orb_font is currently the only font source format gui loads, so we assume it.

    Every atlas is finalized the same way (font_finalize_atlas): a white texel row and
    GUI_DASH_PATTERN_COUNT stipple rows are appended so the active font alone backs solid fills,
    dashed strokes, and text in one texture.

==============================================================================================*/
#pragma once

#include "tools/font_tool/orb_font.h" /* orb_font_glyph_t and the .orb_font on-disk format */
#include "runtime_service/gui/backend/gui_atlas.h" /* gui_atlas_t -- the owned GPU atlas */

// clang-format off
/*----------------------------------------------------------------------------------------------
    Atlas tail: white texel + dash patterns

    A fixed set of full-width 1-row stipple patterns is appended to every atlas after the white
    texel row.  Each row encodes ONE dash period: the leftmost `duty * atlas_w` texels are opaque
    (the "on" run), the rest zero.  Tessellated dashed lines sample these as a single oriented
    quad whose U spans 0..len/period; with REPEAT addressing on U the row tiles along the line --
    O(1) geometry instead of one quad per dash.  Glyph U coords never leave [0,1], so REPEAT does
    not affect text sampling.
----------------------------------------------------------------------------------------------*/

#define GUI_DASH_PATTERN_COUNT 4

/* Capacity of the loaded-font registry (gui_font.c).  Slot 0 is the default; loaded fonts occupy
   ids 1..GUI_FONT_REGISTRY_MAX-1. */
#define GUI_FONT_REGISTRY_MAX 16

/*----------------------------------------------------------------------------------------------
    font_typography_t -- pure type metrics: what layout code reads (font_char_h / font_line_h /
    font_em).  Nothing here names a GPU resource.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32  line_h;   // total line advance
    f32  char_h;   // pixel height of the glyph box (ascent + descent)
    f32  size;     // nominal type size (em) in pixels -- the base for layout proportions

} font_typography_t;

/*----------------------------------------------------------------------------------------------
    font_atlas_sample_t -- resolved atlas-sampling parameters: what the tessellator reads
    (font_atlas_idx / font_white_uv / font_dash_v) to place a glyph, fill, or dashed-line quad.
    Kept separate from font_typography_t so "what size is this font" and "how do I sample its
    atlas" are not the same struct -- one is typography, the other is GPU-atlas bookkeeping.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    u32  atlas_idx;     // bindless texture index -- mirrors slot->atlas.atlas_idx (see font_slot_t)

    f32  white_u;       // UV of a guaranteed-opaque texel in this atlas (solid-fill draws)
    f32  white_v;       // sampling it gives r=1.0 so the vertex color drives the draw

    f32  inv_atlas_w;   // 1 / atlas pixel width              -- precomputed so glyph dispatch avoids a divide
    f32  inv_atlas_h;   // 1 / uploaded atlas height (tex_h)  -- per-glyph UV scale, constant per font

    f32  dash_v[ GUI_DASH_PATTERN_COUNT ]; // center V of each appended dash pattern row

} font_atlas_sample_t;

/*----------------------------------------------------------------------------------------------
    font_metrics_t -- everything the active-font accessors (s_font) read, resolved once at load.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    font_typography_t   type;               // s_font->type.char_h, etc.
    font_atlas_sample_t atlas;              // s_font->atlas.atlas_idx, etc.

} font_metrics_t;

/*----------------------------------------------------------------------------------------------
    font_slot_t -- one registry entry: a loaded proportional .orb_font.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    font_metrics_t      metrics;            // first: resolved metrics; s_font points here when active

    bool                used;               // slot occupied

    gui_atlas_t         atlas;              // owned GPU atlas (metrics.atlas.atlas_idx mirrors atlas.atlas_idx)

    i32                 ascent;             // pixels above baseline (positive)
    i32                 descent;            // pixels below baseline (negative)
    orb_font_glyph_t    lookup[ 95 ];       // codepoints 32..126; advance == 0 marks a missing glyph

} font_slot_t;

// clang-format on
/*============================================================================================*/
