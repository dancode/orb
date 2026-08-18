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
  0       8     pos.x, pos.y, size.w, size.h   -- i16 x4, 1/4 px, WINDOW-RELATIVE
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

### Why coordinates are window-relative i16

Clipping is a **fragment**-stage operation (the bindless clip slab is what keeps the batch key at
viewport only), so scrolled-out geometry is still dispatched carrying its real coordinates. A
long scrolled region emits quads at large negative Y. Unsigned `u16` cannot encode that.

Window-relative `i16` at 1/4 px gives +/-8191 px about the slot origin: enough for any window,
keeps sub-pixel precision for arcs and plot geometry, and makes slot relocation cheaper as a side
effect. `rows_clip` already culls far-offscreen rows, but the encoding must not depend on that.

### Why UV can move into the style record

Putting uv/tex on the deduped medium table is only safe **because glyphs get their own path**.
Per-glyph UVs in a deduped record would explode the prim count -- the same failure that pushed
`col_b` off `gui_prim_t` and onto the quad. The two halves of this design depend on each other.

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

**Stage 3 -- tag union.** Introduce the four tags and the implicit style for `GLYPH`/`SOLID`.
Collapses the index word to 32 bits. NOTE: stage 2 already spends all 32 bits, so this stage is
now about buying bits BACK (an implicit style for glyphs frees 11), not about collapsing 40 into
32.

**Stage 4 -- fixed-point coordinates.** pos/size to window-relative i16 at 1/4 px. 32 -> 16 B,
one row, one load.

**Stage 5 -- uv/tex into the style record.** Retires the UV lanes for the non-glyph textured
cases (icons, nine-slice, sprites).

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

## 6. What this buys

At ~78% glyphs, on three axes at once:

| axis | today | after stage 4 |
|------|-------|---------------|
| tess CPU per glyph | write 48 B, bake 4 UVs | write 16 B, emit 1 id |
| memory per glyph | 48 B | 16 B |
| vertex fetch per glyph | 3 row loads | 1 |

Roughly 3x the geometry inside the same `GUI_MAX_QUADS` 8192, and the arena ceiling stops being
a live constraint.
