# GUI Quad Compaction Plan

How the GUI render record set gets from one 48-byte `gui_quad_t` per quad to a 16-byte root
record, by moving rarity out of the common case instead of compressing the common case in place.

---

## 1. What the measurement says

A menu-bar-only frame of `gui_example` measures **78% glyphs**, ~7 glyphs per text run.
Across several open demo windows the glyph share stays above 70%. Text is the workload; every
other quad class is a rounding error next to it.

Against that, today's `gui_quad_t` spends its third row on fields a glyph never reads:

| row | field | glyph uses it |
|-----|-------|---------------|
| 0 | cx, cy, hw, hh | yes |
| 1 | uv0, uv1 | yes |
| 1 | abgr | yes |
| 1 | style | yes, but the same index for every glyph in the frame |
| 2 | clip | yes |
| 2 | flags | a bit or two |
| 2 | xform (turn) | no -- only `tess_text_xf` |
| 2 | phase | no |
| 2 | col_border | no |

Row 2 is roughly 90% dead weight on 78% of records.

### The design this replaces

An earlier proposal collapsed a text run to one record plus a compact per-glyph strip, with the
vertex stage expanding one record into N quads. It is **rejected**, on numbers rather than taste:

- At 7 glyphs/run a 48-byte run header amortises to ~7 B/glyph; add the 4-byte expansion-table
  entry and the effective cost is ~27 B/glyph.
- Expansion breaks `quad = vid / 6`. Recovery needs a prefix-sum search or a per-element
  back-pointer, and one more arena threaded through slot relocation, volatile patching and the
  flush's upload spans.

A flat 16-byte record beats it on bytes *and* keeps the addressing. The expansion path is
strictly dominated; do not revive it.

---

## 2. Target structure

Four tables, ordered by how often the shader touches them.

| table | one entry per | size | fetched |
|-------|---------------|------|---------|
| **quad** (big) | quad | 16 B | always |
| **glyph UV** (id) | resident glyph | 8 B | glyph tags only |
| **style** `gui_prim_t` (medium) | distinct appearance, deduped | 128 B | non-solid tags |
| **fx** (small) | complex instance | TBD | extended tag only |

The fragment stage composites **fx over style over base**; the fetch order is the reverse, since
the root record is what names the others. Each layer is an additive conditional pass and the base
must be complete standing alone, so the fast path never carries the shaped path's register
pressure.

### The 16-byte root record

```
  offset  size  field
  0       8     pos.x, pos.y | size.w, size.h  -- i16 x2 + u16 x2, 1/4 px, SURFACE space
  8       4     abgr                            -- unchanged
  12      4     index                           -- TAGGED UNION, see below
```

`index` is a tagged union, not a fixed bit layout. This is what makes the budget close: a glyph
never needs a style or fx id, and a shaped quad never needs an atlas id.

| tag (bits 30-31) | layout | bits used |
|------------------|--------|-----------|
| `SOLID` | clip(6) | 6 |
| `GLYPH` | atlas_id(13) + clip(6) | 19 |
| `GLYPH_STYLED` | atlas_id(13) + clip(6) + style(11) | 30 |
| `SHAPED` | style(11) + clip(6) + fx(10) | 27 |

Field widths against today's caps: `GUI_MAX_CLIP_RECTS` 64 -> 6 bits; `GUI_MAX_PRIMS` 2048 ->
11 bits; the resident glyph table is ~1800 entries -> 13 bits carries 8192.

`GLYPH_STYLED` uses 30 of 30 bits. That row has **no slack** -- raising `GUI_MAX_PRIMS` or the
glyph cap forces a re-plan of the union, not a one-line constant bump.

`SOLID` carries no texture reference at all, so the shader skips the sample rather than reading a
known-white texel.

### Why the centre is signed -- and why it is NOT window-relative

Clipping is a **fragment**-stage operation (the bindless clip slab is what keeps the batch key at
viewport only), so scrolled-out geometry is still dispatched carrying its real coordinates. A
long scrolled region emits quads at large negative Y. Unsigned `u16` cannot encode that, which is
why the centre is `i16` and the half-extent -- never negative, and wanting more reach for a
fullscreen backdrop -- is `u16`.

Window-relative was planned and then dropped as a cost with no benefit. The mvp is already a
per-surface pixel ortho, so a coordinate is surface-local before any origin is subtracted; a
window origin is at most a few thousand pixels, smaller than the scroll excursions that are the
real tail risk. Subtracting it would have cost a slot field, a push lane and a volatile-patch
interaction to buy nothing. Out-of-range placements clamp instead: past +-8192 px a quad is far
outside every clip rect either way, and wrapping would drag it back into view.

### Why UV does NOT go in the style record

That was the plan and it was wrong. A uv rect is per-INSTANCE: on the deduped style table every
icon in a toolbar mints its own 128-byte record -- the same failure that pushed `col_b` off
`gui_prim_t`. It belongs beside the other per-instance lanes, so the fx record grew to two rows
(row A the turn/phase/border, row B the uv pair) and a textured quad costs 32 B shared four to a
page. Glyphs never come here at all; their rect is ID-addressed through the glyph table.

---

## 3. The glyph UV table

A previous id-addressed-glyph attempt was reverted for costing ~280 KB and duplicating work that
was already hot at dispatch. Scoped to the font registry, neither objection survives.

`GUI_FONT_REGISTRY_MAX` is 16, and each `font_slot_t` holds a dense `lookup[95]` for codepoints
32..126 plus a sorted `ext[]`:

- 16 x 95 = **1520** ASCII entries, plus a few hundred extended
- 8 B each (uv0, uv1 as packed `u16x2`) -> **under 32 KB**

And it *removes* CPU work rather than adding it. Today `font_slot_glyph` runs per glyph per
re-tessellation:

```
font_slot_cp        -- dense index or binary search
res_atlas_origin    -- tenant page origin lookup
                    -- 4 multiplies + 2 adds against inv_w/inv_h
```

All of it is a pure function of (font slot, codepoint, atlas generation). It belongs at
table-build time. Tess then emits one id from the search it already performs.

The table is rebuilt only on **font load/unload or atlas repack** -- both already tracked
generation events -- so uploads are rare.

### Id assignment

```
id = slot_id * GLYPH_TABLE_STRIDE + gi
     gi = (cp - 32)                  for ASCII
     gi = ORB_FONT_CP_COUNT + ext_i  for extended
```

A **fixed** per-slot stride of 512, not a packed base. 16 slots x 512 = 8192 entries = exactly
the 13-bit budget, at 64 KB.

Packing the bases densely would save ~48 KB and cost correctness: loading or releasing a font
would shift every later slot's base, invalidating ids already baked into **retained** window
geometry. A fixed stride means an id depends only on (registry slot, glyph index) -- never on
atlas placement, never on what other fonts are resident -- so ids are stable across both font
load/unload and atlas repack. That stability is the whole point (see below). A slot whose
`95 + ext_count` exceeds the stride must be caught and reported, not silently truncated.

### This fixes a live bug

`cache_diff_windows` folds `res_atlas_generation()` into a window's hash for `GUI_CMD_SPRITE` and
`GUI_CMD_DASHED_LINE` only. Text is **not** folded -- but `font_slot_glyph` resolves glyph UVs at
tess time through `res_atlas_origin`, so text carries exactly the same dependency.

Nothing invalidates windows globally on a generation bump. So when `res_claim` repacks the
coverage atlas (adding an icon is enough), every font tenant's page origin moves and cached text
windows keep UVs pointing at whatever now occupies the old rectangle. It is masked in practice
because fonts and icons are usually claimed at boot before much geometry is retained, but the
hole is real.

Under the id scheme the table rewrites in place on a repack and the **ids do not move**, so
cached text geometry stays correct with no invalidation at all. That is the structural fix, and
it is why ids must not depend on atlas placement.

Note that the comment at `gui_build_cache.c:562` already describes this design as if it were
built ("text addresses glyphs by stable table id (the glyph table rewrites in place on a
repack)"). That text is **stale** -- left behind when the earlier attempt was reverted, and
currently describing behaviour the code does not have. Stage 1 makes it true again.

### Two atlases

`font_slot_tex` picks the R8 coverage atlas (`res_atlas_*`) or the SDF atlas (`res_sdf_*`) from
`slot->sdf_range`, and returns a tex index carrying `GUI_TEX_SDF`. The batch key already
separates them, so **one** table serves both: each entry's UVs are normalised against whichever
atlas that slot lives in, and the batch's tex index selects the texture.

### Glyphs with no tenant

`atlas_tenant == 0` means the font's pixels are not uploaded yet; today `font_slot_glyph` returns
a zero-size rect so nothing draws while layout keeps its shape. Under the id scheme the
tessellator must emit **no quad at all** for those, rather than an id pointing at an unbuilt row.

---

## 4. Staging

Each stage is independently shippable and independently revertable.

**Stage 1 -- glyph UV table. BUILT.**

- `draw/gui_glyph_table.c` -- the table, its rebuild trigger, and `font_glyph_placed` (ID +
  placement from one record lookup, so the hot loop does not search `ext[]` twice).
- `gui_glyph_uv_t` + `GUI_GLYPH_SLOT_STRIDE` / `GUI_GLYPH_TABLE_MAX` + `GUI_QUAD_F_GLYPH`
  (flags bit 2) in `gui.h`.
- Fourth bindless slot `glyph_buf` -- **not regioned**. The three per-frame tables are rewritten
  every frame and so need a copy per (frame-in-flight, viewport); this one is written only when
  its generation moves, so a rebuild REPLACES the buffer and hands the old handle and bindless
  slot to rhi's deferred destroy (`vk_garbage_push` / `vk_retire_safe_at`), which already holds
  both until no frame in flight can read them. Nothing is overwritten in place, so there is no
  hazard to buy copies against. One table, no `glyph_base`; push block stays 112 B.
- `glyph_table_sync()` at the head of `cache_build_frame`; `glyph_table_mark_dirty()` on font
  upload, upload failure, and `font_slot_release`.
- `tess_rect_glyph` / `tess_glyph_xf` emit by ID; `gui_quad.vs.hlsl` resolves through
  `glyph_uv()`.

Record stayed 3 rows -- the uv1 lane became inert for glyphs, which is what stage 2 spent.

*Not covered:* a glyph straddling its run's window edge carries a narrowed UV span with no table
entry, so it stays a baked-rect quad and pays a second lookup. At most the two end glyphs of a
cut run. It is also the one text case a repack can still leave stale (noted at
`gui_build_cache.c`).

**Stage 2 -- fx record + drop row 2. BUILT.** 48 -> 32 B, uniform 2-row stride, `vid/6`
untouched.

- `gui_fx_t` (16 B: xform, phase, col_border, reserved) holds the three per-instance lanes; the
  quad names one by slot-local ROW index, and 0 means "none" -- identity turn, zero phase, no
  border, which is what a glyph wants.
- Row 1's fourth lane is now the packed `idx` word: rule (0-1), `GUI_QUAD_F_GLYPH` (2), clip
  (3-6), style (7-17), fx row (18-31). Exactly 32 bits with no slack, and every field sits at an
  existing structural ceiling rather than a new budget -- `gui_quad_idx` packs, `gui_quad_style` /
  `_clip` / `_fx` read.
- **Clip became SLOT-LOCAL** (4 bits) instead of region-absolute. The window's slab origin now
  goes out with `pc.clip_base` on the same per-slot tail push that already carried `prim_base`.
  The fragment still computes `pc.clip_base + clip` and did not change.
- `tess_fx_local` resolves the ambient turn/phase/border, one-deep memo. Style is resolved FIRST,
  which is what guarantees an fx page never lands at the slot's record 0 -- the index spent on
  "none".
- `gui_phase_pack` returns a bare unorm16 now; `GUI_QUAD_PHASE_SHIFT`/`_MASK` are gone.

**Stage 3 -- tag union + implicit style for glyphs. BUILT.** Two tags, not four; the index word
is a union, and a glyph's 11 style bits were spent on carrying its atlas ID instead.

- `SHAPED` (0): clip(4) | rule(2) | style(11) | fx(13). `GLYPH` (1): clip(4) | sdf(1) |
  glyph_id(13) | fx(12). Both exactly full. Clip sits at the bottom of BOTH, so it decodes without
  reading the tag. Tags 2 and 3 are unspent.
- **A `GLYPH` names no style record at all.** The fragment sets field 0, ops 0 and takes the
  texture from `pc.tex_cov` / `pc.tex_sdf`, picked by the tag's SDF bit. This is exact rather than
  approximate: text emitters never write `cur_prim`, so the record a glyph used to name held zeros
  and its `tex` -- the implicit path reproduces it byte for byte.
- Consequences: the majority of the frame's quads stop fetching a 128 B record in the fragment,
  the vertex stage's feather fetch is now conditional on the rule (so glyphs skip it too), text
  stops creating style records at all, and a re-registered atlas can no longer leave cached text
  naming a stale bindless slot.
- `prim_row()` answers zero under the implicit tag rather than reading, which is what makes the
  ops cascade safe to walk with no record behind it.
- The uv0/uv1 lanes are now INERT for every whole glyph -- what stage 4 spends.
- Fallback: a glyph whose command carries an op or a field (an SDF outline), or whose fx row is
  past 4095, bakes the table's rect and rejoins `SHAPED`. It gives up repack stability, the same
  trade a straddling glyph already makes.
- `tess_fx_local` now leaves slot record 0 alone explicitly. A rotated glyph resolves no style, so
  "the style claimed record 0 first" stopped being a guarantee.

**Stage 4 -- fixed-point coordinates + the uv lanes retired. BUILT.** 32 -> 16 B, one row, one
load. The two were staged apart originally and cannot be: 8 B of placement plus 8 B of uv plus
colour and index is 24 B, which is not a whole number of rows. The uv lanes had to go first for
the record to collapse at all, so stage 5 landed inside stage 4.

- Placement is **quarter-pixel fixed point in SURFACE space**: `i16 cx, cy` (+-8192 px) and
  `u16 hw, hh` (0..16383.75 px), `gui_quad_pos_pack` / `gui_quad_ext_pack`. A snapped fill and a
  whole glyph are exact on that grid -- integer origin and integer size make both the centre and
  the half-extent multiples of 1/2 -- and the shapes that deliberately do not snap (a disc, a
  rotated box, a polyline segment) keep a quarter pixel, finer than the AA band they wear.
- **Window-relative was dropped.** It buys nothing here: the mvp is already a per-surface pixel
  ortho, so coordinates are surface-local before any origin is subtracted, and a window origin is
  smaller than the scroll excursions that are the actual tail risk. It would have cost a slot
  field, a push lane, and a volatile-patch interaction for that nothing.
- Out-of-range placements **clamp**. A coordinate past +-8192 px is scrolled-out content far
  outside every clip rect, so the clamped quad is discarded exactly as the true one would be;
  wrapping would drag it back into view.
- BBOX is the one rule the vertex stage grants no pad, so `tess_quad_push` adds a quarter pixel to
  its extents before packing. Every other rule either grows by the SDF pad or defines the rect its
  texture is mapped against, which must not move.
- **The uv rect moved into the fx record**, which grew to 2 rows: row A is the instance lanes
  (turn, phase, border colour), row B is the uv pair. Not the style record, which the stage
  heading proposed: a uv rect is per-INSTANCE, so on the style side every icon in a toolbar mints
  its own 128-byte record, while in the fx record it costs 32 B and shares a page with three
  neighbours. The vertex stage reads row B only when the quad is not a glyph.
- `tess_fx_arc` stopped writing a white-texel uv. It has set `GUI_OP_SELF` all along, so the
  fragment never read it -- the lanes were already dead.
- Cost, stated plainly: an SDF-**outlined** glyph run now takes one fx record per character, since
  the outline forces the SHAPED fallback and each character's baked rect differs. A heading is
  fine; a log view in outlined text is not. That is what tag 2 (`GLYPH_STYLED`) is still unspent
  for.

### Where fx records live -- DECIDED, and it was neither option

Both candidates were rejected. A **fifth arena** drags in a per-slot base, relocation on repack,
`cache_slot_reuse` copying, a volatile third axis, upload span union and a bindless slot -- the
bookkeeping tax that sank the expansion design; the prim axis alone is 114 references across 13
files, and a parallel one would roughly double that. The **quad arena tail** avoids the base but
couples fx storage to growth headroom and to a "must sit past the last gpu_cmd's range" invariant
that the volatile patch path can violate silently.

What shipped: fx records live **in the STYLE arena**, eight to a `gui_prim_t` slot (an "fx page"),
addressed by slot-local ROW index. The record arena's per-slot base, relocation, volatile
reservation and upload ranges already carry anything stored there, so the feature cost ZERO new
bookkeeping. A row index is exactly as repack-stable as the style index beside it, and a volatile
patch reproduces its own pages because it already fakes `slot_prim_base`.

The one interaction: a page holds fx bytes, so the style dedup memo must never compare against it.
Allocating a page raises `prim_dedup_floor` past itself -- the same move a volatile boundary
makes -- and `tess_fx_page_reset()` drops the open page wherever that floor rises.

---

## 5. Open risks

**Wave divergence.** Today every quad does the same three loads: uniform, branchless, coalesced.
The layered design makes the fetch count data-dependent, so a wave straddling a glyph run and a
decorated panel pays both paths. A 7-glyph run is 42 vertices -- more than a 32-wide wave -- so
runs should fill waves coherently, but this is to be measured, not assumed. It argues for the
extension path being a single conditional fetch rather than a chain.

**Outlines and drop shadows on an R8 atlas.** A one-record outline is a threshold on a distance
field, effectively free on the SDF fork but not expressible on a coverage atlas. Bitmap fonts
fall back to offset copies -- 4 or 8 extra plain `GLYPH` records, fine for a heading and wrong for
a log view. Decide whether `GLYPH_STYLED` is SDF-only **before** spending the tag.

**Naming.** The slot and stat fields are still named `vert_*` (`vert_base/count/alloc`,
`vert_hwm`, `band0_vert_end`, `tess_verts`) and every one counts quads. That naming is what let a
vertex-era `SLOT_VERT_PAD` of 64 sit unnoticed until it inflated a diagnostic by 3x. A
`vert_* -> quad_*` sweep is mechanical and internal to the render pipeline; worth doing as its own
pass before the record layout churns.

---

## 6. What this bought

At ~78% glyphs, on three axes at once. All four stages are in:

| axis | before | now |
|------|--------|-----|
| tess CPU per glyph | write 48 B, bake 4 UVs | write 16 B, emit 1 id |
| memory per glyph | 48 B | 16 B |
| vertex fetch per glyph | 3 row loads | 1 |
| fragment fetch per glyph | one 128 B style record | none |

Three times the geometry inside the same `GUI_MAX_QUADS` 8192, the quad region halved to 128 KB,
and the arena ceiling has stopped being a live constraint.
