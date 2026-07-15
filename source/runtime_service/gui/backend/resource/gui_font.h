/*==============================================================================================

    runtime_service/gui/backend/resource/gui_font.h -- Font types shared with gui_backend.h.

    Everything declared here is a type, not a function -- these are shared between
    gui_font_internal.c (registry, glyph dispatch, the .orb_font loader -- everything static) and
    gui_font.c (the public API, gui_backend.h) since both need font_slot_t / font_metrics_t.  The
    font unit's actual public surface lives in gui_backend.h and gui_font.c; see the header
    comment in gui_font_internal.c for how the split works.

    The .orb_font is currently the only font source format gui loads, so we assume it.

    A loaded font is no longer its own GPU texture: its baked glyph atlas is packed as a tenant of
    the one shared resource atlas (gui_res_atlas.h), and the solid-fill white texel + dashed-line
    rows live once in that atlas's assist band rather than per font.  A font slot therefore holds
    only a tenant handle into the shared atlas; glyph UVs are rebased through res_atlas_origin at
    dispatch time.  v3 .orb_font atlases are pure glyph coverage; a legacy v2 atlas carries a blank
    trailing band that is simply copied in as dead space (both load identically).

==============================================================================================*/
#pragma once

#include "tools/font_tool/orb_font.h" /* orb_font_glyph_t and the .orb_font on-disk format */

// clang-format off

/* Capacity of the loaded-font registry (gui_font_internal.c).
   Slot 0 is the default; loaded fonts occupy ids 1..GUI_FONT_REGISTRY_MAX-1. */

#define GUI_FONT_REGISTRY_MAX 16

/*==============================================================================================

    font_typography_t -- pure type metrics: what layout code reads (font_char_h / font_line_h /
    font_em).  Nothing here names a GPU resource.

==============================================================================================*/

typedef struct
{
    f32  line_h;   // total line advance
    f32  char_h;   // pixel height of the glyph box (ascent + descent)
    f32  size;     // nominal type size (em) in pixels -- the base for layout proportions

} font_typography_t;

/*==============================================================================================
    font_metrics_t -- everything the active-font accessors (s_font) read, resolved once at load.

    Only typography lives here now: the atlas-sampling parameters (bindless index, white texel, UV
    scale, dash rows) are properties of the shared resource atlas, not of a font, and are read
    directly from gui_res_atlas.h.  A font's only atlas state is its tenant handle (font_slot_t).
==============================================================================================*/

typedef struct
{
    font_typography_t   type;               // s_font->type.char_h, etc.

} font_metrics_t;

/*==============================================================================================
    font_slot_t -- one registry entry: a loaded proportional .orb_font.
==============================================================================================*/

typedef struct
{
    font_metrics_t      metrics;            // first: resolved metrics; s_font points here when active

    bool                used;               // slot occupied

    u32                 atlas_tenant;       // handle into the shared resource atlas (0 = none)

    i32                 ascent;             // pixels above baseline (positive)
    i32                 descent;            // pixels below baseline (negative)
    orb_font_glyph_t    lookup[ ORB_FONT_CP_COUNT ];  // codepoints 32..126; advance == 0 marks a missing glyph

} font_slot_t;

/*============================================================================================*/
// clang-format on