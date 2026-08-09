# GUI Architecture

Dense reference for working on the gui service (this directory) and its sandbox
`source/sandbox/gui/sb_gui/sb_gui.c`. Code is source of truth. ASCII only in all source.

## Big picture -- two servers, one orchestrator

There are only three real things:

- **RENDER SERVER** (`render/`, unit `gui_render.c`): a 2d batch renderer with a narrow
  push-primitive surface any 2d utility can emit to -- draw list, tessellation, retained
  geometry cache, the two resource atlases, GPU flush.  The atlases split by what a texel MEANS,
  since that is what the fragment shader branches on: R8 COVERAGE (glyphs, icons, assists -- the
  vertex colour paints it, sampled NEAREST so text stays crisp) and RGBA SPRITE (authored art --
  the vertex colour tints it, sampled LINEAR, created lazily on the first registration). Knows nothing of ids-as-identity, interact
  state, style, or layout. Renders from an atlas that is PUSHED to it; it does not know what
  a font is (the glyph/sprite source contract in `render/gui_render.h` is implemented by the
  draw unit).  Rounded shapes are not tessellated: the vertex carries an SDF coordinate and a
  packed mode (the EFFECT BAND, `gui_draw_vert_t` in gui.h) and the fragment resolves the
  boundary, so a rounded fill / frame / shadow is four quads with exact edges.  The mode rides
  the vertex rather than a push constant precisely so an effect can never split a batch.  The
  band's one frame-constant input, the clock, goes the other way: `pc.time` (wrapped seconds,
  fed by `gui_render_set_time` from frame_begin) is written once per surface, so a time-driven
  effect re-emits no geometry and adds no draw call.  GUI_FX_PULSE is the first mode to read it.
- **INTERACT SERVER** (`core/`, unit `gui_core.c`): io routing + dedicated retained-mode
  storage -- the id namespace, the keyed state pool, the ambient interaction record, the item
  protocol, the pane/z contest, anim utilities. ALL retained record types live in its storage
  header `core/gui_ctx.h` (window, nav state, viewport, scroll link, the context aggregate);
  the two records only chrome reads (popup entry, dock node) stay shaped in chrome behind
  forward declarations. Knows nothing of style, themes, or drawing.
- **FRAME ORCHESTRATOR** (`frame/` + root `gui_frame.c`): boots both servers, owns
  viewports/app/sys wiring, pumps io into the interact server, hands each surface's GPU
  pieces (vb/ib/target) to the render server at flush, and allocates the context blocks (it
  alone sees every record's size). Owns NO unit header -- its public face is gui.h /
  gui_api.h / gui_host.h.
- **MODULE FACE** (root `gui.c`): the separate, logic-free unit that assembles the module
  vtable (`g_gui_api_struct`) + `mod_desc_t`, and defines the app/rhi API pointer storage the
  module init fetches. It carries no orchestration -- only the module's public identity.

The two servers NEVER see each other. Everything else is a LIBRARY over them:

    unit .c (root)   folder      owns header                     role
    --------------   ---------   -----------------------------   --------------------------------
    gui_rect.c       rect/       rect/gui_rect.h                 leaf: geometry + color + GUI_WARN_ONCE
    gui_render.c     render/     render/gui_render.h             RENDER SERVER
    gui_draw.c       draw/       draw/gui_draw.h                 drawing routines + font/icon/sprite resources
    gui_core.c       core/       core/gui_core.h + gui_ctx.h     INTERACT SERVER (services + storage)
    gui_style.c      style/      style/gui_style.h               state flags in, colors/faces/metrics out
    gui_interact.c   interact/   interact/gui_interact.h         gesture mechanisms (move/resize/drag/feat)
    gui_flow.c       flow/       flow/gui_flow.h                 layout: THE rect producer
    gui_component.c  component/  component/gui_component_internal.h  widget LOGIC, no paint
    gui_stock.c      stock/      the style grid (gui.h) +        reference widget set (stock_* renders),
                                 stock/gui_stock_internal.h      astride both servers
    gui_chrome.c     chrome/     chrome/gui_chrome.h             product windowing policy (6 folders)
    gui_debug.c      debug/      debug/gui_debug.h               server introspection (severable)
    gui_frame.c      frame/      (public headers)                FRAME ORCHESTRATOR
    gui.c            (root)      (public headers)                MODULE FACE (vtable + mod_desc)

The public face (gui.h / gui_api.h / gui_host.h) reads in strata BANDS, one per section
banner; band -> implementing unit:

    GUI_FRAME   -> frame/          GUI_FLOW    -> flow/
    GUI_DRAW    -> render/ + draw/  GUI_STYLE   -> style/
    GUI_CORE    -> core/ + interact/ GUI_STOCK   -> component/ + stock/
    GUI_SURFACE -> core/gui_surface.c + flow/gui_region.c + interact/gui_feature.c
    GUI_RECT    -> rect/            GUI_CHROME  -> chrome/     GUI_DEBUG -> debug/

The GUI_STOCK band covers both rungs at once: the comp_* logic cores (component/) and the
stock_* renders over them (stock/), paired one to one in the header so the two are read together.

Dependency graph (lowest to highest) -- each unit root .c includes EXACTLY the unit
headers at or below its layer, so the include list at the top of each unit IS the graph and
the compiler enforces it per unit:

    rect      -> base only
    render    -> rect                 never sees core/style/ids
    draw      -> render, rect         parameter-pure (no style/core reads)
    core      -> rect                 never sees render/draw/style
    style     -> core, rect           resolves, never emits
    interact  -> core, style, rect    (style = the WIN_BORDER metric read only); never paints
    flow      -> style, draw, core    + the render CLIP STACK (flow computes THE view rect,
                                      so it owns the region scissor -- flow places, never paints)
    component -> core, interact       widget LOGIC only: (id,rect) in, outputs out, NEVER paints,
                                      so it stops below draw/render (see the tier note below)
    stock     -> everything below     the reference widget set -- astride both servers (it paints)
    chrome    -> everything below     + the render run capture (text selection)
    debug     -> everything           severable; its header leads every unit (it computes the
                                      Debug-build switches, the one sanctioned above-layer include)
    frame     -> orchestrates all

Upward calls stay explicit and few, and each is DECLARED in its lowest consumer's header
(higher consumers see it through the stack): core -> `draw_nav_ring`, `nav_scroll_chase`,
`gui_owned_window_event`; draw -> `label_vis_len`, `cell_next(_w)` (canvas placement); flow ->
`scrollbar_widget`, the child box paint trio; render -> the glyph/sprite source contract +
`draw_unit_mem_bytes`; frame steps (chrome's window/popup/nav/dock upkeep) are declared in
`chrome/gui_chrome.h`. Do not add more.

Every unit ends with a `<unit>_unit_mem_bytes()` seam; `gui_ui_mem.c` (frame) aggregates.

## Widget tiers -- component / stock / chrome

The stack has zero one-size-fits-all widgets on purpose. A widget's presentation is USER-driven;
only its logic is shared. Four rungs, each a client of the one below:

    state/interact  ->  component  ->  stock  ->  chrome
     (services)        (logic)       (reference)  (product)

- **component** (`component/`, `gui_comp_*`): a widget's LOGIC with no look. Consumes an
  (id, rect) and does the tedious part -- hit-testing, drag math, value snapping, focus /
  hover / active state -- then reports clear outputs. NEVER paints, so it stops below the
  draw/render servers. A component does not exist on screen; it is a utility front-end a
  widget composes onto. This is the engine's own idiom (world/entity/**component**/actor): a
  widget is an actor, its components are logic aspects that do not independently exist.
- **stock** (`stock/`, the `stock_*` renders): the reference widget set = a component's logic +
  one plain render. It is what a user READS AND FORKS, not a privileged default.
- **chrome** (`chrome/`): the product -- the editor's window/dock/popup/table framework and
  its default-look widgets.
- **user widget** (app-side, e.g. `my_game_slider`): a component + the game's own art. It is a
  SIBLING of the matching stock widget -- same logic, different presentation: the same slider
  logic drives the editor's flat handle and a game's diamond-and-art handle, and neither is
  favored.

One naming vocabulary covers all three tiers: chrome, stock, and a user widget name a color the
same way, through `style_col` inside the library and `gui()->style_color` outside it, over
`GUI_ROLE_*` x `GUI_PHASE_*`. Each `gui_stock_*` render sits over a `gui_comp_*` logic core; both
are public (`gui_host.h` / the vtable), and a user widget is the stock render's sibling over the
same `comp_*` call.

CALL shape: every component opens `(id, rect, ...)` -- logic first, so the identity that keyed the
state leads. A parameter-rich one ADDS an `_ex` desc twin, never replaces the positional form
(`comp_slider` / `comp_slider_ex`). Every `stock_*` RENDER opens `(rect, ...)` instead -- the rect
leads because a render is a rect consumer and the inert three (`stock_panel` / `stock_label` /
`stock_meter`) have no id at all. The two orders are the two tiers, not an oversight: reading
`(id, rect)` tells you you are calling logic, `(rect, id)` that you are calling paint.

NAMING -- three string parameters, three spellings, no overlap, so one glance at a signature says
what the string is for:

    id_str    identity only, never drawn (`gui_id_t id` is the hashed value it produces)
    label     a widget's displayed NAME, which doubles as its identity via the `##` grammar
    str       a run of content the caller wants painted (`text`, `draw_text`, `tooltip`)

RESULT shape: `gui_item_state_t state` FIRST (so it is at offset 0 for every component), then any
geometry, then only the outcomes `state` does not already carry -- never a second spelling of
`state.clicked`.

- `comp_slider` -- `(id, rect, v, lo, hi)` -> `{state, frac, fill, handle, changed}`;
  `comp_slider_ex` takes `gui_comp_slider_desc_t` (snap step, handle width, nav step).
- `comp_button` -- the simplest. `(id, rect)` -> `{state}`; the outcome IS `state.clicked`, so it
  adds no field.
- `comp_check` -- `(id, rect, bool* v)` -> `{state, box, changed}` (inscribed box).
- `comp_cycle` -- COMPOSES `comp_button` for each cap -> `{state, prev, next, prev_box, next_box,
  label, changed}`; `state` is the whole stepper, `prev`/`next` the per-cap faces. Takes `count`
  for wrap, not the strings (the render draws those).
- `comp_selectable` -- `comp_button` + optional `*selected` toggle -> `{state}`.
- `comp_input` -- runs the interact/ edit engine (`edit_field`) and returns PAINTABLE geometry
  (content rect, run origin, selection + caret bars) so a render draws a text field with only
  public verbs, never touching `gui_edit_state_t` or measuring a glyph.

The two verbs that make a user widget a real sibling, both public: `gui()->item_phase( state )`
distils an interact state into a style PHASE (ACTIVE / HOT / IDLE, nav counting as HOT) and
`gui()->style_color( role, phase )` is the RESOLVED grid read -- the same seam the stock renders
and chrome's `COL_*` macros use, so `push_style_color` reaches a user widget exactly as it
reaches a stock one. Reading `style_edit()->col[][]` at paint time instead bypasses the stack.

No component: `stock_panel` / `stock_label` / `stock_meter` are inert paint -- no interaction, no
logic to extract -- so they stay render-only. Not every widget needs one. `sb_gui_base` tier 3
shows the slider and button each with a stock render beside a custom render over one `comp_*`
call. `chrome/` has its own bespoke widgets and makes no `comp_*` calls; the components its
widgets would need do not exist yet (`comp_drag` for the relative-drag family, `comp_scrollbar`,
`comp_tab`).

## The composer / behavior / presentation split

Three roles, one contract (the units carry the same names):

- **Composer** (`flow/`): the ONLY code that POSITIONS rects -- divides regions into
  cells, moves the pen, decides where the next widget lands. Widgets MEASURE themselves with
  the same METRICS vocabulary (a button's natural width is its label plus pad) but only ever
  REQUEST a size through `cell_next_w`; the composer decides placement. Composes and
  never paints (the region scissor lifecycle is its one render crossing). Public face: the
  layout verbs + the `sz_` sizing family.
- **Behavior** (`interact/` over the `core/` services): widget-agnostic interaction SERVICES
  (identity `core/gui_id.c`, keyed state `core/gui_state.c`, the io snapshot `core/gui_io.c`;
  the public readers are `core/gui_query.c`) -- the drag threshold machine + payload transfer
  (`gui_drag.c`), the move-drag protocol + deferred-press latch (`gui_move.c`), the
  edge-resize mechanism (`gui_resize.c`), and the standard item protocol (`core/gui_item.c`:
  `item_state`, plus `item_grab` for hot chrome that is not a widget; the compound-widget
  bracket `gui_item_sub_begin/end` and its full form `gui_item_sub_layout_begin`; the two
  steps big enough to own a file sit beside it -- keyboard-focus policy `core/gui_focus.c`
  and the nav registration seam `core/gui_nav_item.c`). Each
  service knows a capability (exclusivity, clicks, tracking) over (id, rect); none knows a
  slider. This tier plus core are the ONLY writers of the `s_interaction` arbitration fields:
  higher tiers claim through the core verbs (`interact_claim`, `interact_hover_fence`) and
  read the record for gating, never write it raw.
  Behavior's only inputs beyond (id, rect) are the interaction scope (`s_scope`,
  `core/gui_core.h`): the owner window, the interaction clip, the chrome hover suppression,
  and the per-item flag/nav stamps -- placed there by composition at its seams. Behavior never
  reads the composer scratch (`s_build`); the scope record IS the composition->behavior
  contract, and behavior publishes its per-item result back into it (`s_scope.last_*`).
  Style is invisible below this line (the one metric interact reads is `WIN_BORDER`, because
  the resize band straddles the border, and border is geometry): the system adornments (nav
  focus ring, drag accept ring, hot resize edges) are invoked from behavior at the protocol
  point but painted by stock-unit helpers (`draw_nav_ring` / `draw_drop_ring` /
  `draw_resize_highlight` in `stock/gui_adornment.c`), so the paint policy lives with the
  skin.
- **Presentation** (`stock/`: the `stock_*` rect-consuming renders, label paint +
  self-measurement (`label_natural_w`), per-item ambient wrappers, system adornments, the
  styled symbol half): consumes rect + state + skin and paints; state is a parameter, it
  never asks behavior. The widget paint floor is rect-taking (`draw_fill( r, col )` /
  `draw_outline( r, t, col )` in `draw/gui_paint.c`, plus the parameter-pure `draw_*` symbol
  palette in `draw/gui_symbol.c`): widgets speak rects; only the render server emit layer
  (`draw_push_*`) speaks scalar x/y/w/h with UV + texture arguments.

  That floor is WIDENED by one verb: `draw_fill_brush( r, brush )` fills a rect with a
  `gui_brush_t` (gui.h) -- solid, gradient, sprite, nine-slice -- instead of a bare colour, and
  `draw_fill` is its SOLID case. The widening is what stops the palette growing a verb per fill
  kind: a brush is a plain descriptor, so a face can live in a caller's own theme and be handed
  to a widget written before that fill kind existed. A NINE brush expands to up to nine quads at
  TESSELLATION time (`tess_sprite`), holding its authored corners at any destination size, and
  the whole frame stays ONE command in ONE batch. Public face: `gui()->draw_brush` over the
  sprite registry (`register_sprite` / `load_sprite` / `sprite_set_slice`, `draw/gui_sprite.c`).

  Where a widget's surface COMES FROM is then the FACE plane (`stock/gui_face.c`): a brush
  installed on a `(look, role, phase)` cell -- the same coordinate the colour grid uses, in a
  parallel run -- which replaces that cell's flat fill. Every surface fill in stock and chrome
  paints through `draw_face*` rather than `draw_fill`, so a theme that installs faces restyles the
  whole widget set with none of it edited; a cell with no face (0, the default) falls straight
  through to its colour. A face SUPPRESSES the border its cell would have been given -- authored
  art carries its own edge -- which is why the painters take the border and the caller does not
  draw it. Each painter mirrors one colour projection (`col_item_bg` -> `draw_face_item`, and so
  on) so a site converts by changing one call.

  WHEN a widget's surface changes is the MIX (`gui_style_mix_t`, `style_mix`): a CONTINUOUS
  coordinate over the same color grid, since phase and look are enumerations and cannot express
  "most of the way to hovered" on their own. Three weights (toward HOT, toward ACTIVE, toward
  SELECT) are damped in one `gui_anim4` slot. The read is split from the spend, and that split is
  what makes motion affordable everywhere: `style_mix` is the one call that touches storage, and
  a widget reads it ONCE and spends it on every row it paints (`style_col_mix` for the surface,
  the border, the ink), so a three-part widget costs one probe and its parts arrive together.
  Storage stays proportional to items IN MOTION -- the weights rest at zero, so a settled
  widget's slot evicts and an idle UI holds nothing. `GUI_ID_NONE` opts out with no probe at all.
  The three rates are style vars (`GUI_VAR_ANIM_HOT` / `_ACTIVE` / `_SELECT`, class
  `GUI_CLASS_RATE`), so a theme owns the feel of the entire widget set; setting them to 0 makes
  the library snap, with no separate animation code path. Faces CROSS-FADE rather than blend,
  since art does not interpolate: `face_span` collapses the mix onto the two cells the item lies
  between and alpha-composites them.
- MEASURED extents are a different shape of motion and use a different damper. A natural column
  width or a box height is resolved from LAST frame's measure (the engine is single-pass), so a
  change is one wrong frame followed by a snap -- which reads as a glitch, not as motion.
  `GUI_VAR_ANIM_SIZE` eases them through `gui_anim_track`, which unlike `gui_anim_f32` ALWAYS
  stamps its slot: a value that sits still between changes would otherwise have its history
  evicted and snap anyway. It adopts on first sight, so nothing grows in from zero.

A widget (the chrome unit) is the only combiner: it asks composition for a rect, hands it to
behavior, hands both results to presentation. The public `gui_item`/`canvas`/`draw_*` verbs
are the caller's door onto the same roles, skin optional -- the game-UI path.

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
3. A shared pure utility moves to rect/gui_rect.h (the leaf kit) on its SECOND user;
   single-file utils (`fnv1a`, `rect_empty`) stay local.
4. The door is at the bottom: pure leaf helpers first, mechanism next (bottom-up), the file's
   seam / public face last (`cell_next_w` ends gui_layout_core.c, `window_begin_ex` ends
   gui_window_free.c).
5. `gui_` marks the CALLER's door, nothing else. A seam that crosses units but never leaves the
   system keeps its family prefix (`build_*`, `backend_*`, `step_*`, `dash_*`, `dbg_*`,
   `volatile_*`, `replay_scope_*`) so a name tells you at a glance whether a host could call it.
   The exceptions are the core services a widget author uses through gui_host.h anyway
   (`gui_state_*`, `gui_anim*`, `gui_item*`), which are public and named so.

The canonical leaf widget is four lines, one per seam:

```c
gui_id_t         id = item_id( label );
gui_rect_t       r  = cell_next_w( label_natural_w( label ), WIDGET_H );
gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
draw_face_item( r, id, st, false );   /* surface: face if the theme has one, mixed colour if not */
```

The style vocabulary itself (`WIDGET_*` / `WIN_*` / `COL_*` macros) lives with its resolver in
the style unit (`style/gui_style_core.c`) since all three roles read it. `chrome/` is its
CLIENT -- the chrome (and stock) widget set is written on the same substrate a user widget
uses, not a privileged layer. The caller's vocabulary lives with its machinery -- canvas in draw, query
readers in core, gesture verbs in interact, `gui_item` with the item protocol in core -- and internal uses
deliberately dogfood the public surface through gui_host.h.

A custom widget (`game_ui_slider()`) never needs skin or spacing metrics -- it brings its own
look and composes with any layout.

`gui_style_t` sorts every field into two bundles by one test -- can a read of this field move a
rect? (see gui.h): METRICS (row heights, gaps, pads, border, title bar, indicator sides, the
scrollbar gutter -- consumed by the composer to size cells AND by widgets to measure/seat
themselves, deliberately the same numbers) and SKIN (colors, roundings, mark shapes, caret --
paint-only, provably never sizes a cell).

Consumers call through the module vtable: `gui()->verb(...)`. gui links only `app` + `rhi`;
OS services it cannot reach (clock/sleep/wait) are injected via `frame_set_hooks`
(done for you by the test-bed `boot()` path).

Immediate mode with a retained twist: widgets re-emit every frame, but geometry is cached per
window slot and a clean frame (identical command hash, no input) skips emit entirely
(`set_retained_skip`, `frame_begin` returning false). Each text command carries its own font id
(`push_font`/`pop_font` scope per run); the backend re-activates it at deferred tessellation, so
mixing fonts in one window costs extra text commands, never a batch or segment split.

## Frame lifecycle -- two tiers

The core lifecycle is always the same sequence; what differs is who owns the window, the rhi
context, and the render/present step.

### Standard manual loop (real hosts: runtime/run_host.c, sandbox/rhi/sb_vulkan/sb_vulkan.c)

The host owns everything: it creates the app window + rhi context itself, then attaches gui to
them. `gui_boot.c` is NOT involved.

```
// setup (after app window + rhi context + swapchain exist)
gui()->init( font );                              // or GUI_FONT_NONE
gui()->frame_set_hooks( clock, sleep, wait );     // OS services gui cannot reach itself
gui()->debug_enable( true );                      // optional hotkey driver
i32 vp0 = gui()->viewport_open( win_id );         // attach gui to the EXISTING window/ctx

// per frame
while ( app pump )                                // host pumps OS events itself
{
    // events: rhi()->event() first (swapchain resize), then gui()->event( &ev );
    //         each answers app_event_result_t -- stop routing at APP_EVENT_CONSUMED (input,
    //         floater lifecycle); SHARED/PASS keep going, so leftovers reach the app
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
    // pacing is host policy: this path writes its own (run_host's editor_sleep schedules
    // against a deadline, gating on wants_redraw / frame_dirty / volatile_live)
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
while ( gui()->boot_poll( &dt ) )       // pumps OS, routes rhi + gui events; false = quit
{
    if ( gui()->frame_begin( dt ) ) { gui()->ctx_begin(...); ...emit...; gui()->ctx_end(); }
    gui()->frame_end();
    gui()->boot_present_begin( NULL );  // opens main surface frame; bool gates host passes
    gui()->boot_present_end();          // gui()->render + present + render floaters
    gui()->boot_pace( 4, 16 );          // this loop's pacing (idle-skip aware)
}
gui()->shutdown();                      // also tears down the boot window + context
```

Mapping: `boot` = window/ctx creation + init + hooks + `viewport_open`; `boot_poll` = event
pump + routing; `boot_present_begin/boot_present_end` = `rhi frame_begin` + clear +
`gui()->render` + present + `viewport_render_floaters`; `boot_pace` = the sleep this loop
would otherwise hand-roll. A host that wants a different cadence simply does not call it.

Invariants:
- THE BEGIN / END RULE, one rule for every pair: the bool gates the BODY, never the end call.
  Every `*_end` is safe to call whatever its begin returned -- it unwinds exactly what that
  begin opened and nothing when it opened nothing, so there is no per-pair contract to
  memorize. Guarding an end on the bool is what strands an overlay detach (popup), an id scope
  (tab item), or a window record when a begin opens state and then reports a body it does not
  want. `window_begin` false means collapsed OR closed; distinguish a real close with
  `window_is_open( title )` before clearing a host `p_open` flag.
- Any state mutation done outside the emitted build (or at pop time) must not depend on
  re-emit happening next frame -- clean-frame skip will freeze it unless something sets
  wants_redraw (input, animation, `volatile_cb`).
- `volatile_cb` blocks keep animating on skipped frames but MUST keep a fixed layout
  footprint (constant size; pad printf fields).
- The lifecycle above is CHECKED, not just documented: `init` refuses to run without a live rhi
  context (and refuses a second init), `viewport_open` refuses a window with no rhi context or a
  slot already open, and `ctx_begin` / `viewport_update` / `render` report -- once per call site,
  in every build, with the rule they broke -- a build with no active font, an emit outside
  `frame_begin`/`frame_end`, a `viewport_update` inside the build, a render before the draw list
  is sealed, a floater left waiting to be freed, and a viewport whose size disagrees with its
  swapchain (a resize that reached `gui()->event` but not `rhi()->event`, or the reverse).

## Layout engine (flow/gui_layout_core.c = mechanism, flow/gui_layout.c = public verbs)

Model: every window body / region / child owns a `layout_frame_t` with a content box, a PEN
(`pen_y`, flows downward) and a HIGHWATER (`high_x/high_y`, monotonic bounding box used at
region pop to decide scrollbars). `content_reach` moves both; only `layout_pen_jump` parts
them. The frame groups its other state by lifetime: `tmpl` (the installed shape), `mod`
(orthogonal modifiers), `line` (iteration cursor + the open line, re-zeroed per install).
Widgets never see the layout shape: each calls `cell_next_w(natural_w, h)` and is
handed the next cell.

### The one overloaded unit rule (used EVERYWHERE: tracks, splits, fit, pack)

Its canonical statement is the `GUI_FLOW -- THE OVERLOADED UNIT` banner in `gui.h`; every other
mention in the tree points there rather than restating it. In brief, for any size value `t`:
- `t > 1`   : fixed pixels (never floored -- explicit px is authored intent)
- `t == 1`  : fill (equal share of leftover; multiple fills split it)
- `0 < t < 1`: fraction of the gap-adjusted extent
- `t == 0`  : natural (widget's own measure).  A pre-divided COLUMN track resolves it from
  measured feedback: the widest natural item placed in that column last frame (one-frame lag,
  floored at `WIDGET_MIN_W`); grid ROW tracks and the pure-math callers (field_split, split,
  carve) have no measure and it collapses to zero there
- `t < 0`   : unset/terminator (`GUI_END = -1.0f` ends track lists)

Two resolvers only: `layout_tracks_resolve` (lists) and `unit_resolve` (scalar). Flex/fraction
tracks floor at `WIDGET_MIN_W` and the row overflows into the clip; fixed px never floors.

### Modes -- a region opens UNDECLARED (GUI_MODE_NONE)

The first layout header names the mode. A widget emitted before ANY header is a usage error
(debug assert; release falls back to stack). Templates persist until replaced; no push/pop.

| Header verb | Mode | Behavior |
|---|---|---|
| `stack()` / `row(h)` | STACK | one flex column, rows accumulate + scroll (the default list) |
| `cols(tracks)` / `cols_n(n)` / `row2/3/4(...)` / `row_cols(h,tracks)` | COLUMNS | repeating pre-divided column template; wraps row-major |
| `grid(gui_grid_t)` / `grid_cells(nc,nr)` | GRID | fixed cols x rows matrix from pen to region bottom, both axes resolved up front; no scroll; overflow clamps to last cell |
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
`empty(w,h)` (reserve a block, the Dummy analogue), and the two PAINTLESS cell spacers
`skip()` / `new_line(h)`. The rules that draw into a cell -- `separator` / `separator_text` --
are chrome's over the same `cell_next`; the split is paint, not placement.

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
- `box_begin( label, role )` / `box_end` -- NOT a container: a DECORATOR. No region, no clip, no
  scroll -- it insets the content column by one pad, paints `role` behind whatever is emitted,
  and gives the column back. Reach for it wherever a card behind a few widgets would otherwise
  have meant opening a `child_begin` for the rect alone. Its height comes from last frame's
  measure (the surface must be pushed before its content), eased by `GUI_VAR_ANIM_SIZE`; the
  layout always reserves max(painted, measured), so nothing below is ever drawn over. Stack and
  columns only.

## Regions, scroll, and clipping -- the invariants

These are load-bearing rules. A widget that follows them "just works"; every exception listed
is deliberate.

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
4. **One glass space, two anchors; never subtract across them to get a size.** CANVAS values
   (pen, content column, highwater, every cell rect) are content-anchored -- they carry the
   -scroll bias and slide as the region scrolls. SCREEN values (`outer`, `view`, `origin_*`,
   `band_bottom`, clips) are frame-anchored. Both are absolute positions on the same glass,
   which is why a cell rect draws and hit-tests with no conversion, and why crossing anchors
   to *compare* is correct and everywhere (pen vs `band_bottom` = "has content reached the
   visible band end?"). Adding a scroll-free *size* to either anchor is fine too
   (`content_x + ( view.w - pads )`). The single illegal operation is **subtracting two
   differently anchored positions to obtain a size or extent** -- the live scroll offset lands
   in the result, so it is right at scroll 0 and wrong by exactly the scroll after that (a box
   that stretches as you scroll; a panel that measures short). Both legitimate crossings live
   behind one seam in `flow/gui_layout_core.c`: `canv_from_scr_*` applies the bias (the pen
   seed), `content_extent_x/y` cancels it (the pop measure, the split-panel measure). A bare
   `+/- scroll_x/y` in a formula anywhere else is the bug, not the fix. The anchor of every
   `layout_frame_t` field is tagged on the struct.
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

## Interaction / ids / the public verbs

- IDs: label-hashed; `"##hidden"` suffix hides label; `push_id_int/pop_id` for loops.
- Item queries after any emit: `is_item_hovered/active/clicked`; `want_capture_mouse` for
  app-vs-ui input arbitration.
- Custom behavior: `item( id, rect )` runs the shared interaction state machine over a
  caller rect and reports `hover/active/pressed/clicked` (`gui_item_state_t`);
  `invisible_button` is its click bit. A custom widget = rect + `item()` + `draw_*`.
- Custom draw: `canvas(h)` reserves a cell; `draw_rect/line/circle/text/draw_text_in` take
  caller rects, composing with split/carve/anchor. Colors are u32 ABGR (`GUI_COLOR(r,g,b,a)`).
  `draw_text_xf` is the same run scaled and rotated about its anchor -- the game-UI door. What it
  looks like is the FONT's doing: a coverage bake magnifies its texels, a distance-field bake
  (`font_tool -sdf`) resolves its edge in the fragment and is indifferent to scale and angle.
- Style: `style_get()` + edit + `style_apply()`, or scoped `push_style_color/pop_style_color`.
  `push_style_face` is the same verb over the FACE plane (art on a cell, see the presentation
  note above); `style_brush_add` registers a brush in the set's pool and must be called from the
  set's SOURCE, since the pool is cleared at every landing.
- Motion: `style_mix( id, st, selected )` -> three damped weights; `style_color_mix( role, mix )`
  and `draw_face_mix( rect, role, mix )` spend them. Read the mix once per item and spend it on
  every row that item paints. `GUI_VAR_ANIM_HOT` / `_ACTIVE` / `_SELECT` set the rates (Hz;
  0 = snap) for the whole widget set.
  A kit that owns the whole application's look registers `style_source_set(fn, user)` on the
  default set; the source is invoked at every style landing (font / theme / scale) to re-install
  `style_edit()`, seeded from the chrome theme first so it need only overwrite what it owns.
  A kit that wants its look BESIDE chrome's takes a set of its own -- `style_set_create(fn,
  user)` once, then `style_set_push/pop` around its UI. Both looks stay installed; neither
  clobbers the other.
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
Note: editing a unity-included .c/.h does not rebuild the lib -- touch the owning unit root
(`gui.c`, `gui_core.c`, `gui_render.c`, ...) if the build skips.
