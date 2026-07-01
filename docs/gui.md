# GUI ("The Orb GUI")

`source/runtime_service/gui/` — an in-house, immediate-mode GUI library. No Dear ImGui, no
GLFW/SDL: this is a from-scratch IMGUI with its own retained-cache render pipeline, docking,
tables, and text editing, built directly on Win32 + Vulkan.

This document has two goals, in order of importance:

1. **Catalog the jank.** Every layer of this library has real, load-bearing design
   inconsistencies -- inconsistent naming, hidden global state, unenforced ordering
   invariants, and duplicated logic that should be shared. These are called out inline as
   **JANK** blocks, and the worst offenders are summarized in [The Jank Digest](#the-jank-digest)
   below. The goal is a punch list for future cleanup, not a defense of the current design.
2. **Explain how it actually works**, bottom-up, for someone approaching this codebase cold.

Two unity translation units make up the library (`gui_backend.h:7-29`):

- **`gui.c`** -- the UI/core unit: context, layout, widgets, windows, popups, docking, nav,
  input. Owns `s_build` / `s_io` / `s_interaction` / `g_ctx`.
- **`gui_backend.c`** -- the render backend unit: fonts, CPU draw list, path stroking,
  tessellation, retained-geometry cache, GPU flush, debug overlay. Owns `s_draw` / `s_tess` /
  `s_font` / `s_render`.

Everything below is organized bottom-up, matching the actual dependency order: render backend
first, then core context/state, then layout/nav, then widgets, then windows/popups/docking/tables,
then the public API surface a host application actually calls.

---

## The Jank Digest

If you read nothing else, read this. These are the issues that show up **repeatedly**, across
layers, and are the ones most likely to bite a real feature down the line.

1. **The "subject-first naming" convention is real but undocumented in code, and is violated in
   several places.** Push/pop/is_/get_ are a deliberate verb-first carve-out (per project
   history) but nothing in the source says so -- a reader has no way to distinguish "documented
   exception" from "inconsistency." Docking (`gui_dock_split`, `gui_dockspace_over_viewport`)
   and several widget helpers (`gui_button_width` sitting mid-file among draw calls, `tree_pop`
   instead of `tree_end`) break the convention outright with no comment acknowledging it.

2. **There is no single "push/pop stack" abstraction, so it's been reinvented five-plus times**,
   each with a different overflow policy and a different depth constant: the id-scope stack (32,
   silent alias), the item-flag stack (16, silent alias), the style color/var stacks (32, debug
   assert + alias), the layout-frame stack (8, silent alias, duplicated cap-and-slot boilerplate
   copy-pasted across three files), and the listbox stack (4, assert on overflow but *silent
   no-op* on underflow). A contributor adding a sixth stack has no canonical pattern to copy.

3. **Global "only one thing is active" singletons are relied on independently, in at least six
   places, with no shared invariant or cross-reference**: `s_num_scratch` (numeric input),
   `s_drag_anchor_v`/`s_drag_anchor_f` (drag fields), `s_undo` (text edit), `font_active_id`
   (single active font slot), `cursor_flush`'s function-local dedup statics, and the ambient
   `cur_win`/`cur_z`/`cur_vp`/`cur_font` draw-list tags. All of them assume "exactly one focused
   widget / one build thread / one active font at a time" -- true today, but the assumption is
   never centralized, so multi-context work (which this codebase is actively moving toward --
   see the multi-context migration in project history) can break any of these silently and
   independently.

4. **Begin/End pairs do not share one calling convention, and the difference is invisible at the
   call site.** `window_begin`/`window_end` must **always** call `end`, even on a `false` return
   (documented explicitly). `popup_begin`/`popup_end`, `combo_begin`/`combo_end`, and
   `menu_begin`/`menu_end` are the **opposite** convention: only call `end` if `begin` returned
   true. `listbox_begin`/`listbox_end` currently always returns true, so it *looks* like the
   window convention but isn't guaranteed to stay that way. `table_begin`/`table_end` happens to
   be safe either way, by accident, not by contract. A developer who internalizes one rule and
   applies it to a sibling API will silently corrupt open-stack counters.

5. **Ordering invariants are almost universally enforced by comment only, not by assertion**:
   `interaction_frame_reset()` must run exactly once per app frame, never per-context;
   `s_retained.frame` must tick only for contexts actually rebuilt this frame;
   `gui_render_frame_reset()` must precede every surface's `gui_render_flush()`; a live font
   reload must go through the deferred queue, not `ttf_load_file` directly; `table_setup_column`
   must precede the first `table_next_row`. Every one of these, violated, produces a silently
   wrong frame -- stale hover state, reclaimed widget state, a frozen render, wrong column count
   -- with no crash and no log.

6. **The retained-cache / idle-skip system (a real strength of this codebase) has at least two
   known-fragile seams**: `s_tess.force_new_cmd` is a one-shot bool that must be set immediately
   before `cache_tess_window` or two adjacent windows' geometry can merge into one GPU command
   (this has already caused one real, fixed bug -- see project history's "tess slot boundary
   bug"), and the retained-geometry ping-pong table's `set_stable` fast path assumes positional
   alignment between this frame's and last frame's sorted-by-window arrays with no type-level
   guarantee, only a defensive runtime check.

7. **Table's headline claim ("no per-cell clip -- widgets self-fit") is not entirely true.**
   Text does get a per-cell clip via `draw_set_text_clip_x` -- just not a scissor-rect clip. A
   non-text widget (an image, a custom-drawn cell) has no such protection and can bleed into
   neighboring columns; the header comment doesn't scope the claim this precisely.

8. **Silent truncation/reclamation instead of assertion is the default failure mode almost
   everywhere a fixed-size resource runs out**: the widget-state hash table clobbers a live
   entry's slot when full (resets someone's scroll position with zero diagnostic), a window's
   cached GPU command list silently truncates past 16 commands with no warning (contrast: the
   vertex/index overflow *does* warn once), the font/icon registries return a shared "not found"
   sentinel indistinguishable from "ran out of slots," and columns beyond `GUI_LAYOUT_COLS` (8)
   are silently dropped. None of these are used in a debug build any differently than release.

Everything past this point is organized by layer with full detail and file:line citations.

---

## Layer 0 -- Render Backend (`backend/`, `gui_symbol.c`, `gui_backend.c/.h`)

### How it works

The render backend is a three-phase pipeline, documented at the top of `gui_render_cache.c:5-18`
and `gui_render.c:5-25`:

```
EMIT   (gui_draw.c)          widgets push semantic shapes -> s_draw command list
BUILD  (gui_render_cache.c)  diff against last frame + tessellate -> s_tess geometry
SUBMIT (gui_render.c)        upload + issue indexed draw calls, once per live surface
```

**EMIT.** Widgets call `draw_push_rect_filled`, `draw_push_rect_outline`, `draw_push_triangle`,
`draw_push_circle_filled`, `draw_push_text[_n/_clip_n]`, and (from `gui_draw_path.c`)
`gui_draw_line`/`gui_draw_polyline`/`gui_draw_dashed_line`. Each push appends one semantic
`gui_cmd_t` to `s_draw.cmds[]` (`gui_draw.c:50`) -- not GPU geometry yet. Ambient state folded in
at push time: a clip stack (`gui_draw.c:217-245`), and alpha/rounding/text-clip multipliers
(`draw_set_alpha`, `draw_set_rounding`, `draw_set_text_clip_x`). Commands are partitioned into
**segments** tagged `(win, z, vp, font)` by `draw_seg_retag` (`gui_draw.c:267-303`) so the BUILD
phase can work in per-window chunks instead of rescanning the whole buffer. A per-command FNV-1a
hash is computed **at emit time**, while data is still cache-hot, and stashed for the retained
cache diff (`draw_hash_cmd`, `gui_draw.c:497-537`). Every push also culls against the active clip
before it's stored (`draw_cull_box`) -- a culled push returns silently with no signal to the
caller.

**BUILD**, once per frame (`cache_build_frame`, `gui_render_cache.c:408-617`):

1. `cache_diff_windows()` folds each window's segment hashes (plus z/vp/font/live-atlas-index)
   into one hash per window, sorts by window id, and diffs against last frame in a single linear
   scan.
2. Each window has one **geometry slot** in a ping-pong table (`s_slots_a`/`s_slots_b`, swapped
   every frame so current/prev never alias). If the window set is stable and this window's hash
   is unchanged, its geometry is left in place and its cached GPU commands are replayed -- no
   retessellation. Otherwise `cache_tess_window()` groups commands by clip index and calls
   `tess_dispatch()`.
3. `tess_dispatch()` (`gui_render_tess.c:668-763`) walks the ordered commands and dispatches each
   to a `tess_*` function. Rounded rects reuse a cached unit quarter-arc table instead of
   re-running trig per call. `tess_ensure_gpu_cmd` opens a new GPU draw-call batch whenever
   texture/clip/viewport changes -- this is the actual GPU draw-call granularity.
4. Window slots sort back-to-front by z into `s_dispatch[]`.

**SUBMIT** (`gui_render_flush`, called once per live surface): lazily kicks BUILD (only the first
surface pays for it), uploads this surface's vertex/index slice, opens a LOAD-op render pass, and
walks `s_dispatch[]` issuing one `cmd_draw_indexed` per matching GPU command with its own scissor
rect.

**Fonts.** Three deliberately distinct concepts (`gui_font.h:14-24`): a compile-time 1bpp *bit
font* (`font/*.c`, generated by `font_tool`), an *expanded bitmap font* (8bpp R8 monospace grid,
built from a bit font at init), and a *TTF font* (proportional, loaded at runtime from a baked
`.orb_font`). Every atlas -- regardless of source -- is finalized identically by
`font_finalize_atlas()`: a white-texel row plus 4 dash-stipple rows are appended above the glyph
region, so one atlas backs solid fills, dashed strokes, *and* text glyphs. The registry
(`s_fonts[16]`) supports a **deferred reload queue**: swapping a font already in use doesn't
rebuild the GPU atlas immediately -- it's queued and committed once per frame from
`font_flush_pending()` (called at `frame_begin`), specifically to avoid a device-lost error from
GPU churn racing an in-flight frame.

**Icons** are a separate, dynamically packed R8 atlas (`stb_rect_pack`), uploaded lazily like
fonts, drawn through the ordinary textured-quad path.

**Debug tooling** (`gui_debug.c`, compiled only under `GUI_DEBUG_OVERLAY`) is an entirely separate
draw list/GPU buffer, populated by `DBG_WIDGET`/`DBG_CLIP`/`DBG_WINDOW`/`DBG_RESIZE`/`DBG_LAYOUT`
macros and flushed last, always on top, visualizing hit rects, resize bands, and clip nesting.

### Known issues

**Naming.** Three different prefixes name the same "push a shape" concept at three pipeline
stages with no visual cue which layer you're touching: `draw_push_rect_filled` (EMIT),
`tess_rect_filled` (BUILD), `gui_draw_line` (public). Dashed lines alone have three names --
`gui_draw_dashed_line`, `draw_dashed_line`, `tess_dashed_line` -- confusing enough that the author
left a comment explaining which one is "the real one" (`gui_symbol.c:605-607`). `font_use` vs
`bmp_select` use different verbs for the same "make X active" action at two layers of the same
subsystem.

**Leaky ambient state.** The `cur_win`/`cur_z`/`cur_vp`/`cur_font` draw-list tags are mutated by
`draw_set_window`/`draw_set_sort_key`/`draw_set_viewport`/`draw_set_font` and silently tag every
*subsequent* emit call. There is no scoping construct, no balance check -- forgetting to reset
`draw_set_window(0)` after a window ends mis-tags every later background draw into that window's
retained slot with zero diagnostic. `draw_set_alpha`/`_rounding`/`_text_clip_x` are the same
footgun. `s_tess.force_new_cmd` (a one-shot flag that must be set immediately before
`cache_tess_window`, or adjacent windows' geometry can merge) has already caused one real bug
(see project history).

**Stale/misleading comment.** `draw_push_text*`'s doc comment says the string must remain valid
until `gui_render_flush` (`gui_draw.c:688`), but the string is actually copied into a pool at push
time (`gui_draw.c:714-722`) -- the comment is simply wrong, in the safe direction, but still
wrong.

**Magic sentinels with no named constant**: `tex_idx == 0` means "use solid white" across four
functions; `n == 0xFFFFFFFFu` means "whole NUL-terminated string"; `GUI_TEXT_NO_CLIP` is tested
asymmetrically (`clip_x1 >= GUI_TEXT_NO_CLIP`, but the "off" sentinel is stored as both
`+GUI_TEXT_NO_CLIP` and its negation depending on which side). `draw_push_rect_outline` and
`draw_push_triangle` both accept a `tex_idx` parameter and then silently ignore it
(`(void)tex_idx`) -- the signature promises texture support the implementation doesn't have.

**Duplicated logic.** Axis-aligned line stroking is implemented twice, once at EMIT time
(`stroke_axis_aligned_rect`) and once at BUILD time (`tess_axis_line`) -- a pixel-snap fix in one
will not automatically propagate to the other. Four of five `tess_*` "ensure a GPU command is
open" functions re-implement the same guard sequence inline instead of routing through
`tess_prim_begin`, which exists for exactly this purpose. `cache_diff_windows`' per-window lookup
is an O(n) linear scan inside an O(segments) loop -- worst-case O(n^2) -- sitting right next to a
different pass in the same file that was deliberately optimized from O(n^2) to O(n), suggesting
this one was simply missed.

**Silent truncation with no warning.** `WIN_SLOT_CMD_MAX = 16` caps cached GPU commands per
window; overflow is silently truncated with **no warning printed**, unlike the vertex/index
overflow path in the same file which does warn once. A font-reload double mechanism exists: the
deferred queue exists specifically to avoid a GPU stall mid-frame, but `font_flush_pending` still
calls `ttf_load_file`, which itself unconditionally stalls the GPU (`device_wait_idle`) when
swapping a live slot -- so the "safe" deferred path still stalls, just at a different point in the
frame, and nothing documents that the deferral doesn't buy stall-freedom, only frame-boundary
safety.

---

## Layer 1 -- Core Context / State (`gui_ctx*.c`, `gui_input.c`, `gui_anim.c`, `gui_stacks.c`, `gui_style.c`)

### How it works

A `gui_context_t` (`gui_internal.h:573-600`) is the unit of "bind and use" retained state: keyed
widget-state pool, nav cursor, popup open-set, window pool, viewports, dock tree. Up to 8 exist in
a fixed pool; slot 0 is the always-present default, allocated once at `gui_init` and never freed.
Every context is a **single malloc** -- header plus five trailing arrays laid out in one block.
`g_ctx` is the only indirection: a family of macros (`s_retained`, `s_nav`, `s_windows`, etc.)
textually rewrite reads/writes through `g_ctx->...`. Switching context is one pointer store.
Ambient interaction state, frame-build scratch, IO, and the id/item-flag/layout/style stacks are
explicitly **not** per-context -- they're shared globals, on the premise there is only one
mouse/keyboard/build-thread regardless of which context is bound.

**IDs.** `id_hash()` is FNV-1a seeded per-context (so identical labels in different contexts don't
collide); `id_combine()` is the boost-style hash-combine used to namespace every sub-id. Neither
ever returns 0 (`GUI_ID_NONE` is a hard sentinel). An id-scope stack (depth 32) backs
`push_id`/`pop_id`. `gui_state_get(id, size)` is the mechanism turning an id into a stable pointer
into an open-addressing hash table: linear probing on collision, and a slot untouched for more
than one frame is a reclaimable tombstone the next colliding insert can steal in place -- no free
list, no sweep. `gui_state_peek()` is the non-stamping read used by animation to check "does this
id have history" without extending its lifetime.

**Input.** `gui_event(ev)` is fed per-OS-event by the host and unpacks text/wheel/clipboard/debug
hotkeys into pending accumulators; `input_update(w, h, dt)` is called once per app frame, *polls*
`app()`'s mouse/key state directly (not event-driven for continuous state), merges in the pending
accumulators, and computes double-click detection with a self-rolled timer. The resulting
`s_io_dirty` flag (OR of moved/edge/wheel/text/paste) feeds `gui_frame.c`'s idle-skip gate --
`s_frame_dirty = io_dirty() || wants_redraw || render_any_changed` -- the mechanism that lets the
host skip rebuilding+re-emitting+re-rendering an unchanged frame entirely.
`interaction_frame_reset()` is the per-app-frame (never per-context) counterpart that snapshots
`active_id_prev`/`focused_id_at_frame_start` for edge detection (`is_item_activated` etc.) and
resolves last frame's deferred hover-window vote.

**Stacks.** Four structurally identical "push/pop + one-shot next" stacks exist side by side: id
scope, item flags, style colors, style vars -- each with its own storage, its own depth, and its
own overflow policy. `gui_stacks.c` is a thin public-API veneer with no logic of its own; its own
header comment says it was pulled out of `gui_layout.c` "where it had accreted only because it was
the public API file." Item flags and style vars/colors share one resolve seam
(`item_flags_resolve()`) so both are latched and reset in lockstep on every widget emit, even
though they live in different files.

**Animation** (`gui_anim.c`) is "peek-then-stamp": check history non-destructively, exponentially
decay toward target, only write back (stamp) the slot if the value hasn't settled -- a settled
value returns the target directly without writing, letting the slot cool and get reclaimed
naturally. Any mid-transition animation sets `wants_redraw = true`, which is animation's hook into
the idle-skip system -- a fading hover color is what keeps the app rendering during an otherwise
input-idle frame.

**Style** (`gui_style.c`) resolves in three layers per slot: a live base (several vars are
literally enums smuggled in as floats, e.g. `GUI_VAR_CHECK_STYLE`), a push/pop stack (pop takes a
*count*, ImGui-style), and a one-shot per-item "next" override promoted at the same seam as item
flags. `style_new_frame()` re-seeds everything from base every frame -- a safety net against
exactly the unbalanced-push bug class the stacks' silent-overflow design invites.

### Known issues

**Silent ordering hazards.** `interaction_frame_reset()` must run exactly once per app frame,
never per-context -- stated only in a comment, unenforced by any assert; a second context
accidentally calling it clobbers `hover_win`/`active_id` with no crash, just wrong hover state.
`s_retained.frame` must tick only for a context actually rebuilt this app frame, or its live
entries read as cold and get reclaimed -- again, comment-only. `gui_state_peek`/`gui_state_get`'s
2-frame-cold reclamation window means any future caller reading state across a longer gap (e.g. a
conditionally-rendered-every-third-frame widget) silently loses its history with no diagnostic.

**Inconsistent stack implementations.** Four near-identical push/pop stacks (`id_*`, `item_flag_*`,
`style_push/pop_color`, `style_push/pop_var`) each have a bespoke internal shape and a different
depth cap (16 for item flags vs 32 for id/style) with no shared rationale, and different overflow
behavior (only the style stacks assert in debug; id and item-flag stacks silently alias the top
slot forever). A contributor adding a fifth has no canonical pattern to copy, and push always
"succeeds" from the caller's point of view even when it silently didn't.

**Hidden state / action at a distance.** `cursor_flush()` hides its dedup state in **function-local
statics two scopes deep** -- invisible from `gui_context_t`, and would silently misbehave if the
function is ever called from more than one place. `window_nominate_hover`/`hover_win` resolution is
deferred by exactly one frame via a hidden vote-then-resolve mechanism; a window that skips
`window_begin` for one frame silently drops out of hover contention. `s_popup_begin_count` is a
bare module global living outside `s_build`/`s_interaction`/`gui_context_t` despite tracking
per-frame popup nesting -- there's no consistent rule for what scratch state lives where.
`s_debug_enabled` gates a side effect (F1-F5 overlay toggles) directly inside the lowest-level
input-event handler, entirely bypassing the widget/build system -- a maintainer reading `gui.c`
for "how does F1 work" will never find it there.

**Magic namespacing with no registry.** `ANIM_TAG_BG = 0xA501u` is a hand-picked salt to keep an
animation slot distinct from a widget's other per-id state -- there's no enum or naming convention
for these tags, so two features independently picking the same hex constant would silently read
and write each other's animation slots.

**Duplicated hand-maintained parallel structs.** `gui_popup_t`/`gui_overlay_save_t` duplicate the
entire `s_build` window-context field list by hand, field-for-field, as a separate struct with no
static assertion tying them together -- adding a field to one and forgetting the other means
popups silently fail to restore that piece of parent state on `popup_end`.

**Asymmetric lifecycle.** Slot 0's context is allocated once and never freed while secondary
contexts get a full destroy-with-teardown -- a reasonable but undocumented asymmetry. Context pool
count is a high-water mark that never shrinks on destroy, so repeated create/destroy cycles at
high slot indices leave iteration loops scanning past dead slots forever (harmless today, latent
if the pool ever grows).

**Silent state destruction under load.** The keyed-state hash table, when full, clobbers a live
entry's home bucket rather than growing, erroring, or asserting -- "your window's scroll position
resets for no reason" is a real, silent failure mode behind a widget count that's rarely tested.

---

## Layer 2 -- Layout, Navigation, Resize (`gui_layout*.c`, `gui_nav.c`, `gui_resize.c`)

### How it works

Every layout verb operates on the current `layout_frame_t` (`gui_internal.h:~250-349`), one of up
to 8 stacked frames -- one per open region (a window body, a `child_begin` box, a transient
sub-layout). A frame's `mode` is `NONE | STACK | COLUMNS | GRID | PACK`.

**The overloaded-unit sizing language** governs every size parameter across columns, grid tracks,
field-split tracks, `gui_split`, and `gui_carve`, resolved by one shared function
(`layout_resolve_tracks`, `gui_layout_core.c:65-108`): `> 1` is fixed pixels, `== 1` is an equal
fill-share of leftover space, `(0, 1)` is a fraction of the gap-adjusted extent, `== 0` is
"natural" (mostly meaningless outside pack mode), and negative values are reserved sentinels
(`GUI_END`, `GUI_CUT_X`, `GUI_CUT_Y`). One resolver, five call sites -- genuinely good reuse.

**Primitives**: Stack (`gui_stack`/`gui_row`, a single flex column that scrolls), Columns
(`gui_cols`/`gui_row_cols` and friends, N flow tracks), Grid (`gui_grid`, both axes resolved
up-front against a bounded band, nothing scrolls, overflow reuses the last cell), Pack
(`gui_pack`/`gui_bar`/`gui_strip`, items placed at natural size along one axis, a "print run"),
Field split (`gui_field_split`/`gui_form`, label+control tracks via the same resolver), and pure
rect-math primitives with no emitted state (`gui_split`, `gui_carve`, `gui_anchor`) meant to pair
with `push_layout_overlay`.

**Regions** (`gui_layout_region.c`) implement the shared scrollable-region mechanism used by both
window bodies and `child_begin` boxes: persistent scroll/content-size state lives either inline in
`gui_window_t` or in a keyed `gui_region_t` pool entry, and the region engine itself is agnostic,
taking four `f32*` in/out params. Scrollbar-gutter visibility is decided from **last frame's**
measured content (a deliberate one-frame lag, since reserving one bar can push the other axis over
threshold). Windows pass `own_clip = false` (single outer clip, chrome overpaints on top);
children pass `true` (their own nested clip).

**Children** (`gui_layout_child.c`) carve a box from the parent pen, draw a frame, and hand off to
the region engine. `CHILD_RESIZE_X/_Y` adds a drag grip on the right/bottom edge only, persisting
size in a keyed `gui_region_t.user_w/user_h`. `push_layout`/`pop_layout` open a transient sub-layout
over the parent's next template cell (advancing the parent, like a widget would);
`push_layout_overlay` does the same over an explicit rect (does *not* advance the parent) -- both
share one `pop_layout`.

**Resize** (`gui_resize.c`) is factored as mechanism vs. policy: hit-test, grab (records
cursor-to-edge offsets), apply (maps cursor to rect edges with an explicit edge bitmask, no
clamping). Three consumers share it -- window chrome, `child_begin`, and (per comment) a future
dock splitter -- each layering its own min/max clamp on top.

**Navigation** (`gui_nav.c`) is entirely rect-based and layout-mode-agnostic -- it never reads
`layout_frame_t` internals. The seam is `nav_item_register`, called from `widget_behavior` for
every item in the current nav window: it records emission order for Tab, scores directional
candidates via pure center-to-center rect geometry (`nav_score_dir`), and lights the focus ring /
synthesizes Enter/Space clicks. Because scoring is pure rect math against whatever the layout
system handed out, any layout mode "just works" for nav with zero mode-specific code.

### Known issues

**Confusing overloaded sizing semantics -- the same `0` means three different things.** In
`layout_resolve_tracks`, `0` means "natural" (silently becomes 0px unless in pack mode); in
`child_begin`, `h <= 0` means a much richer "auto-size to content"; in `gui_field_label_left`,
`width <= 0` turns the field split off entirely. A user porting a `0` between a column-track and a
child height gets silently different behavior. `gui_pack_size(0.0f)` (explicit natural) and the
*unset* default `-1.0f` (also "natural," but falls back to fill if the widget has no natural size)
resolve differently for a widget with no natural width -- a genuinely fragile trap where "being
explicit" and "not calling the function at all" produce different results.

**Emit-before-header is a release-mode silent degrade, not a hard failure.**
`widget_next_rect_w` asserts in debug if no layout mode was declared, but falls back to a default
stack mode in release -- exactly the "wrong call order produces silently wrong layout instead of a
compile/assert error" failure class.

**Hidden reset scope mismatches.** `gui_pad()` implicitly clears the *entire* template
(`layout_clear`), so a widget call immediately after `gui_pad()` re-triggers the emit-before-header
fallback above -- nothing in the name `gui_pad` suggests this. Three different "reset" verbs
(`gui_row(0)`, `gui_field_label_left(0)`, `gui_layout_default()`) each clear a different subset of
persistent modifiers (row template vs. field split vs. both) -- confusing enough that
`gui_layout_default`'s own comment has to spell out the difference. `gui_window_set_next_size_constraints`
is a one-shot global positional (not id-keyed, unlike almost everything else in this GUI) side
channel that silently misattributes to the wrong child if any other `child_begin` intervenes
before the intended one consumes it.

**`gui_indent`/`gui_unindent` only re-resolve flow modes**, and nothing stops a caller from using
them inside a grid or pack region -- `content_x`/`content_w` shift silently without re-resolving
already-resolved grid cell geometry, producing a persistent mismatch with no assert (contrast:
the CHILD_RESIZE-inside-a-grid-parent case *does* assert -- an inconsistent policy on which
flow-only features get a defensive guard).

**Copy-pasted stack-slot boilerplate**, three separate times, for "cap and alias the deepest
layout frame" (`sublayout_open`, `layout_push_region` twice) -- a fix to the overflow policy
requires editing multiple places and is easy to miss one. `split_pop_panel` and `gui_pop_layout`
independently duplicate the same three-line teardown sequence rather than sharing a
`sublayout_close()` counterpart to the shared `sublayout_open()`. `gui_split_begin/next/end`
reimplements the same "measure last frame, preallocate this frame" pattern already used by
`child_begin`'s auto-height and the region engine's gutter reservation -- three separate hand-rolled
instances, and notably **inconsistent** in a load-bearing way: a comment in the split code
explicitly explains why it measures to `prev_item` bottom rather than `cursor_y`, to avoid an
unbounded-growth bug that the region engine's parallel code does not guard against the same way.

**Nav fragility tied to layout internals.** Tab order is pure emission order, not visual order --
diverges silently from what's on screen whenever a caller uses `push_layout_overlay` (explicit
rects) to draw things out of call order. Directional scoring only ever compares against the single
previous winner's rect, so an auto-sizing child box that changes height frame-to-frame can cause
the nav cursor to jump unexpectedly for one frame. Grid overflow silently reuses the last cell,
which can leave several nav items with identical rects -- the scorer's strict less-than comparison
means the first emitted overflow item always wins ties, making later overflowed widgets
permanently unreachable by arrow keys despite being emitted.

**`i16` truncation.** `gui_region_t.user_w/user_h` (persisted resizeable-child dimensions) are
`i16` while every other width/height in the layout system is `f32` -- a child dragged past 32767px
silently wraps, with no clamp or assert at any of the cast sites.

---

## Layer 3 -- Widgets (`gui_widget*.c`, `gui_text_edit.c`)

### How it works

**One function is the entire interaction model**: `widget_behavior(id, rect, kind)`
(`gui_widget_core.c:384-489`). Every interactive widget -- from a plain button to the text-edit
engine's mouse-drag selection -- reduces to "compute a rect, call `widget_behavior`, branch on the
returned `widget_state_t`." Its sequence: latch "last item" unconditionally (so item-query APIs
always see the most recent emit, interactive or not) -> disabled/deaf early-outs -> hit-test
eligibility (must belong to the hovered window, no other widget owns `active_id`, not blocked by
resize/nav) -> press capture (claims `active_id`, and `focused_id` if `WIDGET_KIND_FOCUSABLE`) ->
state resolution (`hover`/`active`/`focused`/`clicked` as direct id comparisons) -> keyboard-nav
mirror (registers for Tab order, scores as a directional candidate, synthesizes clicks from
Enter/Space through the *same* `st.clicked` path) -> auto-repeat override if
`GUI_ITEM_BUTTON_REPEAT` is set.

Two color helpers built on the returned state cover almost every widget's visual: `widget_bg_color`
(active > hover|nav > idle, for pushbutton-style widgets) and `frame_bg_color` (same precedence
but with a caller-supplied idle base, for framed fields).

**Label grammar** (Dear ImGui style): `"Text##key"` hides everything after `##` from display but
not from the id hash unless a `###` marker re-roots it. Every labeled widget routes through
`widget_id`/`label_vis_len`/`draw_label`, so the grammar is centralized in one place.

**Concrete families**, all built from the same generic machinery:
- **Buttons/checkables** (`gui_widget.c`): `gui_button`, `gui_button_fill`, `gui_small_button` are
  three parallel implementations; `gui_checkbox`/`gui_radio_button` hand-roll their own
  label/control split fallback (a *third*, slightly different, "default layout" than the one
  `widget_split_label` implements). `gui_selectable` is `WIDGET_KIND_BUTTON` with two pieces of
  hidden cross-file coupling: it flags a combo-specific global when inside an open combo, and
  closes the enclosing popup on click unless told not to.
- **Combo/listbox** (`gui_widget_combo.c`): the box is `widget_split_label` + `widget_behavior`;
  the dropdown is a popup keyed by `id_combine(id, GUI_POPUP_SALT)`. Needs a 1-frame-delayed
  "was open" flag to distinguish "click that opens" from "click that would instantly reopen" after
  the popup layer's own top-of-frame close-on-click-outside logic runs.
- **Menus** (`gui_widget_menu.c`): same "thin coordination over popups" philosophy.
  `gui_menu_begin` branches its entire layout/anchor/open-policy on the *ambient* layout mode
  (bar-drop vs. side-opening submenu) rather than being parameterized explicitly.
  `gui_menu_bar_begin/end` manually save/restore clip rect and cursor position around a region
  push/pop, with a comment admitting this workaround exists because the generic hit-test path
  doesn't naturally support a region drawn outside its parent's clip.
- **Numeric fields** (`gui_widget_numeric.c`): `input_scalar` factors float/int into one generic
  path with an `is_int` bool -- the cleanest "core + variant" pattern in the widget layer.
- **Sliders/drag fields** (`gui_widget_slider.c`): `slider_render` is one shared visual for both
  float and int sliders (good factoring), but unlike numeric.c, the float/int *interaction* bodies
  (`gui_slider_float_step`/`gui_slider_int`, `drag_int`/`drag_float`) are fully duplicated rather
  than unified. The same file also contains a large, self-contained color-picker widget
  (`color_edit_n`/`gui_color_edit3/4`) that the file's own header comment never mentions.
- **Text edit** (`gui_text_edit.c`): the deepest, most special-cased widget -- explicit
  `memmove`/`memcpy` buffer surgery for insert/delete/paste/undo, a single global undo ring keyed
  by "currently focused id" (relying on the same "only one active widget" assumption used
  independently elsewhere), and hand-tuned off-by-one corrections between click hit-testing and
  word-boundary detection, bridged only by a comment.

### Known issues

**No canonical pattern for a new widget to copy.** Sliders duplicate float/int bodies; numeric
inputs unify them. Combo/menu use id-combined popups; the color picker in the *same file* as
sliders uses a third, string-keyed popup convention (`snprintf`'d ids, paying formatting cost every
frame). A newcomer copying the "wrong" neighbor inherits the wrong pattern with no signal either
way is deprecated.

**Three near-identical button implementations** (`gui_button`, `gui_button_fill`,
`gui_small_button`) differ in height source and, inconsistently, in whether they use an animated
background tint -- copying the wrong one silently drops hover animation with nothing suggesting
it was intentional.

**Verb-noun inconsistency inside the widget API itself.** `gui_button_width` is a pure size-query
function wedged mid-file among drawing functions, oddly named like an action, far from
`gui_text_size` (a genuinely parallel query) which lives elsewhere in the same file.
`gui_label_text` (two-string, layout-split display) and `gui_text_colored`/`gui_text_disabled`
(color/none + one string) look like siblings but have different arity and completely different
behavior -- naming gives no hint.

**Hardcoded/magic constants inconsistently promoted to style vars.** `combo_cap_items` hardcodes
4/8/20 visible rows with no named constants; `NAV_RING = 2.0f` is the one visual constant in
`gui_widget_core.c` **not** routed through `style_var`, unlike every sibling constant in the same
file; `color_edit_n` has an unexplained `44.0f` per-drag-field width literal not derived from any
font metric, so DPI/font-scale changes will silently misjudge the picker's minimum width.

**Hidden global "only one active widget" singletons, independently implemented four times**:
`s_num_scratch` (numeric), `s_drag_anchor_v`/`s_drag_anchor_f` (drag fields), `s_undo` (text edit),
plus the shared `s_click_x[0]` press-anchor array used directly by two drag-field functions with
no doc comment explaining where it's populated. None reference each other; each file re-derives
and re-justifies the same assumption independently.

**Cross-file hidden coupling.** `gui_selectable`'s signature gives no indication it has
combo-specific and popup-specific side effects baked in -- a new list-style widget built on
`gui_selectable` inherits this coupling unknowingly. `input_scalar` manually saves and restores
`last_item_id`/`rect`/`status` around its internal step-button calls so `is_item_*` queries report
the text box, not the last-drawn button -- a documented workaround. **`color_edit_n`, which faces
the identical problem** (internal preview square + drag fields + popup), **does not** do this
save/restore, so `is_item_*` after a color editor call reports whichever internal sub-widget ran
last. Same problem, two widgets, one fixed and one not.

**Duplicated N-way splitting arithmetic**, three times, with no shared helper: `input_float_n`,
`drag_float_n`, and `color_edit_n`'s per-component loop all divide a control rect into N equal
sub-boxes by hand. Sub-widget id-addressing offsets are similarly ad hoc and inconsistent
(`id_combine(id, i+1u)` in one place, `id_combine(id, 10u+i)` in another, in the *same file*, for
conceptually identical "Nth channel" addressing).

**Unenforced tree/stack pairing.** `gui_tree_node`/`gui_tree_pop` has no overflow or imbalance
guard at all -- an early `return`/`continue` inside a tree body that skips `tree_pop` permanently
misindents everything drawn afterward with zero signal. `gui_listbox_end` silently no-ops on
underflow while `gui_listbox_begin` asserts on overflow -- asymmetric handling of the two failure
directions of the same stack. `gui_menu_bar_begin/end`'s manual clip/cursor save-restore has the
same "unpaired call permanently corrupts the rest of the frame" exposure, again with no assert.

**Three different Begin/End calling conventions among visually-parallel widget pairs**: window
(`begin`/`end`, always call end), popup/combo/menu (`begin`/`end`, only call end if begin returned
true), listbox (currently always returns true, so it *looks* like the window convention but
nothing enforces that it stays that way). `tree_node`/`tree_pop` breaks the naming pattern outright
-- `pop` instead of `end`, the only `_pop`-named close call among a dozen `_end`-named ones for the
conceptually identical "close a conditionally-opened body."

---

## Layer 4 -- Windows, Popups, Docking, Tables, Frame Orchestration, Public API

(`gui_window.c`, `gui_widget_window.c`, `gui_popup.c`, `gui_dock.c`, `gui_table.c`,
`gui_frame.c`, `gui.c`, `gui_api.c`, `gui.h`, `gui_api.h`, `gui_host.h`)

### How it works

**Minimal frame loop**, per `gui_api.h:157-164`:

```
gui_frame_begin(dt)                  // once/frame: input poll + draw-list reset
  gui_ctx_begin(GUI_CTX_DEFAULT)     // bind a context; emit windows after this
    gui_window_begin("Tools", 0) -> widgets -> gui_window_end()
  gui_ctx_end()
gui_frame_end()                      // seals the build
gui_viewport_update()                // reconciles owned floater surfaces
gui_render(vp, cmd)                  // per live viewport, inside the host's render frame
gui_viewport_render_floaters()       // presents any gui-owned floater OS windows
```

`gui_frame_begin` computes `s_frame_dirty`: if input is idle, no animation is in flight, and
nothing structurally changed, the host is *permitted* to skip the entire `ctx_begin`/emit/`ctx_end`
cycle and just replay last frame's draw list via `render()`. Whether to actually skip is the
host's decision (`gui_frame_dirty()`); nothing stops a caller from skipping unconditionally and
serving stale UI.

**Windows.** Persistent state lives in a per-context pool keyed by `id_hash(title)`; first
appearance seeds geometry, then the registry owns position/size forever. Every kind funnels through
one `window_begin_ex`:
- **Regular floating panel** -- drag, edge/corner resize, autosize, collapse, optional menu bar.
- **Detached floater** -- torn off into its own OS window. "Detach" implies "native" automatically
  (any window on an owned viewport is treated as native) -- a derived property, not a flag the
  caller sets.
- **Native-borderless (`GUI_WIN_NATIVE`)** -- gui publishes caption-band/resize-border geometry;
  the OS drives move/resize/snap via `WM_NCHITTEST`. A `frame_only` variant draws chrome but is
  click-through in its body.
- **Docked** -- placed by a dock node instead of free-floating; title bar replaced by the node's
  tab strip; drag/resize/tear-off/collapse all bypassed.
- **Closeable (`GUI_WIN_CLOSEABLE`)** -- adds an X button; `window_begin` returns false and emits
  nothing while closed; a closed floater additionally tears down its OS window and re-spawns on
  reopen.

**Popups.** A thin layer entirely on top of the window path (`popup_begin_common_id` calls
`window_begin_ex` with a fixed no-move/no-resize/no-collapse/autosize flag set). The load-bearing
invariant: `popup_close_check()` runs at the very top of the frame, before any user emit code, so
the click that opens a popup this frame can never also be the click that closes it next frame.
Modals fence input by hijacking `hover_win` globally -- one write freezes every window behind the
modal, no per-widget code needed. Tooltips are a single non-stacking reserved id one z-band above
every popup, with no open-state tracking at all -- purely "was the previous item hovered."

**Docking.** A tree of `gui_dock_node_t` per viewport; LEAF nodes tab windows, INTERNAL nodes split
at a ratio resolved fresh every frame. Programmatic API: `gui_dockspace_over_viewport`,
`gui_dock_split`/`gui_dock_split_root` (leaf split vs. whole-tree edge split -- distinct
operations sharing one opaque handle type), `gui_dock_window`, `gui_dock_undock`. Mouse gestures:
a 5-way drag-to-dock chip overlay, plus outer edge chips once already split; undock-by-tab-drag
past a 12px threshold. Layout persistence is a small line-oriented ASCII format; node ids are not
serialized (freshly assigned on load), but tab window ids are, so a `"Title##key"` window restores
correctly.

**Tables.** A region whose content lays out in a column grid resolved once per `table_begin`.
Central design claim (stated three times in the header comment): no per-cell *scissor* clip --
content stays in its column because widgets self-fit, matching the layout engine's columns-mode
contract. One clip surrounds the whole table (header included); the header strip draws **last**,
as chrome, so it overpaints rows that scrolled up underneath it. Column-boundary resize is a
"pair-resize" that keeps every other column fixed. Sorting is push-based: a one-frame "dirty" flag
on header click, consumed by an insertion sort of an index array.

**Frame orchestration and public API.** `gui.c` gluess 27 constituent files into the UI/core unity
unit; `gui_frame.c` owns init/shutdown, the perf overlay, frame/context/viewport lifecycle, and
font/clip helpers. `gui_api.c` builds a roughly 280-entry vtable (`gui_api_t`) that the host
consumes either through the vtable (hot-reloadable modules) or through direct-call declarations in
`gui_host.h` (host/sandbox code, no indirection). `mod_desc_t` declares deps on `{"rhi", "app"}`.

### Known issues

**The naming convention is violated most visibly here.** `gui_dock_split`, `gui_dock_split_root`,
`gui_dock_window`, `gui_dock_undock`, and especially `gui_dockspace_over_viewport` read as verb
phrases, not the noun_verb pattern the rest of the surface (mostly) follows. `gui_dock_window`
(dock verb, window object) inverts subject/object relative to `gui_window_set_open` (window
subject, open verb) -- a caller guessing "how do I dock a window" by analogy to
`gui_window_set_open` will reach for a nonexistent `gui_window_dock`.

**Begin/End contract differs by family, and violating it corrupts state silently, not loudly.**
`window_end()` must *always* be called regardless of `window_begin`'s return -- documented
explicitly. Popup's depth-miss early return does **not** increment the open-count, so the opposite
rule applies (skip `popup_end` on a false `popup_begin`) -- and calling it anyway doesn't crash,
it's guarded to a silent no-op (`if (!s_popup_begin_count) return`), which *masks* the bug instead
of surfacing it. Table happens to be safe either way, by accident of its internal flag, not by any
stated contract.

**Ordering violations fail silently across the board.** Unbalanced `ctx_end`/`popup_end` calls are
ignored rather than asserted (only the *opposite* imbalance -- an unclosed `ctx_begin` -- is caught,
and only in debug builds). A late `table_setup_column` (after headers/rows have already resolved)
silently doesn't participate until next frame, with the ordering requirement stated only in a
comment. `gui_dock_load`'s own comment warns that calling it from inside a docked window's body
frees a node mid-render -- a use-after-free-adjacent hazard flagged only in prose, no runtime
guard.

**Hidden global/implicit state a first-time integrator will not expect.** `gui_window_set_drag`
(global drag mode) is a single ambient toggle affecting *every* window, despite being conceptually
a per-window property elsewhere in the IMGUI world. The "ambient viewport" a new window/popup
silently inherits depends on emit order via a static mutated as a side effect of
`window_begin`/`window_end` bracketing -- get the nesting wrong and a popup renders on the wrong
OS surface with no diagnostic. `gui_window_set_next_pos`/`_size` are one-shot side channels that
silently land on whichever window is opened *next* if no `window_begin` follows immediately.
`s_vp_request` (tear-off/merge-back/dock) is a single global slot for one gesture per frame --
concurrent gestures silently drop, first-wins.

**Overloaded flags whose meaning depends on how they were reached.** `GUI_WIN_NATIVE` means two
different things depending on origin: explicitly passed (a click-through frame-only shell) vs.
implicitly derived from living on an owned viewport (a real-content detached floater) -- both share
the same boolean but diverge sharply in body-fill and raise-on-press behavior. `gui_cond_t`
(`ONCE`/`ALWAYS`/`APPEARING`) silently reinterprets an unset `0` value as `ALWAYS` -- the most
aggressive option, not the safest default -- for anyone who didn't think about the condition
argument.

**Real duplication across the window/popup/dock trio.** Tear-off drag-threshold detection and
undock-by-tab-drag detection are two independently implemented "press, wait for N px, commit"
state machines with different named thresholds, doing the same job. Splitter dragging (docking)
and column-boundary dragging (tables) reimplement the same "grab a thin band, drag to redistribute,
clamp to a floor" pattern separately. Popup and dock-drag-overlay each maintain their own "top
z-band" constant, hand-coordinated only by comment, not a shared ordering enum.

**Table's headline claim has a real exception.** "No per-cell clip" is true for scissor rects but
not for text -- `draw_set_text_clip_x` is pushed per header label and per cell, which *is* a
per-cell clipping mechanism, just not the scissor kind the banner comment implies. A non-text
custom-drawn cell has no equivalent protection and can bleed into a neighbor. Column-resize and
header interaction both temporarily widen the effective clip to the whole table box via a manual
save/restore of `s_build.clip_rect` -- exactly the kind of ad hoc clip-juggling the "one clip only"
design was meant to avoid; an unbalanced save/restore here leaves every subsequent widget's
hit-test clip wrong for the rest of the frame with no assertion to catch it.

**Public API sizing/config is exposed but not validated.** `gui_ctx_config_t` exposes four raw pool
sizes with no resize-later path and only two baked presets; an out-of-range `state_slots` is
silently rounded to the next power of two with no feedback that the caller's requested value was
altered. There is no public way to enumerate a dock tree's leaves/tabs or query a docked window's
node rect -- a host wanting a "restore default layout" button beyond load/save has no vtable
surface for it and would need internal (unexposed) struct access.

---

## Cross-cutting takeaway

The strongest parts of this library are the places where one mechanism is deliberately shared
across very different features: the overloaded-unit track resolver (layout), the resize
mechanism/policy split (windows, children, and a planned dock splitter), the retained
geometry-cache diff (render), and the single `widget_behavior` interaction seam (every widget).

The jank is almost entirely in the layer *above* each of those good seams: the composition code
that wires label/control splitting, sub-id addressing for composite widgets, popup-toggle
bookkeeping, save/restore of ambient state around a sub-call, and stack-overflow policy gets
reinvented per file rather than centralized in the file that already claims to be "the foundation
the rest of the layer is built on." A cleanup pass should prioritize, in order:

1. **One shared push/pop stack primitive**, replacing the four-plus independent reimplementations,
   with one overflow policy (assert in debug, and pick *one* alias-or-clamp behavior for release).
2. **Assertions for the comment-only ordering invariants** listed in the Jank Digest -- these are
   cheap to add and catch exactly the class of bug that currently surfaces as "the UI looks wrong
   for no reason."
3. **A documented, in-code note (not just project history) of the verb-first exception list**, so
   future additions to the public API don't have to guess whether `push_id`-style naming is
   sanctioned or accidental.
4. **Consolidating the "only one active widget" singletons** into one acknowledged mechanism before
   the multi-context work goes further, since every one of them breaks independently and silently
   under multi-focus.
