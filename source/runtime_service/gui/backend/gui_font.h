/*==============================================================================================

    runtime_service/gui/backend/gui_font.h -- Font types shared across the font unit.

    The font unit is split into three translation units, all included by gui_backend.c:

        gui_font.c      -- neutral: the id-addressed registry, slot lifecycle, glyph dispatch,
                             and the shared atlas finalize (white texel + dash rows + UV metrics).
        gui_font_bmp.c  -- bmp fonts: a monospace R8 grid atlas.  The built-ins are 1-bpp "bit
                             fonts" (compile-time, bit_font_def_t) expanded into a bmp atlas.
        gui_font_ttf.c  -- ttf fonts: a proportional .orb_font loaded at runtime.

    Three font concepts, kept distinct on purpose:

        bit font  -- 1 bit per pixel, packed.  The generated built-in tables (font/*.c) are bit
                     fonts; bit_font_def_t describes one.  Source data only -- never sampled.
        bmp font  -- 8 bits per pixel (R8), a monospace grid atlas.  A bit font is *converted* to a
                     bmp atlas at init; later, externally authored bmp atlases could load the same
                     way without being built in.
        ttf font  -- proportional, per-glyph metrics + UVs, loaded from a baked .orb_font file.

    Every atlas -- bmp or ttf -- is finalized the same way (font_finalize_atlas): a white texel row
    and GUI_DASH_PATTERN_COUNT stipple rows are appended so the active font alone backs solid
    fills, dashed strokes, and text in one texture.

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

/* Capacity of the loaded-font registry (gui_font.c).  Slot 0 is the default / fallback; loaded
   fonts occupy ids 1..GUI_FONT_REGISTRY_MAX-1. */
#define GUI_FONT_REGISTRY_MAX 16

/*----------------------------------------------------------------------------------------------
    font_metrics_t -- pre-resolved metrics shared by every font source; callers read via s_font.
----------------------------------------------------------------------------------------------*/

typedef struct
{
    f32  line_h;        // total line advance
    f32  char_h;        // pixel height of the glyph box (ascent + descent)
    f32  char_w;        // monospace advance (0 for proportional fonts)
    f32  size;          // nominal type size (em) in pixels -- the base for layout proportions
    u32  atlas_idx;     // bindless texture index

    bool proportional;  // true = per-glyph advance from a ttf lookup[]; false = monospace bmp grid

    f32  white_u;       // UV of a guaranteed-opaque texel in this atlas (solid-fill draws)
    f32  white_v;       // sampling it gives r=1.0 so the vertex color drives the draw

    f32  inv_atlas_w;   // 1 / atlas pixel width             -- precomputed so glyph dispatch avoids a divide
    f32  inv_atlas_h;   // 1 / uploaded atlas height (tex_h)  -- per-glyph UV scale, constant per font

    f32  dash_v[ GUI_DASH_PATTERN_COUNT ]; // center V of each appended dash pattern row

} font_metrics_t;

/*----------------------------------------------------------------------------------------------
    bit_font_def_t -- compile-time description of one 1-bit-per-pixel bit font (font/*.c tables).
----------------------------------------------------------------------------------------------*/

typedef struct
{
    u32             atlas_w;            // width of the glyph grid in pixels
    u32             atlas_h;            // height of the glyph grid in pixels
    u32             glyph_w;            // width of each glyph cell in pixels  (0 for whitespace)
    u32             glyph_h;            // height of each glyph cell in pixels

    u32             glyphs_row;         // glyphs per atlas row (16 for our built-ins)
    u32             glyph_count;        // glyph count (96 for our built-ins)
    u32             row_stride;         // bytes per glyph row: 1 for <=8-wide, 2 for 9..16-wide (u16 LE)

    const u8*       data;               // packed 1-bpp bits, bit 0 = leftmost pixel
    const char*     debug_name;

} bit_font_def_t;

/*----------------------------------------------------------------------------------------------
    font_slot_t -- one registry entry; a tagged union over the two runtime font sources.
----------------------------------------------------------------------------------------------*/

typedef enum
{
    FONT_SRC_BMP = 0,   // monospace R8 grid atlas (built from a bit font)
    FONT_SRC_TTF = 1,   // proportional .orb_font

} font_src_t;

/* bmp source: glyph layout comes from the source bit font's grid.  The atlas itself is owned by
   gui_font_bmp.c (the resident built-in set), so a slot referencing it never owns the GPU. */
typedef struct
{
    const bit_font_def_t* def;          // grid dimensions for glyph dispatch

} bmp_slot_t;

/* ttf source: per-glyph records loaded from the .orb_font file. */
typedef struct
{
    i32                ascent;          // pixels above baseline (positive)
    i32                descent;         // pixels below baseline (negative)
    orb_font_glyph_t   lookup[ 95 ];    // codepoints 32..126; advance == 0 marks a missing glyph

} ttf_slot_t;

typedef struct
{
    font_metrics_t    metrics;          // first: resolved metrics; s_font points here when active

    font_src_t        src;              // which union member is valid
    bool              used;             // slot occupied
    bool              owns_atlas;       // true when this slot created its own GPU atlas (ttf loads)

    rhi_texture_t     atlas;            // GPU atlas (owned slots only; referencing slots leave zero)
    u32               atlas_idx;        // bindless index (mirrors metrics.atlas_idx)
    u32               atlas_w;          // atlas width in pixels  (memory accounting)
    u32               atlas_h;          // uploaded atlas height  (memory accounting)

    union
    {
        bmp_slot_t    bmp;
        ttf_slot_t    ttf;
    };

} font_slot_t;

/*----------------------------------------------------------------------------------------------
    Cross-file helpers (the unity build resolves these regardless of include order).

    Neutral (gui_font.c):
        font_slot_free_gpu   -- release a slot's owned GPU atlas; no-op for referencing slots.
        font_atlas_tex_h     -- uploaded height for a glyph region of `glyph_h` rows (adds the tail).
        font_finalize_atlas  -- append the white + dash rows to a staged R8 atlas and fill the
                                metrics UV/scale fields that describe them.  Every builder calls this.

    bmp (gui_font_bmp.c):
        bmp_init / bmp_shutdown -- create / destroy the resident built-in bmp atlases.
        bmp_select / bmp_scale_set -- choose the active built-in and its integer upscale.
        bmp_fill_slot        -- copy the active built-in bmp into a slot (referencing, not owning).
        bmp_glyph            -- glyph draw parameters for a bmp slot.
        bmp_atlas_bytes      -- GPU bytes held by the resident bmp atlases.

    ttf (gui_font_ttf.c):
        ttf_load_file        -- load a .orb_font into a slot (creates an owned atlas).
        ttf_glyph            -- glyph draw parameters for a ttf slot.
        ttf_char_advance     -- horizontal advance of one glyph in a ttf slot.
----------------------------------------------------------------------------------------------*/

void font_slot_free_gpu  ( font_slot_t* slot );
u32  font_atlas_tex_h    ( u32 glyph_h );
void font_finalize_atlas ( u8* pixels, u32 atlas_w, u32 glyph_h, u32 tex_h, font_metrics_t* m );

bool bmp_init            ( void );
void bmp_shutdown        ( void );
void bmp_select          ( gui_font_t font );
void bmp_scale_set       ( u32 scale );
void bmp_fill_slot       ( font_slot_t* slot );
void bmp_glyph           ( const font_slot_t* slot, u8 ch,
                           f32* u0, f32* v0, f32* u1, f32* v1,
                           f32* ox, f32* oy, f32* gw, f32* gh, f32* advance );
u32  bmp_atlas_bytes     ( void );

bool ttf_load_file       ( font_slot_t* slot, const char* path );
void ttf_glyph           ( const font_slot_t* slot, u8 ch,
                           f32* u0, f32* v0, f32* u1, f32* v1,
                           f32* ox, f32* oy, f32* gw, f32* gh, f32* advance );
f32  ttf_char_advance    ( const font_slot_t* slot, u8 ch );

// clang-format on
/*============================================================================================*/
