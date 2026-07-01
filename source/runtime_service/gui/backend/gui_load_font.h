/*==============================================================================================

    runtime_service/gui/backend/gui_load_font.h -- Font types shared across the font unit.

    The font unit is split into two translation units, both included by gui_backend.c:

        gui_load_font.c      -- neutral: the id-addressed registry, slot lifecycle, glyph dispatch,
                                  and the shared atlas finalize (white texel + dash rows + UV metrics).
        gui_load_font_ttf.c  -- a proportional .orb_font loaded at runtime.

    Every atlas is finalized the same way (font_finalize_atlas): a white texel row and
    GUI_DASH_PATTERN_COUNT stipple rows are appended so the active font alone backs solid fills,
    dashed strokes, and text in one texture.

==============================================================================================*/
#pragma once

#include "tools/font_tool/orb_font.h"   /* orb_font_glyph_t and the .orb_font on-disk format */

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

/* Capacity of the loaded-font registry (gui_load_font.c).  Slot 0 is the default; loaded fonts occupy
   ids 1..GUI_FONT_REGISTRY_MAX-1. */
#define GUI_FONT_REGISTRY_MAX 16

/*----------------------------------------------------------------------------------------------
    font_metrics_t -- pre-resolved metrics; callers read via s_font.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32  line_h;        // total line advance
    f32  char_h;        // pixel height of the glyph box (ascent + descent)
    f32  size;          // nominal type size (em) in pixels -- the base for layout proportions
    u32  atlas_idx;     // bindless texture index

    f32  white_u;       // UV of a guaranteed-opaque texel in this atlas (solid-fill draws)
    f32  white_v;       // sampling it gives r=1.0 so the vertex color drives the draw

    f32  inv_atlas_w;   // 1 / atlas pixel width             -- precomputed so glyph dispatch avoids a divide
    f32  inv_atlas_h;   // 1 / uploaded atlas height (tex_h)  -- per-glyph UV scale, constant per font

    f32  dash_v[ GUI_DASH_PATTERN_COUNT ]; // center V of each appended dash pattern row

} font_metrics_t;

/*----------------------------------------------------------------------------------------------
    font_slot_t -- one registry entry: a loaded proportional .orb_font.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    font_metrics_t    metrics;          // first: resolved metrics; s_font points here when active

    bool              used;             // slot occupied

    rhi_texture_t     atlas;            // owned GPU atlas
    u32               atlas_idx;        // bindless index (mirrors metrics.atlas_idx)
    u32               atlas_w;          // atlas width in pixels  (memory accounting)
    u32               atlas_h;          // uploaded atlas height  (memory accounting)

    i32                ascent;          // pixels above baseline (positive)
    i32                descent;         // pixels below baseline (negative)
    orb_font_glyph_t   lookup[ 95 ];    // codepoints 32..126; advance == 0 marks a missing glyph

} font_slot_t;

/*----------------------------------------------------------------------------------------------
    Cross-file helpers (the unity build resolves these regardless of include order).

    Neutral (gui_load_font.c):
        font_slot_free_gpu   -- release a slot's owned GPU atlas.
        font_atlas_tex_h     -- uploaded height for a glyph region of `glyph_h` rows (adds the tail).
        font_finalize_atlas  -- append the white + dash rows to a staged R8 atlas and fill the
                                metrics UV/scale fields that describe them.  Every builder calls this.

    ttf (gui_load_font_ttf.c):
        ttf_load_file        -- load a .orb_font into a slot (creates an owned atlas).
        ttf_glyph            -- glyph draw parameters for a slot.
        ttf_char_advance     -- horizontal advance of one glyph in a slot.
----------------------------------------------------------------------------------------------*/

void font_slot_free_gpu  ( font_slot_t* slot );
u32  font_atlas_tex_h    ( u32 glyph_h );
void font_finalize_atlas ( u8* pixels, u32 atlas_w, u32 glyph_h, u32 tex_h, font_metrics_t* m );

bool ttf_load_file       ( font_slot_t* slot, const char* path );
void ttf_glyph           ( const font_slot_t* slot, u8 ch,
                           f32* u0, f32* v0, f32* u1, f32* v1,
                           f32* ox, f32* oy, f32* gw, f32* gh, f32* advance );
f32  ttf_char_advance    ( const font_slot_t* slot, u8 ch );

// clang-format on
/*============================================================================================*/
