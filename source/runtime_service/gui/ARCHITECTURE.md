# GUI Architecture

In-house immediate-mode GUI for ORB. No Dear ImGui, no GLFW/SDL. Windowing/input come
from the engine `app` layer (Win32); rendering goes through `rhi` (Vulkan). This document is
the orientation map: read it before chasing a bug across files, so you reconstruct the system
from here instead of from cold code every time.

ASCII only, like the rest of the tree. No Unicode in this file or in source.

---

## 1. The one idea that explains everything: three state tiers

Almost every "why does this work / why did this break" question resolves to *which tier a
piece of state lives in*. There are exactly three, and they have different lifetimes and
different sharing rules.

**Tier 1 -- Ambient singular (module-static globals, one per app).**
There is one physical user: one mouse, one keyboard, one focus. So this state is a single
global, never duplicated per context.
- `s_interaction` (gui_ctx.c): `hover_id`, `active_id`, `focused_id`, `hover_win`, cursor
  request, focus-departure latches.
- `s_io` (gui_input.c): the distilled per-frame input snapshot.
- `s_replay_mode` (gui_ctx.c): true while a volatile widget is being replayed standalone.

**Tier 2 -- Per-context retained ("bind and use").**
State that must survive between frames *for one UI*: window geometry, scroll offsets, keyed
widget state, nav cursor, popups, viewports, dock tree, table state. All of it hangs off one
struct, `gui_context_t`, reached through the global `g_ctx` pointer. Switching contexts is a
single pointer assignment (`ctx_bind`) -- no copy, no save/restore. Named aliases resolve
through `g_ctx`: `s_retained`, `s_nav`, `s_windows`, `s_popups_open`, `viewports`,
`dock_nodes`, `table_pool`.
Every context, including the default (slot 0), is a single heap-allocated block: one `malloc`
in `ctx_alloc_slot` holds the `gui_context_t` header followed by its state/popup/window/
viewport/dock pools, with `_alloc` pointing at the block. Slot 0 is created at `gui_init`
(`ctx_pool_init`, editor-profile defaults, starts `listening`); secondary contexts are created
on demand by `ctx_create` (start deaf). Each gets a unique `id_salt` (`slot * 0x9e3779b9`, so
slot 0's salt is 0) so identically-named widgets never alias across contexts.

**Tier 3 -- Frame scratch (module-static, rebuilt every frame).**
Thrown away and rebuilt each frame; shared across all contexts in a frame (a second context
reuses the same scratch). Never stash anything here that must outlive the frame or differ per
context.
- UI unit: `s_build` (flat current-window/clip/item-flag context), the stacks (`s_layout`,
  `s_id`, style, item-flag), `s_draw` command list.
- Render-backend unit: `s_tess` (tessellated geometry), `s_dispatch` (z-sorted slot table).

If you are unsure where to add a field, decide the tier first. Wrong-tier state is the most
common structural bug: a per-context value parked in Tier 3 bleeds between contexts; an
ambient value copied per-context desyncs.

---

## 2. Two translation units

`gui` builds as **two** unity TUs, not one:
- **UI unit** -- `gui.c` `#include`s every `core/`, `widgets/`, `window/`, `popup/`, `dock/`,
  `table/` file. This is layout, widgets, interaction, the semantic draw list.
- **Render-backend unit** -- `gui_backend.c` `#include`s the `backend/` files. This is
  tessellation, the geometry cache, GPU upload, flush, fonts, the debug overlay.

`gui_internal.h` lifts every cross-file *type* into one place so include order does not matter.
`gui_backend.h` is the one-way seam: the UI unit calls into the backend (draw_push_*,
render_flush, font accessors); the backend does not reach back up (two tiny debug/font
exceptions aside). Keep that arrow one-way.

Header split per the house convention: `gui.h` (types) -> `gui_api.h` (DLL) ->
`gui_host.h` (hosts/sandboxes). `gui_internal.h` is unity-build-private, above `gui_host.h`.

---

## 3. File map

```
gui.c                  UI unity entry (includes all UI-tier .c files)
gui_backend.c          render-backend unity entry (includes backend/ files)
gui_internal.h         shared cross-file TYPES (the tier-2/tier-3 record layouts)
gui_frame.c            frame lifecycle, viewports, fonts, clip, perf/state overlays
gui_api.c / gui_api.h  the vtable + the public FRAME CONTRACT
gui_dashboard.c        pipeline dashboard (an ordinary DEBUG_BAND window)

core/                  the engine of the UI unit
  gui_ctx*.c           context state, ID hashing, keyed state pool, IO accessors, new_frame
  gui_input.c          OS event -> s_io snapshot; io_dirty
  gui_layout*.c        the layout engine (pen, highwater, rows/grid/pack, regions, children)
  gui_widget_core.c    widget_behavior: the shared hover/active/click resolver
  gui_style/theme/anim/symbol/stacks/resize/region   ambient draw state + helpers

widgets/               leaf widgets (button/slider/numeric/text_edit) + volatile widgets
window/                window record behavior + window chrome (title/resize/collapse/close)
popup/                 popups, context menus, tooltips, combos, menus, keyboard nav
dock/                  dock-node tree: tabs, splitters, drag-to-dock, serialize
table/                 Dear ImGui-style tables

backend/
  pipeline/
    gui_emit_draw.c    EMIT: draw_push_* -> semantic command list + segments
    gui_build_tess.c   tessellation (semantic shape -> vertices/indices)
    gui_build_cache.c  BUILD: per-window diff, reuse/retessellate, z-sort dispatch
    gui_build_volatile.c   volatile widget reserve/replay path
    gui_render.c       RENDER: GPU resources, upload, indexed draw submission
    gui_emit_path.c / gui_shader.h
  resource/            font registry (bit/bmp/ttf), icon atlas
  gui_debug_overlay.c  wire/batch debug overlay
  gui_dash_capture.c   dashboard snapshot capture
```

---

## 4. The frame contract

The host drives the frame. The lifecycle (see `gui_api.h` FRAME CONTRACT and `gui_frame.c`):

```
frame_begin(dt)                 once. Input poll + draw-list reset. Binds NO context.
  ctx_begin(GUI_CTX_DEFAULT)    bind + per-context init; emit this context's windows
    window_begin(...) / ... / window_end()
  ctx_end()                     restore the previously-bound context
  ctx_begin(ctx2) ... ctx_end() any additional contexts
frame_end()                     seal the build; assert all ctx_begins were matched
render(vp, cmd)                 once per live viewport: flush that surface
```

Division of labor:
- `frame_begin` is the **global** half: it polls input, computes the frame-dirty flag, and --
  only on a dirty frame -- resets the draw list and the global interaction state
  (`interaction_frame_reset`). It binds no context.
- `ctx_begin` is the **per-context** half: binds `g_ctx`, runs `ctx_new_frame` (per-context
  scratch reset + frame-clock bump), plus popup/modal/raise/nav per-frame steps. It never
  touches `s_interaction` (that was already reset once, globally).

A single-context host runs exactly one `ctx_begin(DEFAULT)`/`ctx_end` pair.

---

## 5. The render pipeline: EMIT -> BUILD -> RENDER

The single most important dynamic model. Three phases, each in its own file:

**EMIT (gui_emit_draw.c).** Widgets push *semantic* shapes (`gui_cmd_t`: rect, text run,
polyline, icon...) into `s_draw.cmds[]`. No vertices yet. Each command is stamped with the
current `(win, z, vp, font, band)`; whenever any of those changes, the open span closes and a
new one opens, cutting the buffer into contiguous **segments**. One hash is baked per command
at emit time -- that hash is what BUILD diffs against.

**BUILD (gui_build_cache.c).** Runs **once per frame, lazily**, on the first `render()` call
(guarded by `s_frame_built`, cleared at `frame_begin`). It walks each window's segments, diffs
their command hashes against last frame, **reuses unchanged geometry in place** and
**re-tessellates only changed windows**, then z-sorts the result into `s_dispatch`. Sibling
windows that did not change keep their geometry.

**RENDER (gui_render.c).** Runs **once per surface**. Uploads that surface's slice of the
shared geometry into the viewport's per-frame-in-flight VB/IB region and emits one indexed draw
call per cached GPU command, back-to-front in dispatch order. Geometry is surface-independent
(BUILD produced it once); RENDER just routes each window's slice to the viewport hosting it.

---

## 6. The caching layers (why a frame may do less work)

There are three independent skip/reuse mechanisms. Know which one you are looking at.

**(a) Whole-frame emit skip -- `s_frame_dirty` (gui_frame.c).** A single global bool computed
in `frame_begin`:
```
s_frame_dirty = force_redraw || io_dirty() || wants_redraw || build_any_changed()
```
When false, the host skips `ctx_begin` / widget emit / `ctx_end` entirely and calls `render()`
directly. The previous frame's `s_draw`, `s_tess`, and `s_dispatch` are all preserved and
replayed verbatim. This is the big win for an idle UI.

**(b) Per-window geometry reuse -- BUILD (gui_build_cache.c).** Even on a dirty frame, only the
windows whose command hashes changed are re-tessellated. Granular, per window slot.

**(c) Volatile widgets (gui_build_volatile.c + widgets/gui_volatile.c).** A widget declared
volatile reserves padded sub-slots and is replayed by re-tessellation into that reservation,
so a single animating widget updates without invalidating its whole window. On an idle frame it
can be replayed standalone under `s_replay_mode` (which renders with the ambient hover/active
state the last real frame established, and never resolves fresh interaction).

The failure mode shared by all three: **a state change that the dirty test cannot see stalls
until the next real input.** Anything that mutates state at pop time or one frame deferred
(wheel scroll applied at region pop, a window toggle, an in-flight animation) must feed the
dirty test -- animations via `gui_anim_f32` (sets `wants_redraw`), other deferred mutations by
setting `wants_redraw` explicitly. See the invariants.

---

## 7. Core subsystems, briefly

**IDs + keyed state (gui_ctx_id.c).** A widget id is `id_hash(label)` (FNV, XOR-seeded by the
context's `id_salt`). Nesting combines via `id_combine`/the id stack. A widget keeps a few bytes
across frames in the per-context keyed state pool (`gui_state_get`/`GUI_STATE`), an
open-addressed hash keyed by id, LRU-reclaimed by `seen_frame`. Payload is capped at
`GUI_STATE_CAP` (24 bytes).

**Layout (gui_layout*.c).** A `layout_frame_t` per scrollable region. It carries a **PEN**
(where the next item goes) and a monotonic **HIGHWATER** (the far corner content reached, used
at pop to size scrollbars/autosize). Forward flow advances both; a pen reposition (table row,
menu-bar restore) moves the pen alone so the highwater never rewinds. Row/grid/pack templates,
alignment, field-split, and same-line all build on one open-line record. A region opens with a
default single-flex-column template, so a plain vertical stack needs no layout call.

**Windows + viewports (window/, gui_frame.c).** A `gui_window_t` owns persisted geometry, z,
target viewport, scroll, and flags. A `gui_viewport_t` is a render *surface*: slot 0 is the
host-owned main swapchain, slots 1+ are floaters (possibly gui-owned tear-offs). **Viewport slot
index == app win_id** -- the input router depends on it (`APP_WIN_MAX == GUI_MAX_VIEWPORTS`).
Owned floaters have their whole OS-window + rhi-context lifecycle managed by gui and are torn
down only at `gui_viewport_update` (a safe point between build and present).

**Interaction resolution (gui_widget_core.c).** `widget_behavior` is the one producer of
`widget_state_t` (hover/active/pressed/clicked/focused/nav); every widget consumes it instead of
touching `s_interaction`. `hover_win` is resolved **one frame deferred**: each `window_begin`
nominates itself, and the front-most (highest z) nominee is promoted at the next
`frame_begin`. Only the hover window hit-tests its widgets; within a window, widgets do not
overlap, so widget hover is immediate.

**Popups (popup/).** A popup is a top-level overlay begun while a parent window is open but laid
out/clipped/painted independently. It is stamped into a reserved high z-band
(`GUI_POPUP_Z_BASE`, 0x80000000) so it paints above every normal window; a window in that band
is an overlay, never a native OS-window frame. `gui_overlay_save_t` snapshots exactly the
cross-cutting state that `window_begin`/`window_end` clobber, restored at `popup_end`. The open
set is a parent->child stack on the context.

**Docking, tables, nav, fonts.** Dock tree = a fixed per-context node pool (leaf tabs / internal
splits), indexed by pool ref so handles survive frames. Tables = per-context persisted column
widths/sort/scroll, one active table at a time. Nav = a keyboard cursor mirroring `hover_id`,
scored directionally, with a menu-bar state machine layered on top. Fonts = an id-addressed
registry in the backend; loads can defer an atlas swap to the next `frame_begin`, which then
rescales layout metrics.

---

## 8. Invariants and points of caution

Break one of these and the failure is usually non-local (a stale frame, a cross-context bleed, a
surface freed under an in-flight draw). Check this list first when a change misbehaves.

**Frame / lifecycle**
- `interaction_frame_reset` runs **once per app frame** (in `frame_begin`), never per context.
  Calling it per context would let a second `ctx_begin` clobber the hover/active state the first
  established.
- `ctx_new_frame` must **not** touch `s_interaction` (Tier 1). It only rebuilds Tier-2/Tier-3
  per-context scratch.
- Every `ctx_begin` must be matched by a `ctx_end`; `frame_end` asserts the balance. Same for
  `window_begin`/`window_end` -- call `window_end` **even when `window_begin` returns false**
  (collapsed), or the layout/clip/popup stacks unbalance.
- A font must be active before `ctx_begin` (asserted). With no font, `s_style` is zero and every
  widget collapses to zero size -- an invisible UI, not a crash.

**The dirty test (the most common "it only updates when I move the mouse" bug)**
- Any state change the dirty test cannot observe **stalls the update** until the next real input.
  Deferred/pop-time mutations (wheel scroll, window toggle, external state pushed between frames)
  must set `wants_redraw`, or the clean-frame skip (6a) reuses the stale draw list indefinitely.
- Any widget that animates must drive its value through `gui_anim_f32`, which sets
  `wants_redraw`; otherwise the animation freezes on the first idle frame.
- To diagnose a suspected stall, pin `set_force_redraw(true)`: if the symptom vanishes, the bug
  is a missing dirty signal, not the emit.

**Pipeline / segments**
- Every `draw_push_*` inherits the current `(win, z, vp, font, band)`. A change in any of those
  must cut a **new segment** -- commands must never merge across window slots, or one window's
  geometry cache poisons another's. When adding a batch-context axis, add it to the segment key.
- BUILD runs lazily on the first `render()` and is guarded by `s_frame_built` (cleared at
  `frame_begin`). Build/render **stats are published at `frame_begin`**, so a reader sees the
  previous frame's totals -- the perf overlay trails by one frame by construction.

**State tiers / pools**
- Do not park per-context state in Tier-3 scratch (`s_build`, `s_draw`, `s_tess`) -- a second
  context reuses that scratch in the same frame. Per-context state lives on `gui_context_t`.
- Keyed-state payloads must fit `GUI_STATE_CAP` (24 bytes); the largest state struct must not
  exceed it.
- Fixed pools (windows, viewports, dock nodes) never compact by value; a slot is identified by
  its pool index and a freed slot has `id == 0`. Never assume a pointer into a pool moved.

**Surfaces**
- Viewport slot index == app `win_id`. Preserve it on every spawn/open path.
- Tear down owned floater surfaces **only** in `gui_viewport_update` (between build and present),
  never mid-build -- no in-flight draw list may reference a surface being freed.
- `caption_inset` is sticky: it is not cleared each frame, so window clamping always has a valid
  top bound regardless of build order.

**House rules (apply everywhere in gui)**
- Widgets **self-fit**: ellipsize/shrink to their rect (e.g. `draw_text_fit_n`). Do not add clip
  rects to contain overflow -- fix the widget to fit its cell.
- Prefer the smallest mechanism: a tag/discriminator inside an existing system over duplicating a
  subsystem.
- ASCII only, in code and comments.
</content>
</invoke>
