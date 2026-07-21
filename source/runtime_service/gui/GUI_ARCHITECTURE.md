# GUI Architecture + Layout/Composer Notes (AI-oriented)

Dense reference for working on the gui service (this directory) and its sandbox
`source/sandbox/gui/sb_gui/sb_gui.c`. Code is source of truth. ASCII only in all source.

## Big picture

Static lib `gui`, six translation units (the per-library TU split, GUI_STACK_PLAN inc 10 --
cross-unit reach goes through the ambient-record externs + service seams in `gui_internal.h`,
so each library boundary is compiler-enforced):

- `gui.c` (core + frame unit): context, id/state pool, input snapshot, the interact/ services,
  present/ paint primitives, surface service, user/ vocabulary, frame lifecycle, mod vtable.
  Unity-includes its constituents; directories name ROLES and the include list in gui.c is THE
  dependency order (core/ -> surface/ -> interact/ + present/ (siblings) -> user/ -> frame/,
  the conductor). Include order matters; later files may reference statics from earlier ones.
  `surface/gui_surface.c` is the surface service: window records as placed, stacked,
  occluding rectangles before any layout or chrome -- the record pool (`window_get` /
  `window_find`), the next-window placement channel, the z policy (the tier authors ALL z: the
  band map -- region tiers, the overlay band -- is defined here, `surface_z_raise` is the
  dispenser's only verb, and popups stamp their band through `surface_z_overlay` plus the
  record's `overlay` type flag, so z itself is pure paint order), the hover-win occlusion
  contest (`surface_hover_nominate`, entered by windows, floating dock groups, and root regions
  alike), the surface reassignment request slot (tear-off / merge-back, serviced by the
  conductor), and open/closed state. Storage + frame turnover
  stay in `gui_context_t` (core), the house pattern; window GESTURES (drags, grips,
  raise-on-press with its dock exception) are window/ policy over these services.
- `gui_backend.c` (render unit): fonts, draw list, tessellation, GPU flush, debug overlay.
  UI unit calls it one-way through `gui_backend.h` (`draw_*`, `font_*`, `gui_render_*`).
- `element/gui_element.c` (element unit): the `el_*` rect-consuming widget cores + the
  installed `gui_el_style_t` (see gui_api.h GUI_ELEMENT / docs/GUI_STACK_PLAN.md). Reaches
  the rest of gui ONLY through the public `gui_*` surface plus the `style_active()` seam
  (gui_internal.h) -- the library boundary is compiler-enforced.
- `compose/gui_flow.c` (flow unit): composition -- spacing metrics in, rects out. Track
  resolver + cell emitters, scroll regions, children, sub-layouts, splits, the root region,
  the public layout verbs + sz_ sizing. Two upward seams only: `scrollbar_widget` (the
  gutter's one widget) and the `gui_anim_*` ease.
- `gui_chrome.c` (chrome unit): widgets/ + table/ + window/ + dock/ + popup/ + nav/ -- the
  stock set and the host structures, composing the core services + flow's emit surface.
  Its core-facing definitions (the frame lifecycle's window/popup/nav/dock steps) are seams
  the other direction.
- `debug/gui_debug.c` (debug unit): the pipeline dashboard + command stepper -- severable
  tooling over the backend capture snapshots (`gui_frame_overlay.c` stays with the frame
  group: the lifecycle calls its timing helpers).

## The composer / behavior / presentation split

Three roles, one contract (the directories carry the same names):

- **Composer** (`compose/`): the ONLY code that POSITIONS rects -- divides regions into
  cells, moves the pen, decides where the next widget lands. Widgets MEASURE themselves with
  the same METRICS vocabulary (a button's natural width is its label plus pad) but only ever
  REQUEST a size through `cell_next_w`; the composer decides placement. Composes and
  never paints. Public face: the layout verbs + the `sz_` sizing family.
- **Behavior** (`interact/`, public door `user/gui_behavior.c`): widget-agnostic
  interaction SERVICES over the core/ utilities (identity `core/gui_id.c`,
  keyed state tracking `core/gui_state.c`, the io snapshot `core/gui_io.c`;
  the public readers over it are `user/gui_query.c`) -- the drag threshold machine + payload
  transfer (`gui_drag.c`), the move-drag protocol + deferred-press latch (`gui_move.c`:
  `move_grab`/`move_track`, `press_defer_*`), the edge-resize mechanism (`gui_resize.c`:
  `resize_item`), and the standard item protocol (`gui_item.c`: `item_state`, the default
  composition every stock widget runs, plus `item_grab`, the bare grab for hot chrome that is
  not a widget -- a dock splitter gutter, a table column boundary; the compound-widget bracket
  `gui_item_sub_begin/end` scopes a widget's INNER item emissions so the queries after it
  report the outer widget, and `gui_item_sub_layout_begin` is its full form -- id scope +
  sub-layout over the widget's rect, so a widget can emit real widgets inside itself). Each
  service knows a
  capability (exclusivity, clicks, tracking) over (id, rect); none knows a slider. Consumes
  finished rects, produces interaction state (`hover`/`active`/`pressed`/`clicked`). This tier
  is the ONLY writer of the `s_interaction` arbitration fields: the window/dock/table hosts
  claim hover/active/focus exclusively through these verbs and read the record for gating,
  never write it raw (the popup modal fence claims through `interact_hover_fence`, no
  exceptions).
  Behavior's only inputs beyond (id, rect) are the interaction scope (`s_scope`,
  `core/gui_ctx.c`): the owner window, the interaction clip, the chrome hover
  suppression, and the per-item flag/nav stamps -- placed there by composition at its seams
  (window/child/popup/table begin, the emit seam `cell_next_w`).  Behavior never reads
  the composer scratch (`s_build`); the scope record IS the composition->behavior contract,
  and behavior publishes its per-item result back into it (`s_scope.last_*`, read by the
  `is_item_*` queries and the context-menu / drag anchors).
  Style is invisible below this line -- a service never
  reads a skin value and never paints (the one metric it reads is `WIN_BORDER`, because the
  resize grab band and the scroll view box straddle the border, and border is geometry): the
  system adornments (nav focus ring, drag accept ring, hot resize edges) are invoked from
  behavior at the protocol point but painted by present-tier helpers (`draw_nav_ring` /
  `draw_drop_ring` / `draw_resize_highlight` in `present/gui_paint_core.c`), so the paint
  policy lives with the skin.
- **Presentation** (`present/`: label grammar + self-measurement (`label_natural_w`),
  text-fit, frame color policy, system adornments, symbol draws): consumes rect + state + skin
  and paints; state is a parameter, it never asks behavior. The widget paint floor is
  rect-taking (`draw_fill( r, col )` / `draw_outline( r, t, col )` in `gui_paint_core.c`, plus
  the `draw_*` symbol palette): widgets speak rects; only the backend emit layer
  (`draw_push_*`) speaks scalar x/y/w/h with UV + texture arguments.

The internal prefix names the seam a widget crosses: `item_id` (identity), `cell_next(_w)` /
`cell_reach` / `cell_split_field` (composer), `item_state` / `item_grab` (behavior),
`label_*` / `draw_*` / `col_*` (presentation), `io_*` / `gui_state_*` / `gui_anim*`
(core services).

Static-function charter (how internals stay organized):
1. The file is the family; the prefix is the file's noun (`nav_*` in gui_nav.c, `dock_*` in
   dock/). A second mini-family in the same file gets its own banner section (`press_defer_*`
   in gui_move.c).
2. A narrow helper adopts its user's language, not a system prefix (`checkable_cell`,
   `slider_render`, `carve_skip`).
3. A shared pure utility moves to gui_internal.h's stateless-helpers section on its SECOND
   user; single-file utils (`fnv1a`, `rect_empty`) stay local.
4. The door is at the bottom: pure leaf helpers first, mechanism next (bottom-up), the file's
   seam / public face last (`cell_next_w` ends gui_layout_core.c, `window_begin_ex` ends
   gui_window_free.c).

The canonical leaf widget is four lines, one per seam:

```c
gui_id_t         id = item_id( label );
gui_rect_t       r  = cell_next_w( label_natural_w( label ), WIDGET_H );
gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
draw_fill( r, col_item_bg_anim( id, st ) );
``` The style vocabulary itself
  (`WIDGET_*` / `WIN_*` / `COL_*` macros) lives with its resolver in `core/gui_style.c`
  since all three roles read it. `widgets/` and the window/dock/popup chrome are its
  CLIENTS -- the stock widget set is written on the same substrate a user widget uses, not a
  privileged layer.

`user/` is the top tier and the public door onto the first two roles -- the caller's vocabulary,
pure verbs + readers with zero state or machinery: `canvas`/`split`/`carve`/`empty` for
rects, `item`/`invisible_button` for behavior, `draw_*`/`text_size` for your own presentation,
the bracketing stacks (`push_id`, item flags, `push_style_*`, `scale_push`, `disabled_begin`),
and the query readers (`want_capture_*`, `is_item_*`, `is_key_*`). Nothing below depends on it;
internal uses (combo's `push_id`, the overlay's key reads) deliberately dogfood the public
surface through gui_host.h declarations.

A custom widget (`game_ui_slider()`) never needs skin or spacing metrics -- it brings its own
look and composes with any layout.

`gui_style_t` sorts every field into two bundles by one test -- can a read of this field move a
rect? (see gui.h): METRICS (row heights, gaps, pads, border, title bar, indicator sides, the
scrollbar gutter -- consumed by the composer to size cells AND by widgets to measure/seat
themselves, deliberately the same numbers) and SKIN (colors, roundings, mark shapes, caret --
paint-only, provably never sizes a cell).

Consumers call through the module vtable: `gui()->verb(...)`. gui links only `app` + `rhi`;
OS services it cannot reach (clock/sleep/wait) are injected via `set_frame_hooks`
(done for you by the test-bed `boot()` path).

Immediate mode with a retained twist: widgets re-emit every frame, but geometry is cached per
window slot and a clean frame (identical command hash, no input) skips emit entirely
(`set_retained_skip`, `frame_begin` returning false). Text renders from ONE global active font
at deferred tessellation time -- `push_font`/`pop_font` cannot scope a second font per-run.

## Frame lifecycle -- two tiers

The core lifecycle is always the same sequence; what differs is who owns the window, the rhi
context, and the render/present step.

### Standard manual loop (real hosts: runtime/run_host.c, sandbox/rhi/sb_vulkan/sb_vulkan.c)

The host owns everything: it creates the app window + rhi context itself, then attaches gui to
them. `gui_boot.c` is NOT involved.

```
// setup (after app window + rhi context + swapchain exist)
gui()->init_config_front( caps );                 // optional feature gates, BEFORE init
gui()->init( font );                              // or GUI_FONT_NONE
gui()->set_frame_hooks( clock, sleep, wait );     // OS services gui cannot reach itself
gui()->debug_enable( true );                      // optional hotkey driver
gui_vp_t vp0 = gui()->viewport_open( win_id );    // attach gui to the EXISTING window/ctx

// per frame
while ( app pump )                                // host pumps OS events itself
{
    // events: rhi()->event() first (swapchain resize), then gui()->event( &ev );
    //         true = gui consumed it (input, floater lifecycle); leftovers go to the app
    bool gui_ran = gui()->frame_begin( dt );      // false = clean frame, SKIP the build
    if ( gui_ran )
    {
        gui()->ctx_begin( GUI_CTX_DEFAULT );
        ...emit...                                // host_main emits viewport_shell + services
        gui()->ctx_end();
    }
    gui()->frame_end();                           // replays volatile_cb blocks
    gui()->viewport_update();                     // platform window sync (safe teardown point)

    // render: the host opens the frame and composites gui over its own passes --
    rhi_cmd_t cmd = rhi()->frame_begin( ctx_id ); // (or render()->begin_frame/draw_scene/
    ...host clear / scene passes...               //  frame_cmd when a render module owns it)
    gui()->render( vp0, cmd );                    // gui draw list over the finished scene
    rhi()->frame_end( ctx_id );                   // present (render path: render->end_frame)
    gui()->viewport_render_floaters();            // tear-off windows, after the main surface
    // pacing is host policy (host_main has its own; frame_pace() also works standalone)
}
gui()->shutdown();
```

run_host.c picks a render path per tick: render module owns the frame and gui composites
over `render()->frame_cmd()`; gui-without-render drives the explicit rhi frame by hand;
draw-only skips gui entirely. In every path gui is one `gui()->render( vp, cmd )` call inside
an already-open frame -- gui never owns the swapchain here.

### Boot convenience wrapper (test-bed tier only: sb_gui.c, sb_gui_editor)

`gui_boot.c` composes the same PUBLIC app/rhi/gui primitives above into one-call setup + an
easy-mode loop; it owns nothing the manual path cannot do, and everything still works
unchanged without it.

```
gui()->boot( &desc )                    // window + rhi ctx + font + hooks + viewport_open
while ( gui()->frame_poll( &dt ) )      // pumps OS, routes rhi + gui events; false = quit
{
    if ( gui()->frame_begin( dt ) ) { gui()->ctx_begin(...); ...emit...; gui()->ctx_end(); }
    gui()->frame_end();
    gui()->present_begin( NULL );       // opens main surface frame; bool gates host passes
    gui()->present_end();               // gui()->render + present + render floaters
    gui()->frame_pace( 4, 16 );         // spin/idle pacing (idle-skip aware)
}
gui()->shutdown();                      // also tears down the boot window + context
```

Mapping: `boot` = window/ctx creation + init + hooks + `viewport_open`; `frame_poll` = event
pump + routing; `present_begin/present_end` = `rhi frame_begin` + clear + `gui()->render` +
present + `viewport_render_floaters`.

Invariants:
- `window_begin` false means collapsed OR closed; always still call `window_end`. Distinguish
  a real close with `window_is_open( title )` before clearing a host `p_open` flag.
- Any state mutation done outside the emitted build (or at pop time) must not depend on
  re-emit happening next frame -- clean-frame skip will freeze it unless something sets
  wants_redraw (input, animation, `volatile_cb`).
- `volatile_cb` blocks keep animating on skipped frames but MUST keep a fixed layout
  footprint (constant size; pad printf fields).

## Layout engine (compose/gui_layout_core.c = mechanism, compose/gui_layout.c = public verbs)

Model: every window body / region / child owns a `layout_frame_t` with a content box, a PEN
(`pen_y`, flows downward) and a HIGHWATER (`high_x/high_y`, monotonic bounding box used at
region pop to decide scrollbars). `content_reach` moves both; only `layout_pen_jump` parts
them. The frame groups its other state by lifetime: `tmpl` (the installed shape), `mod`
(orthogonal modifiers), `line` (iteration cursor + the open line, re-zeroed per install).
Widgets never see the layout shape: each calls `cell_next_w(natural_w, h)` and is
handed the next cell.

### The one overloaded unit rule (used EVERYWHERE: tracks, splits, fit, pack)

For any size value `t`:
- `t > 1`   : fixed pixels (never floored -- explicit px is authored intent)
- `t == 1`  : fill (equal share of leftover; multiple fills split it)
- `0 < t < 1`: fraction of the gap-adjusted extent
- `t == 0`  : natural (widget's own measure).  A pre-divided COLUMN track resolves it from
  measured feedback: the widest natural item placed in that column last frame (one-frame lag,
  floored at `WIDGET_MIN_W`); grid ROW tracks and the pure-math callers (field_split, split,
  carve) have no measure and it collapses to zero there
- `t < 0`   : unset/terminator (`GUI_END = -1.0f` ends track lists)

Two resolvers only: `layout_resolve_tracks` (lists) and `unit_resolve` (scalar). Flex/fraction
tracks floor at `WIDGET_MIN_W` and the row overflows into the clip; fixed px never floors.

### Modes -- a region opens UNDECLARED (GUI_MODE_NONE)

The first layout header names the mode. A widget emitted before ANY header is a usage error
(debug assert; release falls back to stack). Templates persist until replaced; no push/pop.

| Header verb | Mode | Behavior |
|---|---|---|
| `stack()` / `row(h)` | STACK | one flex column, rows accumulate + scroll (the default list) |
| `cols(tracks)` / `cols_n(n)` / `row2/3/4(...)` / `row_cols(h,tracks)` | COLUMNS | repeating pre-divided column template; wraps row-major |
| `grid(desc)` / `grid_cells(nc,nr)` | GRID | fixed cols x rows matrix from pen to region bottom, both axes resolved up front; no scroll; overflow clamps to last cell |
| `bar()` / `strip()` | PACK | print run: items at natural size along the axis; `pack_size(u)` overrides next item; `pack_nextline()` breaks; `pack_wrap()` opts the run into auto-wrap at the line edge |

Flow cell sizing: a widget with a natural width (button, checkbox, text) shrinks to it and is
seated by `align()`; one without (slider, input) fills the track. `next_item_fit(u)` is a
one-shot per-item override (1.0 = stretch, 0.0 = natural). Auto-height rows take the FIRST
item's height for the whole row. Two more one-shots ride the same seam: `next_item_h(u)`
overrides the next item's height (resolved against the room left below the pen; 1.0 = fill the
rest of the region), and `next_item_align(a)` is align-self for one item (restored at the
following emit).

Modifiers are orthogonal to the template and persist across header installs: `align(a)`,
`field_split(side, label, control)` / `field_label_left(w)` / `form(side, label_w)` (labeled
value widgets become label-track + control-track rows), gaps. Only `layout_default()` resets
everything.

Other flow verbs: `same_line(spacing)` / `stack_same_line` (one-shot pen placement continuing
the previous item's line), `indent()/unindent()` (shift content column, reflow; flow only),
`empty(w,h)` (reserve a block, the Dummy analogue), `new_line`, `separator_text`.

Sizing: the `sz_` family is the one category that turns intent into a pixel dimension; layout
verbs consume what it produces. Grid-first, in order of preference: `sz_u(n)` (quanta to px),
`sz_rows_h(n)` / `sz_row_gap()` (box heights from row counts, scale-aware), `sz_scale_row(s)`
(a ramp step's row height without pushing it; `scale_push/scale_pop` scope a step). Content-fit
escape hatches: `sz_fit_row(content_h)` / `sz_fit_col(content_w)` (content px plus the standard
row / cell margin; `fit(0)` is the bare margin) and `sz_line_h()` (raw font line advance). Text
measurement lives with the draw family (`text_size`). Placement queries stay unprefixed:
`content_avail()`, `cursor_screen_pos()`.

### Containers

- `window_begin/window_end` -- persistent window (chrome, drag, dock, autosize); body is a region.
- `child_begin/child_end` -- scrollable child box inside the current layout.
- `region_begin/region_end` -- fixed caller-owned rect, no chrome; position is app-owned per frame.
- `push_layout_overlay( rect )` / `pop_layout` -- start a fresh layout frame over an arbitrary
  screen rect (no reservation). THE bridge from rect composition to real widgets.

## Regions, scroll, and clipping -- the invariants

These are load-bearing rules, learned the hard way (2026-07: the multiline editor first
hand-rolled its own scrollbar and broke hover arbitration, then sized itself to the content
column and seated itself under the window's scrollbar). A widget that follows them "just
works"; every exception listed is deliberate.

1. **Regions own everything scroll-shaped.** Gutter reservation, scrollbar emission, wheel
   claim (innermost-wins via `s_build.wheel_used`), scroll clamping, and both clips belong to
   `layout_push_region` / `layout_pop_region`. A widget NEVER calls `scrollbar_widget`,
   claims the wheel, or carves a gutter out of its own box. A widget that needs scrolling IS
   a child region (the listbox recipe: `child_begin` + content-sized cells); programmatic
   scrolling writes `lf()->scroll->scroll_y` (applied next frame, like the wheel).
2. **`f->view` is the single source of visible geometry.** The screen-space view rect (outer
   inset by the border, minus reserved gutters) is computed once at push. Draw clip,
   interaction clip, the content track (`view.w - pads`), the bar tracks (which sit exactly
   on `view`'s right/bottom edges), and the nav scroll chase all READ it. Never re-derive
   visible extents from `outer` -- drift between two derivations is how content ends up
   interacting under a scrollbar. (`GUI_DBG_REGION`, numpad 7, draws view/gutters/hit-clip
   per region to check this at a glance.)
3. **The interaction clip equals the view.** Both `layout_push_region` branches narrow
   `s_scope.clip` to `view` (intersected with the parent clip), so no body widget or child
   region can hover or press inside a gutter or under the title bar -- the bars hit-test
   after pop restores the parent clip. Bar-over-widget arbitration is therefore structural,
   not an emission-order accident.
4. **Two coordinate spaces; never mix them in one formula.** CANVAS values (pen, content
   column, highwater, every cell rect) carry the -scroll bias; SCREEN values (`outer`,
   `view`, `origin_*`, `band_bottom`, clips) do not. A width anchored canvas-point-to-screen-
   edge bakes the live scroll offset into the result (a box that stretches as you scroll).
   The space of every `layout_frame_t` field is tagged on the struct.
5. **Passive rows size to the content column; interactive surfaces size to the view.** The
   column (`content_avail`) can legitimately run wider than the view when a sibling
   overflowed -- text rows may ride it (the bar overpaints and out-claims them). An opaque
   interactive surface (child box, text editor, canvas that hit-tests) must not: size it with
   `view_avail()` (scroll-free, never exceeds the visible track). `child_begin( w <= 0 )`
   already defaults this way.
6. **Windows draw unclipped bodies on purpose; children scissor.** A window body pushes no
   draw clip (one clip, one batch; chrome drawn last overpaints anything scrolled under the
   title bar), so anything drawn after body content in a window must fully repaint the
   gutter/border it owns. A child passes `own_clip` and scissors to its view -- content
   inside it may safely overflow in either axis.

## Rect composition ("composer") -- pure math, nothing emitted, no state

Single-pass, known-size placement; the companion to the flow templates. All use the same
overloaded unit. Pair results with `push_layout_overlay` or `draw_*`.

- `content_rect()` -- pen + remaining space as one screen rect (the band to carve).
- `split( area, axis, sizes, gap, out[] )` -- carve one rect into panels along X or Y.
  Returns count (<= GUI_LAYOUT_COLS = 8 tracks per level). Recurse by splitting a result.
- `carve( form, area, gap, out[], max )` -- whole nested partition from ONE flat f32 form.
  Sentinels: `GUI_CUT_X`/`GUI_CUT_Y` open a nested list, `GUI_END` closes it. A size followed
  by a CUT is a container subdivided on that axis; otherwise a leaf. Form opens with a root
  CUT. Leaf rects stream to out[] in reading order. Example (sidebar + header/body/footer):
  ```c
  static const f32 FORM[] = { GUI_CUT_X, 80.0f, 1.0f, GUI_CUT_Y, 28.0f, 1.0f, 28.0f,
                              GUI_END, GUI_END };
  ```
- `anchor( parent, gui_anchor_t )` -- UE-Slate anchor frame: per axis, min == max point-pins a
  fixed size (with pivot + offset), min < max stretches between two fractions (offsets =
  margins). Behind `gui_rect_align` (center etc.) and `gui_anchor_box` (corner + pad).
- `split_begin/split_next/split_end` -- inline widget-level split of the current row.

Absolute-rect placement does NOT move the layout pen: after drawing a HUD/carved band, reserve
it with `gui()->empty( 0.0f, band.h )` so the window sizes around it.

## Interaction / ids / the user tier (user/)

- IDs: label-hashed; `"##hidden"` suffix hides label; `push_id_int/pop_id` for loops.
- Item queries after any emit: `is_item_hovered/active/clicked`; `want_capture_mouse` for
  app-vs-ui input arbitration.
- Custom behavior: `item( id, rect )` runs the shared interaction state machine over a
  caller rect and reports `hover/active/pressed/clicked` (`gui_item_state_t`);
  `invisible_button` is its click bit. A custom widget = rect + `item()` + `draw_*`.
- Custom draw: `canvas(h)` reserves a cell; `draw_rect/line/circle/text/draw_text_in` take
  caller rects, composing with split/carve/anchor. Colors are u32 ABGR (`GUI_COLOR(r,g,b,a)`).
- Style: `style_get()` + edit + `style_apply()`, or scoped `push_style_color/pop_style_color`.
- Drag-drop: `drag_source_begin` + `drag_payload_set` (emit preview widgets) /
  `drag_target_begin` + `drag_payload_accept`; payload copied by value.

## sb_gui sandbox specifics

`source/sandbox/gui/sb_gui/sb_gui.c` -- host exe: `mod_static` sys/ref/app/core/rhi/draw/gui, then the
boot + loop above. Demos map 1:1 to features: demo window (widgets, volatile pulse), font
browser (dev_font bake + `font_load/_into/use`), split panels (`carve` + `push_layout_overlay`),
HUD overlay (`anchor`/`gui_anchor_box`/`gui_rect_align`), region demo, drag-drop, tab groups
(`window_tab`/`dock_undock`). Debug hotkeys live inside gui when booted with `.debug = true`
(F1-F4 layers, F9 render view, F10 dashboard, P/O overlays, C retained skip, F force redraw,
I idle skip); host adds M = mem stats.

Build + run: `bin\build_tool.exe -config Debug -target sb_gui && bin\sb_gui.exe`.
Note: editing a unity-included .c/.h does not rebuild the lib -- touch the unit file
(`gui.c` / `gui_backend.c`) if the build skips.
