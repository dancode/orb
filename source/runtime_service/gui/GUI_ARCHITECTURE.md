# GUI Architecture + Layout/Composer Notes (AI-oriented)

Dense reference for working on the gui service (this directory) and its sandbox
`source/sandbox/gui/sb_gui.c`. Code is source of truth. ASCII only in all source.

## Big picture

Static lib `gui`, two translation units:

- `gui.c` (UI/core unit): context, id/state pool, input snapshot, layout engine, widgets,
  4_window/4_dock/4_popup/nav/table, frame lifecycle, mod vtable. Unity-includes its constituents in
  NUMBERED DEPENDENCY TIERS -- the directory listing is the stack, bottom-up (0_foundation/ +
  the three sibling 2_* roles -> 3_widgets/ -> 4_window/ -> 4_dock/ + 4_popup/; 4_table/ needs
  tiers 0-2 only; 5_user/ sits on top of everything; 1_surface/ is reserved for the window
  record/chrome carve; root files are the frame conductor, top alongside 5_user/). Include
  order matters; later files may reference statics from earlier ones.
- `gui_backend.c` (render unit): fonts, draw list, tessellation, GPU flush, debug overlay.
  UI unit calls it one-way through `gui_backend.h` (`draw_*`, `font_*`, `gui_render_*`).

## The composer / behavior / presentation split (two orthogonal groupings)

The directories group by DEPENDENCY; the API groups by ROLE. Three roles, one contract:

- **Composer** (`2_compose/`): the ONLY code that turns style spacing metrics
  (`line_size`/`gap`/`pad`/`grid_quantum`/`scales`) into geometry. Consumes spacing, produces
  rects. Public face: the layout verbs + the `sz_` sizing family.
- **Behavior** (`2_interact/`, public door `5_user/gui_behavior.c`): widget-agnostic
  interaction SERVICES over the 0_foundation/ utilities (identity `0_foundation/gui_id.c`,
  keyed state tracking `0_foundation/gui_state.c`, the io snapshot `0_foundation/gui_io.c`;
  the public readers over it are `5_user/gui_query.c`) -- the drag threshold machine + payload
  transfer (`gui_drag.c`), the edge-resize mechanism (`gui_resize.c`), animation stepping
  (`gui_anim.c`), and the standard item protocol (`gui_item.c`: `widget_behavior`, the default
  composition every stock widget runs). Each service knows a capability (exclusivity, clicks, tracking) over
  (id, rect); none knows a slider. Consumes finished rects, produces interaction state
  (`hover`/`active`/`pressed`/`clicked`). Style is invisible below this line -- a service never
  reads a metric or color to make a decision, and never paints: the system adornments
  (nav focus ring, drag accept ring, hot resize edges) are invoked from behavior at the
  protocol point but painted by present-tier helpers (`draw_nav_ring` / `draw_drop_ring` /
  `draw_resize_highlight` in `2_present/gui_widget_core.c`), so the paint policy lives with
  the skin.
- **Presentation** (`2_present/`: palette, widget macros, label grammar, text-fit, symbol
  draws): consumes rect + state + skin and paints; state is a parameter, it never asks
  behavior. `3_widgets/` and the 4_window/4_dock/4_popup chrome are its CLIENTS -- the stock
  widget set is written on the same substrate a user widget uses, not a privileged layer.

`5_user/` is the top tier and the public door onto the first two roles -- the caller's vocabulary,
pure verbs + readers with zero state or machinery: `canvas`/`split`/`carve`/`empty` for
rects, `item`/`invisible_button` for behavior, `draw_*`/`text_size` for your own presentation,
the bracketing stacks (`push_id`, item flags, `push_style_*`, `scale_push`, `disabled_begin`),
and the query readers (`want_capture_*`, `is_item_*`, `is_key_*`). Nothing below depends on it;
internal uses (combo's `push_id`, the overlay's key reads) deliberately dogfood the public
surface through gui_host.h declarations.

A custom widget (`game_ui_slider()`) never needs skin or spacing metrics -- it brings its own
look and composes with any layout.

`gui_style_t` mirrors the split as three labeled bundles (see gui.h): composer metrics / skin /
widget inner geometry. `widget_pad` is the one deliberate double-agent (region inset for the
composer, label inset for stock widgets).

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

### Standard manual loop (real hosts: runtime/host/host_main.c, sandbox/vulkan/sb_vulkan.c)

The host owns everything: it creates the app window + rhi context itself, then attaches gui to
them. `gui_boot.c` is NOT involved.

```
// setup (after app window + rhi context + swapchain exist)
gui()->init_config_front( caps );                 // optional feature gates, BEFORE init
gui()->init( font );                              // or GUI_FONT_NONE
gui()->set_frame_hooks( clock, sleep, wait );     // OS services gui cannot reach itself
gui()->debug_enable( true );                      // optional hotkey driver
gui_vp_t vp0 = gui()->viewport_open( win_id );    // attach gui to the EXISTING 4_window/ctx

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

host_main.c picks a render path per tick: render module owns the frame and gui composites
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

Mapping: `boot` = 4_window/ctx creation + init + hooks + `viewport_open`; `frame_poll` = event
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

## Layout engine (2_compose/gui_layout_core.c = mechanism, 2_compose/gui_layout.c = public verbs)

Model: every window body / region / child owns a `layout_frame_t` with a content box, a PEN
(`content_y`, flows downward) and a HIGHWATER (`content_max_x/y`, monotonic bounding box used
at region pop to decide scrollbars). `content_reach` moves both; only `layout_pen_jump` parts
them. Widgets never see the layout shape: each calls `widget_next_rect_w(natural_w, h)` and is
handed the next cell.

### The one overloaded unit rule (used EVERYWHERE: tracks, splits, fit, pack)

For any size value `t`:
- `t > 1`   : fixed pixels (never floored -- explicit px is authored intent)
- `t == 1`  : fill (equal share of leftover; multiple fills split it)
- `0 < t < 1`: fraction of the gap-adjusted extent
- `t == 0`  : natural (widget's own measure; zero in pre-divide track lists)
- `t < 0`   : unset/terminator (`GUI_END = -1.0f` ends track lists)

Two resolvers only: `layout_resolve_tracks` (lists) and `unit_resolve` (scalar). Flex/fraction
tracks floor at `WIDGET_MIN_W` and the row overflows into the clip; fixed px never floors.

### Modes -- a region opens UNDECLARED (GUI_MODE_NONE)

The first layout header names the mode. A widget emitted before ANY header is a usage error
(debug assert; release falls back to stack). Templates persist until replaced; no push/pop.

| Header verb | Mode | Behavior |
|---|---|---|
| `stack()` / `row(h)` | STACK | one flex column, rows accumulate + scroll (the default list) |
| `cols(tracks)` / `cols_n(n)` / `row2/3/4(...)` / `row_cols(h,tracks)` / `layout(desc)` | COLUMNS | repeating pre-divided column template; wraps row-major |
| `grid(desc)` / `grid_cells(nc,nr)` | GRID | fixed cols x rows matrix from pen to region bottom, both axes resolved up front; no scroll; overflow clamps to last cell |
| `bar()` / `strip()` / `pack(dir)` | PACK | print run: items at natural size along the axis; `pack_size(u)` overrides next item; `pack_nextline()` breaks |

Flow cell sizing: a widget with a natural width (button, checkbox, text) shrinks to it and is
seated by `align()`; one without (slider, input) fills the track. `next_item_fit(u)` is a
one-shot per-item override (1.0 = stretch, 0.0 = natural). Auto-height rows take the FIRST
item's height for the whole row.

Modifiers are orthogonal to the template and persist across header installs: `align(a)`,
`field_split(side, label, control)` / `field_label_left(w)` / `form(side, label_w)` (labeled
value widgets become label-track + control-track rows), gaps. Only `layout_default()` resets
everything; `pad(p)` re-insets the content box and clears the template back to UNDECLARED
(declare a header after it).

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

## Interaction / ids / the user tier (5_user/)

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

`source/sandbox/gui/sb_gui.c` -- host exe: `mod_static` sys/ref/app/core/rhi/draw/gui, then the
boot + loop above. Demos map 1:1 to features: demo window (widgets, volatile pulse), font
browser (dev_font bake + `font_load/_into/use`), split panels (`carve` + `push_layout_overlay`),
HUD overlay (`anchor`/`gui_anchor_box`/`gui_rect_align`), region demo, drag-drop, tab groups
(`window_tab`/`dock_undock`). Debug hotkeys live inside gui when booted with `.debug = true`
(F1-F4 layers, F9 render view, F10 dashboard, P/O overlays, C retained skip, F force redraw,
I idle skip); host adds M = mem stats.

Build + run: `bin\build_tool.exe -config Debug -target sb_gui && bin\sb_gui.exe`.
Note: editing a unity-included .c/.h does not rebuild the lib -- touch the unit file
(`gui.c` / `gui_backend.c`) if the build skips.
