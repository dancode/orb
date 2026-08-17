/*==============================================================================================

    runtime_service/gui/render/resource/gui_glyph_table.h -- the glyph uv table.

    A bindless storage buffer of one uv rect per glyph slot, addressed by a STABLE id: each font
    registry slot owns a fixed window of GUI_FONT_GLYPH_TABLE_PER_FONT entries, and a glyph's
    index inside it (font_glyph_table_index, the font/ leaf) depends only on the font's own
    glyph set -- never on where the atlas packed the page.  When a repack or a growth rung moves
    tenant origins, the table is REWRITTEN IN PLACE and re-uploaded; every id keeps naming the
    same glyph.  That is the property the quad-record renderer builds on: retained text that
    references glyphs by id samples correctly after a repack with no re-tessellation, where
    uv-baking consumers must fold res_atlas_generation into their cache hash and rebuild.

    One entry is a single float4 -- u0, v0, u1, v1, normalized to the font's own backing atlas
    (coverage or SDF; the consumer's tex word already says which texture it samples).  A font
    with no tenant, an empty record, and every unused window entry hold the zero rect, which
    rasterizes as a degenerate quad.

    Upkeep is a per-frame latch, not a hook: glyph_table_sync runs right after the frame loop's
    res_atlas_flush_upload and compares a change signature -- both atlas generations plus each
    font's (tenant, atlas kind, ext count) -- against what it last built.  Any difference
    rebuilds the whole mirror and uploads it once; the table is ~128 KB and changes only when a
    font loads or the atlas reshuffles, so wholesale is cheaper than tracking.

    The GPU buffer is created LAZILY by the first sync that has a packed font, so a build that
    never loads one pays nothing.  Included by gui_render.c after gui_res_atlas.c (origins and
    inverse dimensions are read live from there).

==============================================================================================*/
#pragma once

#include "runtime_service/gui/font/gui_font.h"   /* the registry + the per-font window span */

// clang-format off

/* Total table entries: one fixed window per registry slot.  16 fonts x 512 entries x 16 B =
   128 KB, CPU mirror and GPU buffer alike. */
#define GUI_GLYPH_TABLE_SLOTS  ( GUI_FONT_REGISTRY_MAX * GUI_FONT_GLYPH_TABLE_PER_FONT )

/* Rebuild + upload the table when the fonts or their atlas placements changed since the last
   sync.  Called once per frame from the frame orchestrator, after res_atlas_flush_upload, so
   every placement committed this latch is what the table captures.  Returns true when a rebuild
   was uploaded.  A rebuild dirties nothing downstream -- rewriting in place under stable ids is
   the entire point. */
bool glyph_table_sync      ( void );

/* Unregister + destroy the GPU buffer (render_shutdown; a no-op if it was never created). */
void glyph_table_shutdown  ( void );

/* The table's bindless buffer slot (0 = not created: no font has packed yet, or creation
   failed).  A consumer holding 0 falls back to baked uvs. */
u32  glyph_table_idx       ( void );

/* The global stable slot id for (font id, codepoint): the font's window base plus
   font_glyph_table_index.  Total, like the index -- misses resolve inside the window. */
u32  glyph_table_slot      ( u32 font_id, u32 cp );

/* Read one entry from the CPU mirror (diagnostics / verification).  False when `slot` is out of
   range; entries never written are the zero rect. */
bool glyph_table_entry     ( u32 slot, f32* u0, f32* v0, f32* u1, f32* v1 );

/* GPU bytes held (0 until created) -- memory accounting. */
u32  glyph_table_gpu_bytes ( void );

// clang-format on
/*============================================================================================*/
