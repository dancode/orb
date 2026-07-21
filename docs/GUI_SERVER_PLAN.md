# GUI SERVER PLAN -- the v2 unit reorganization

Status: R1 (rect) + R1b (header split) + R2 (render) + R3 (draw) DONE 2026-07-21 -- next: R4 (core)

R3 DONE: the GUI_DRAW unit exists (root gui_draw.c).  Moved in: draw/gui_paint.c (the paint
floor + fitted text painters, split out of present/gui_paint_core.c), draw/gui_symbol.c
(whole palette; its five style-read sites are banner-marked for the R8 parameterization),
draw/gui_canvas.c, and the FONT + ICON resources (draw/gui_font.h/.c/_internal.c,
gui_icon.c/_load.c) out of render/resource/.  The server side now speaks the GLYPH/SPRITE
SOURCE CONTRACT (render/gui_render.h): font_use / font_active_id / font_valid / font_glyph +
icon_get -- implemented by draw over tables beside the atlas; the old font_atlas_idx /
font_white_uv / font_dash_v / font_atlas_bytes / icon_atlas_idx delegates died (render call
sites retargeted to res_atlas_* directly).  Fonts/icons push into the atlas through the
res_atlas_* API (gui_res_atlas.h).  Lifecycle re-seated per the model: gui_backend_init
stands up pipeline + atlas only; the orchestrator (frame/gui_frame.c) then calls the NEW
gui_draw_boot( icons ) (font_init + optional icon layer + builtins), and gui_draw_shutdown()
tears down before gui_backend_exit -- font/icon boot left gui_submit.c and the render root.
Memory accounting: NEW gui_draw_unit_mem_bytes() (fonts registry + reload queue + icon
tables); render's gui_backend_memory fills its font bucket through that seam and keeps only
sizeof(s_res) + res_atlas_bytes().  paint_core is now grammar (->core R4) + state->color
projections (style material) + styled adornments (->element R8) only.  Full build clean,
both canaries clean.  Still deferred: the gui_backend_* lifecycle identifier rename.

R2 DONE: backend/ -> render/ wholesale (git mv; font/icon stay in render/resource/ until R3
claims them for draw); gui_backend.c -> root unit gui_render.c; gui_backend.h ->
render/gui_render.h; pipeline/gui_render.c -> pipeline/gui_submit.c (freed the unit name --
it is the RENDER-phase upload+submit file); gui_backend_mem.c -> gui_render_mem.c.  All
include paths + comment path references swept repo-wide (incl. sb_vulkan_stress's
gui_shader.h include); orb.targets unit lines updated.  KEPT for now: the gui_backend_*
identifiers (gui_backend_init/exit/memory, gui_backend_caps_t) -- gui_render_init already
names the GPU-resource init in gui_submit.c, so the lifecycle rename needs a deliberate
naming pass (candidate: gui_render_boot/shutdown at R3 when the unit's contents settle).
Full build clean; both canaries clean.

R1b DONE: gui_internal.h is now a pure UMBRELLA -- one header per unit, included in stack
order, so the include list IS the dependency graph.  New headers: core/gui_core.h (server
primitives: caps, io, interaction, scope, state pool, item protocol, pane/z contest, anim
utilities), style/gui_style.h (element bridge, vocabulary macros, lattice, state->color
projections), draw/gui_draw.h (draw scope record + the paint vocabulary), interact/
gui_interact.h (resize/move/drag/select/feat mechanisms), flow/gui_flow.h (scroll link +
layout types + emit surface), chrome/gui_chrome.h (window/nav/popup/dock records, frame
steps, route seam), frame/gui_frame.h (gui_viewport_t + caps), core/gui_ctx.h (the
gui_context_t aggregate + build scratch -- closes the stack; the known entanglement point),
debug/gui_debug.h.  Shared stateless inlines (saturate/clampf/rect_intersect) moved to
rect/gui_rect.h.  Misplaced entries are marked in place with the increment that moves them.
Full build clean first try; canary clean.

R1 DONE: rect/ folder created; gui_rect.h moved to rect/gui_rect.h (includers gui.h +
gui_element.h repointed); NEW rect/gui_rect_core.c holds the compiled half (col_lerp,
align_x/align_y, rect_align -- pulled from present/gui_paint_core.c, align_y un-static'd
for its canvas consumer); NEW root unit gui_rect.c; decls moved from gui_internal.h to
rect/gui_rect.h; orb.targets gui + gui_stress gained `unit gui_rect.c`.  Full Debug build
green, sb_gui_example canary clean.

Predecessor: docs/GUI_STACK_PLAN.md (increments 1-10, DONE) carved the monolith into six
translation units and surfaced ~120 seams into gui_internal.h.  That proved where the seams
ARE.  This campaign re-seats the code into its final shape: the six units still wrap the OLD
folder taxonomy; the folders must become the units, the units must become the layers, and
gui_internal.h must dissolve into per-unit headers so the include list at the top of each
unit IS the dependency graph.

## The model (v1.0 of the whole GUI)

There are only three real things:

- RENDER SERVER ("render") -- a 2d batch renderer with a narrow primitive foundation that
  any 2d utility can emit to: UI, HUD, debug overlays, game 2d.  Knows nothing of ids,
  state, or style.  The "draw" unit above it is a LIBRARY of drawing routines (shapes,
  wrappers, text painters) that emit through the render primitives -- server and library
  are separate units so the server surface stays narrow.
- INTERACT SERVER ("core") -- io routing + dedicated retained-mode storage: the id
  namespace, the keyed state pool, retained rect records, the hover/z contest.  Answers
  interact state queries.  Knows nothing of style, themes, or drawing.
- FRAME ORCHESTRATOR ("frame") -- boots both servers, owns viewports/app/sys wiring, pumps
  io into the interact server and hands contexts to the render server, assembles the vtable.

The go-between type is the pane -- fundamentally a z-ordered clip region with an identity:

    typedef struct gui_pane_s
    {
        gui_id_t   id;      // identity: hover attribution, state pool key, draw segment tag
        gui_rect_t rect;    // where it is; hit test + base clip derive from it
        u32        z;       // one number, two consumers: occlusion contest + paint order
        u8         up;      // hosting OS surface (viewport index)
        u8         input;   // input flags (hover, ...)
        // 2 pad bytes spare -- design headroom
    } gui_pane_t;

Everything else is a LIBRARY layered over the two servers: style, interact gestures,
elements, flow, chrome, debug.  Complexity lives in content using the libraries; the kernel
stays fast and simple.

## Target layout

/gui root: one .c per compilation unit + the shared/public headers (gui.h, gui_api.h,
gui_host.h, gui_element.h).  Each unit's implementation lives in its own folder, which also
owns that unit's cross-unit header.

    unit .c (root)     folder        owns header             role
    ---------------    -----------   ---------------------   ------------------------------
    gui_rect.c         rect/         rect/gui_rect.h         leaf: geometry + color primitives
    gui_render.c       render/       render/gui_render.h     RENDER SERVER: batch, fonts, atlas, pipeline (narrow primitive emit)
    gui_draw.c         draw/         draw/gui_draw.h         drawing routines over render primitives: shapes, wrappers, text painters, canvas
    gui_core.c         core/         core/gui_core.h         INTERACT SERVER: io, id, keyed state, pane table, item query, anim
    gui_style.c        style/        style/gui_style.h       interact-state -> color/metric resolution; theme; stacks
    gui_interact.c     interact/     interact/gui_interact.h gesture mechanisms: move/resize/drag/select/feat
    gui_flow.c         flow/         flow/gui_flow.h         layout: rect placement, builder data (rect producer)
    gui_element.c      element/      gui_element.h (root)    component cores over produced rects (interact+render building blocks)
    gui_chrome.c       chrome/       chrome/gui_chrome.h     managed windowing, dock, popup, nav, widgets, table
    gui_debug.c        debug/        debug/gui_debug.h       server introspection (severable)
    gui.c              frame/        (public headers)        FRAME ORCHESTRATOR: boot, viewports, lifecycle, api vtable

## Dependency graph (lowest to highest)

    rect      -> base only            pure geometry + color math
    render    -> rect                 RENDER SERVER; NEVER sees core/style/ids
    draw      -> render, rect         drawing routines emitting through render primitives
    core      -> rect                 INTERACT SERVER; NEVER sees render/draw/style
    style     -> core, rect           state flags in, colors/metrics out; never paints
    interact  -> core, rect           gesture mechanisms; never paints
    flow      -> style, core, rect    rect PRODUCER: layout carves the rects the layers above consume
    element   -> style, draw, core, rect     rect CONSUMER cores; the first layer astride both servers
    chrome    -> everything below     stock recipes: policy over mechanisms (consumes flow's rects with element's cores)
    debug     -> everything           reads both servers' internals; severable
    frame     -> orchestrates all     boots servers, pumps io, flushes render

Hard rules to enforce at carve time:
- render/draw and core never include each other's headers.  The pane handoff (z/clip to
  segment sort) happens in frame.
- the render server surface stays NARROW (push-primitive level); every convenience shape /
  wrapper / text painter belongs in draw, not render.
- style resolves, never emits.  interact decides, never paints.  Gesture feedback paint
  (resize highlight, drop ring, select highlight) belongs to element/chrome.
- upward calls stay explicit and few (the inc-10 discipline): flow -> scrollbar, frame ->
  chrome frame steps + debug windows.

## File mapping (current -> target)

rect/     gui_rect.h (from root) + NEW rect/gui_rect_core.c (col_lerp, align_x/align_y,
          rect_align -- pulled from present/gui_paint_core.c)
render/   backend/pipeline/** + backend/ captures + backend_mem + the ATLAS only from
          backend/resource/ (gui_atlas.c, gui_res_atlas.c) -- the server proper renders from
          an atlas that is PUSHED to it; it does not know what a font is.
          gui_backend.h -> render/gui_render.h; gui_backend.c -> root gui_render.c.
draw/     the drawing-routine library over the render primitives: pure draw wrappers from
          present/gui_paint_core.c (draw_fill/outline, text-fit painters, ellipsis) + the
          parameter-pure shape half of present/gui_symbol.c (arrows, frames, arcs, curves,
          gradients, grips) + user/gui_canvas.c (the user door to 2d drawing) + the FONT and
          ICON resources (backend/resource/ gui_font*.c, gui_icon*.c): glyph metrics and
          baking live above the server, writing into the atlas it hands down.  This also
          gives flow's text-measure need a single home one level up.
core/     core/gui_id.c, gui_io.c, gui_state.c, gui_ctx.c (ambient records + item flags),
          surface/gui_surface.c (pane table, z bands, hover contest), interact/gui_item.c
          (item state machine = THE interact query), user/gui_query.c (query sugar),
          interact/gui_anim.c (dampers/timers are retained-state utilities -> server-side,
          which frees style's animated blends and interact's tweens from a cross-dep),
          label grammar id half (item_id / label_id_str / label_vis_len).
style/    core/gui_style.c, core/gui_theme.c, user/gui_stacks.c, state->color projections
          from paint_core (col_frame_bg, col_item_bg, col_item_bg_anim), COL_* vocabulary,
          metric macros, lattice snapping.
interact/ gui_move.c, gui_resize.c, gui_drag.c, gui_select.c (decision half), gui_feature.c,
          user/gui_behavior.c.
flow/     compose/* (folder rename).
element/  element/gui_element.c + styled paint helpers from paint_core (draw_field_label,
          draw_child_bg/border, draw_window_focus_border, draw_resize_highlight,
          draw_nav_ring, draw_drop_ring) + styled half of gui_symbol.c.
chrome/   widgets/, window/, dock/, popup/, nav/, table/ (moved under chrome/).
debug/    debug/gui_dashboard.c, gui_step_window.c.
frame/    frame/* + debug/gui_frame_overlay.c (lifecycle timing) + gui_api.c + gui_ui_mem.c;
          root gui.c becomes the frame unit.

## gui_internal.h breakup -- DONE early (R1b)

One header per unit, all under gui_internal.h as an ordered umbrella: every unit .c still
includes gui_internal.h once, and under the hood the per-unit headers stack lowest-to-highest
(core -> style -> draw -> interact -> flow -> chrome -> frame -> ctx -> debug).  Each header
holds only what crosses a unit boundary; misplaced entries are marked in place with the
increment that moves them.  core/gui_ctx.h (the context aggregate) deliberately closes the
stack -- gui_context_t embeds records from most units, the entanglement R4 starts shrinking.
Later increments tighten each unit .c to include ONLY the headers at or below its layer
(dropping the umbrella), which is when the compiler starts enforcing the graph per unit.

## Open design tasks (resolved inside their increments)

- R4: re-shape the pane record toward the gui_pane_t sketch above; map the behavior
  services' retained data sets and combine where one shared utility set can serve several
  (dampers, timers, deferred-press, last-rect -- candidates for one keyed record class).
- R5: color id table = core ids (border, background, universal helpers) + user-extended
  range at the end.  SIMPLIFY: resolution stays pure where possible -- interact state
  arrives as PARAMETERS (col_item_bg( st ) style), never queried from core -- so style is
  usable for HUD theming with no interact server present; the one keyed-anim blend
  (col_item_bg_anim) takes its t from the caller or rides core's anim utility explicitly.
- R3/R8: split gui_symbol.c -- parameter-pure shape emitters go to draw; COL_*-consuming
  sugar moves up to element.

## Increment ladder (one per go-ahead, each builds green)

    R1  rect     folder + root unit + move leaf math out of paint_core        <- DONE
    R2  render   backend/ -> render/; gui_backend.h -> render/gui_render.h; gui_backend.c -> gui_render.c   <- DONE
    R3  draw     NEW drawing-routine unit: pure wrappers + shape half of gui_symbol.c + canvas   <- DONE
    R4  core     interact server assembly; core/gui_core.h carved from gui_internal.h; pane re-shape
    R5  style    style/ folder; style/gui_style.h takes the COL_/metric vocabulary
    R6  interact gesture unit gui_interact.c; paint halves pushed up
    R7  flow     compose/ -> flow/; unit .c to root; flow/gui_flow.h (rect producer sits below element)
    R8  element  absorb styled paint helpers; unit .c to root
    R9  chrome   six folders under chrome/; chrome/gui_chrome.h
    R10 debug    unit .c to root; debug/gui_debug.h; frame_overlay -> frame/
    R11 frame    gui.c = pure orchestrator; gui_internal.h DELETED; GUI_ARCHITECTURE.md rewrite

Method per increment (proved in inc 10): create/move the unit + orb.targets lines (gui AND
gui_stress) + -gen; build; let the compiler enumerate the seams; place each decl in its
owner's header; repeat until green; canary sb_gui_example + host_editor.
