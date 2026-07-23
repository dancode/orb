# The ORB GUI, from the outside in

A tour of the gui service for someone who has never seen it -- written from the API
forward, not the internals out. It answers three questions in order: what do you type,
what is actually underneath, and where does it stand next to Dear ImGui / Clay / Slate.

Companion to `source/runtime_service/gui/GUI_ARCHITECTURE.md` (which is written inside-out,
for people modifying the engine). This one is for people *using* it. ASCII only; code is
source of truth.

---

## 1. The one-sentence positioning

> An immediate-mode 2D UI engine whose widget kit is a **removable client** of a strictly
> layered service stack -- so the same system is a batteries-included editor toolkit at the
> top and a bring-your-own-look interaction engine at the bottom, with a Clay-style rect
> composer and a retained geometry cache built in.

Every other design decision falls out of that sentence. The rest of this document is just
unpacking it and testing it against real use cases.

---

## 2. The ladder: what you type at each rung

Most people meet the system at the top and never leave. That is fine -- the top is a
complete editor UI kit. But the whole point of the design is that you can step *down* a rung
whenever the rung above stops fitting, and nothing below the rung you land on knows or cares
that the rungs above it exist. `source/sandbox/gui/sb_gui_base/sb_gui_base.c` is this ladder
as runnable code -- number keys 0-6 each demonstrate exactly one rung using nothing above it.

Read this table top-down (how you *start*) then bottom-up (how you *escape*):

| Rung | Band | What you type | What you get | You stop here when... |
|------|------|---------------|--------------|-----------------------|
| 7 | `CHROME` | `window_begin("Tools")` / `button("Save")` / `table_begin(...)` | Full editor UI: windows, docking, menus, tables, ~100 stock widgets | You are building a tool/editor and want the house style |
| 6 | `STYLE` | `style_source_set(fn)` / `push_style_color(...)` | The stock widgets, but wearing *your* palette across theme/font/scale changes | You want the stock widgets but your product's look |
| 5 | `ELEMENT` | `el_button(rect, "Go")` / `el_slider(rect, ...)` | A styled widget core that paints into a rect you hand it -- no windowing, no auto-layout | You own placement (a HUD, a fixed panel) but want ready widgets |
| 4 | `FLOW` | `region_begin(...)` + `row(h)` + `flow_cell(w,h)` | A scrolling region and a layout engine that hands you cells to fill | You want automatic layout/scroll but not window chrome |
| 3 | `RECT` | `carve(form, area, ...)` / `split(...)` / `anchor(...)` | Pure-math partitioning of a rect into sub-rects. Nothing drawn, no state | You know your sizes and just need geometry (HUD, canvas) |
| 2 | `SURFACE`/`CORE` | `pane_begin(...)` + `item(id, rect)` + `feat_move(...)` | Identity + hover/z arbitration + the interaction state machine over *your* rect | You are building a genuinely custom widget from scratch |
| 1 | `DRAW` | `draw_rect(...)` / `draw_text(...)` / `draw_bezier(...)` | A 2D batch renderer with an atlas. No ids, no layout, no style | You just want to paint pixels into the frame |
| 0 | `FRAME` | `boot()` / `frame_begin()` / `render(vp, cmd)` | The loop, the window, the GPU flush | You are wiring the host |

The key property: **rung N is implemented purely in terms of rungs below it, and never reaches
up.** `el_button` (rung 5) is `cell`/`rect` + `item()` + `draw_*` -- literally the rung-2 and
rung-1 verbs. `button()` (rung 7) is `flow_cell()` + `el_button()`. The compiler enforces this
per unit: a unit's include list *is* its rung, and a lower unit that includes a higher header
does not build.

---

## 3. The one idea, stated four ways

Everything distinctive about this GUI is the same idea seen from different sides.

**As an architecture rule:** chrome is a *client*. The stock widget set is written on the same
substrate a user widget uses -- not a privileged fast path with hooks the public API lacks.
If `gui_button` can do it, your widget can do it, because `gui_button` has no vocabulary you
don't.

**As a canonical example:** the leaf widget is four lines, one per seam.

```c
gui_id_t         id = item_id( label );                       // identity   (CORE)
gui_rect_t       r  = cell_next_w( label_natural_w(label), H );// placement  (FLOW)
gui_item_state_t st = item_state( id, r, ITEM_BUTTON );        // behavior   (CORE/INTERACT)
draw_fill( r, col_item_bg_anim( id, st ) );                    // paint      (ELEMENT/DRAW)
```

Identity, placement, behavior, paint -- four independent services. A custom widget swaps any
one of them (its own paint, its own rect from `carve`) and keeps the rest.

**As a data split:** `gui_style_t` sorts every field by one question -- *can reading this
field move a rect?* METRICS (row heights, pads, gaps, border, scrollbar gutter) can, and are
read by *both* the layout composer and the widgets that measure themselves, deliberately the
same numbers. SKIN (colors, rounding, mark shapes, caret) cannot, and is paint-only. Layout is
invisible to paint; paint is invisible to layout.

**As a loop property:** immediate mode with a retained twist. Widgets re-emit every frame, but
geometry is cached per window slot; a frame with an identical command hash and no input skips
emit entirely (`frame_begin` returns false). You write the easy immediate-mode code and get
close to retained-mode idle cost.

---

## 4. The two ways to place custom UI

When you drop below chrome, placement is not one thing. There are two composers and they meet
at a documented seam.

**Rect composition (rung 3) -- single-pass, known-size, pure math, nothing drawn.**

```c
static const f32 FORM[] = { GUI_CUT_X, 80.0f, 1.0f,               // sidebar | rest
                            GUI_CUT_Y, 28.0f, 1.0f, 28.0f,         //   header/body/footer
                            GUI_END, GUI_END };
gui_rect_t leaf[4];
gui()->carve( FORM, area, 6.0f, leaf, 4 );                        // one flat form -> 4 rects
```

`split` cuts one rect along an axis; `carve` runs a whole nested partition from one flat form;
`anchor` is the Slate-style anchor frame (pin a fixed size, or stretch between two fractions).
This tier *is* what Clay does, and it is the same shape: describe a partition, get rects back.

**Flow layout (rung 4) -- multi-pass, auto-sized, scrolls, stateful.**

```c
gui()->region_begin( "list", x, y, w, h, GUI_REGION_MID, GUI_WIN_NONE );
gui()->row( 26.0f );                                              // declare the shape
for ( int i = 0; i < n; ++i )
    gui()->el_label( gui()->flow_cell( 0, 0 ), GUI_ALIGN_LEFT, rows[i] );  // fill each cell
gui()->region_end();
```

Flow owns the pen, the highwater, scrollbars, and the wheel claim. Widgets never see the shape;
each asks `flow_cell`/`cell_next_w` for the next rect. One overloaded unit governs every size
value everywhere (tracks, splits, fits, packs): `t>1` px, `t==1` fill, `0<t<1` fraction,
`t==0` natural, `t<0` terminator.

**The seam between them:** `push_layout_overlay(rect)` starts a fresh flow frame over any rect
-- including a leaf `carve` handed you. That is the bridge from "I carved a HUD by hand" to
"now run real auto-layout widgets inside this piece of it."

---

## 5. Where the chrome / custom-UI split actually frays

The promise is "chrome is just a client, so the custom path has everything chrome has." The
promise holds for the *mechanisms* (identity, behavior, layout, style, paint are all public).
It frays on the *widget vocabulary*. This is the most useful thing to fix if you want the
"custom UI" story to be as strong as the "editor toolkit" story.

**Fray 1 -- the `el_*` set is thin; chrome is ~100 widgets.** The rect-taking element tier is
`el_panel / el_label / el_button / el_check / el_slider / el_meter / el_cycle / el_input`, plus
`el_selectable` (the first parity extraction -- the row primitive lifted out of chrome's
`gui_selectable`; see the parity pass). Chrome adds combo, listbox, tree, tab bar, color
pickers, drag-floatN, tables, collapsing headers, radio, and more -- but *only in flow-placed
form*. If you are below chrome
and want a rect-taking combo or tree, there is no `el_combo`; you are back to composing
`item()` + `draw_*` by hand. The custom path drops off a cliff exactly where the widget gets
interesting. **Fix: widen `el_*` toward parity, or make the flow-placed chrome widget provably
`flow_cell() + el_core()` for every widget, so no widget exists only above the seam.**

**Fray 2 -- chrome bundles two different animals in one band.** "Chrome" is both (a) windowing
*policy* -- window/dock/popup/menu/tab -- and (b) the stock *widget kit*. A game HUD wants the
widgets but not docking; a custom window manager wants pane/feat_* but its own widgets. Today
they are one rung, so "take widgets without policy" and "take policy without widgets" are not
clean cuts. **Fix: split CHROME into CHROME_WIDGETS (the stock kit over `el_*`) and
CHROME_WINDOWING (window/dock/popup/menu policy over pane/feat_*).** The ladder gains a rung
and the "good for X" branches below get cleaner.

**Fray 3 -- style ownership is a single global source.** `style_source_set(fn, user)` is the
right primitive -- a kit that owns the installed element look re-installs it at every style
landing instead of being clobbered by the theme compiler. But there is one source. A real
product with two design languages in one window (a dark canvas panel next to a light inspector)
wants *scoped/stacked* sources. **Fix: make style sources a push/pop stack, matching the
existing `push_style_color` shape.**

None of these are architectural failures -- the substrate is right. They are *seam-width*
issues: the custom path has every mechanism but not every convenience, and the one place that
matters is the widget vocabulary below chrome.

---

## 6. How it sits next to the field

The honest way to place this system: **it is the union of three tools that usually ship
separately, minus their ecosystems.**

| System | What it is | ORB overlap | Who wins where |
|--------|-----------|-------------|----------------|
| **Clay** | Pure declarative layout; emits render commands. No interaction, no style, no widgets | ORB's `RECT`+`FLOW` rungs *are* Clay's whole job | Clay: tiny, renderer-agnostic, embeddable anywhere. ORB: layout is one tier of a full stack |
| **Dear ImGui** | Monolithic immediate-mode widget kit; `imgui_internal.h` for custom; `ImDrawList` exposed | Same loop model + a superset feature set (docking, tables, multi-viewport) | ImGui: colossal ecosystem, bindings, maturity. ORB: explicit strata, chrome-as-client, native retained cache + idle skip, built-in rect composer |
| **Nuklear** | Single-header C immediate mode, more customizable than ImGui | Same C, immediate-mode niche | Nuklear: single-file drop-in. ORB: layered, hot-reload, richer widget kit |
| **egui** (Rust) | Immediate mode; AccessKit accessibility; big Rust ecosystem | Same philosophy | egui: accessibility + i18n + Rust crates. ORB: C, hot-reload, layered architecture |
| **Slate / UMG** (Unreal) | Retained widget tree, declarative, invalidation-based; designer tooling; a11y + localization | Both target editor + shipping game UI | Slate: shipping-product features (localization, accessibility, visual authoring). ORB: immediate, tiny, hackable, no object-tree ceremony |

The one-line reads:

- **vs Clay:** ORB contains Clay's job as one rung and wraps interaction, style, widgets, and a
  renderer around it. If all you want is layout math you don't need, use Clay. If you want the
  layout to feed a real interaction engine, that seam already exists here (`carve` ->
  `push_layout_overlay`).
- **vs Dear ImGui:** the closest sibling and the fairest benchmark. ORB's bet is that ImGui's
  power-user story (`imgui_internal.h`, custom widgets, `ImDrawList`) should have been the
  *documented, layered, first-class* surface from day one -- so it is. ImGui's bet is that the
  ecosystem and 10 years of hardening matter more than architectural cleanliness. Both are
  right for different teams.
- **vs Slate/UMG:** the philosophical opposite. Slate is a retained object tree with
  invalidation, designer tooling, accessibility, and localization -- the machinery a *shipping,
  localized, accessible product* needs, at the cost of weight and ceremony. ORB is immediate,
  C, hot-reloadable, and hackable, at the cost of exactly those shipping-product features. This
  is the sharpest fork in the road and Section 7 is mostly about which side of it a use case
  falls on.

**Where ORB is genuinely differentiated (not just "another IM UI"):**

1. The strata are the product. Chrome is removable; you can ship at rung 3, 5, or 7.
2. A pure rect composer (`carve`/`anchor`) as a first-class tier, not a layout afterthought.
3. Native retained geometry cache + idle skip -- immediate-mode ergonomics, near-retained idle
   cost -- built in, not bolted on.
4. Hot-reload-first: the whole thing is a reloadable module; iterate the UI without restart.

---

## 7. "Is it good for X?" -- verdicts and branches

For each use case: the verdict, and if it is not already there, the *branch* -- the specific
work that gets you there. This is the working-backwards payoff: it tells you what the system
is, by telling you exactly what it is not yet.

| Use case | Verdict | The branch to get there |
|----------|---------|-------------------------|
| Game debug tools / in-engine editor | **Yes -- core competency** | none; this is what rung 7 is |
| Node graph / canvas-heavy tools | **Yes -- sweet spot** | none; `canvas` + `item()` + bezier/path + custom-widget path fit exactly |
| Data-dense tables / spreadsheets | **Yes -- heavily invested** | none; table engine (tracks/sort/span) + `rows_clip` virtualization exist |
| Shipping game HUD / in-game UI | **Yes -- proven** | skip chrome: `pane` + `carve` + `el_*` + your own `style_source`. The `ui.h` game kit is the reference |
| Very high widget count (10k+) | **Yes -- with virtualization** | use `rows_clip`/`table_rows_clip`; retained slot cache + clip grouping + idle skip already invested |
| Custom-look product UI (design system) | **Mostly** | `style_source_set` + METRICS/SKIN gets you far; scoped style sources (Fray 3) needed for two looks in one window |
| Complex desktop app (DAW/Blender-class) | **Mostly** | docking/tables/multi-viewport/multi-context exist; strains on text (Section 8) and accessibility |
| Animation / motion-design UI | **Partial** | anim service (dampers/timers) + `volatile_cb` exist; but sub-pixel *text* motion is out -- NEAREST atlas cannot sub-pixel glyphs (motion-snap was tried and reverted) |
| Localized / international product | **No -- hard branch** | needs a text-shaping subsystem: per-run fonts (today one global font at tessellation time), CJK/RTL/bidi, and an IME composition surface. This is a real subsystem, not a patch |
| Accessibility-required (gov/enterprise) | **No -- hard branch** | no accessibility tree today. The id namespace + `debug_name` registry is a seed to grow an AccessKit-style semantic export from, but immediate mode makes the persistent tree the hard part |
| Web / browser target | **No -- backend branch** | the render server is narrow (push primitives + atlas + GPU flush), so a WASM + WebGPU/WebGL backend is a "swap the backend" project -- plausible, but none exists |
| Designer/non-programmer authoring | **No -- by design** | it is immediate-mode C: no markup, no visual designer, no data-binding. The branch is a retained authoring layer on top (UMG-over-Slate shape) -- a large project, nothing today |

Read the "No" rows as the system's true shape: it is a **programmer-facing, immediate-mode,
Latin-text 2D UI engine for tools and games.** The three hard branches -- text shaping,
accessibility, retained authoring -- are exactly the three things that separate "excellent
tools UI" from "shipping consumer application UI," and all three are hard for the same reason:
immediate mode has no persistent semantic tree, and each of those features wants one.

---

## 8. The honest gaps, collected

So a new user is never surprised:

- **Text is Latin, one font per frame.** Rendering pulls from ONE global active font at
  deferred tessellation time; `push_font`/`pop_font` cannot scope a second font within a run.
  No shaping, no bidi, no RTL, no CJK input. ASCII-only is an engine-wide ethos, not just a
  style rule.
- **No sub-pixel text motion.** The atlas samples NEAREST; text cannot smoothly sub-pixel
  animate. Motion-snap was implemented and deliberately reverted. Widgets animate; glyph
  positions snap to pixels.
- **No accessibility tree.** No screen-reader export. The id + debug-name registry is the only
  seed.
- **No visual/declarative authoring.** You write C. Layout is code, not markup or a designer.
- **No web/mobile backend.** Windows-first (Win32 + Vulkan); POSIX is maintained but untested
  for UI; no browser target.
- **The custom-UI widget vocabulary trails chrome's** (Section 5, Fray 1) until `el_*` reaches
  parity.

None of these are bugs. They are the boundary of the design, and every one of them is a
consequence of the choices in Section 3 that make the good cases good.
