# GUI SERVER PLAN -- the v2 unit reorganization

Status: R1 (rect) + R1b (header split) + R2 (render) + R3 (draw) + R4 (core) + R5 (style) + R6 (interact) + R7 (flow) + R8 (element) + R9 (chrome) + R10 (debug) DONE 2026-07-21 -- next: R11 (frame, the finale)

R10 DONE: the DEBUG UNIT is re-seated -- debug/gui_debug.c -> root gui_debug.c (the last
carved unit joins the root like every other), and debug/gui_frame_overlay.c ->
frame/gui_frame_overlay.c (its own banner already said it: conductor code the lifecycle
calls, never severable tooling -- now the folder says it too).  Constituents unchanged
(debug/gui_dashboard.c + gui_step_window.c); debug/gui_debug.h stays the unit's cross-unit
header (the DBG_*/STEP_SET_OWNER umbrella blocks live there since R4).  orb.targets unit
lines updated (gui + gui_stress) + -gen; path sweep was small (8 files: gui.c banner +
include, the two moved banners, chrome/io/render.h/ui_mem/ARCHITECTURE references).  No
seam changes, no code motion between units.  Full + mono builds clean (first try); both
canaries clean.

R9 DONE: the CHROME UNIT's six folders folded under chrome/ -- widgets/, table/, window/,
dock/, popup/, nav/ are now chrome/widgets/ ... chrome/nav/, beside the unit header
chrome/gui_chrome.h that has lived there since R1b.  Purely mechanical: no code moved between
units, no seams changed; the unit root gui_chrome.c stayed at the gui root (orb.targets
untouched), its constituent include paths and every path reference repo-wide re-pointed
(35 files; prose role references like "window/ policy" kept -- the folders still exist, one
level down).  The gui root now shows exactly the target layout's shape: one folder per unit
plus the root unit .c files and public headers.  NOTE discovered en route: 13 gui sources
carry pre-existing UTF-8 BOMs (violating the ASCII rule byte-wise; MSVC accepts them) --
left untouched, a separate hygiene pass if wanted.  Full + mono builds clean (first try);
both canaries clean.

R8 DONE: the ELEMENT UNIT is complete (root gui_element.c) -- THE FIRST LAYER ASTRIDE BOTH
SERVERS: everything that combines interact state with styled paint over a caller rect.  Three
constituents: element/gui_element_core.c (the el_* cores, renamed from element/gui_element.c
on the rect_core precedent to free the root name -- contract unchanged: fill exactly the rect,
read only the installed element style), element/gui_adornment.c (the old
present/gui_paint_core.c, moved whole -- the impure per-item wrappers item_flags_resolve /
item_flags_chrome_reset, draw_field_label + label_natural_w (the WIDGET_PAD self-measure, up
from draw/gui_paint.c), and the system adornments: nav ring, focus border, drop ring, child
box pair, resize highlight), and NEW element/gui_symbol_style.c (the styled half of
draw/gui_symbol.c: draw_arrow, draw_collapse_arrow, draw_close_x, draw_check_indicator,
draw_rule, draw_frame -- every emitter that resolves its own look from a style-var pick /
WIN_BORDER / checkmark_pad / ROUND_WIDGET; their public wrappers gui_draw_arrow /
gui_draw_close / gui_draw_frame moved with them so draw never calls upward; the chevron
variant routes back through the public gui_draw_chevron; the file-local dashed forwarder died
-- draw_rule strokes gui_draw_dashed_line, the backend primitive, directly).  The
gui_set_check/bullet/arrow_style setters write the active style record, so they went to the
STYLE unit (style/gui_stacks.c), not element.  present/ is DISSOLVED; the draw unit is now
parameter-pure (no style_var / s_style / vocabulary-macro reads).  NEW
element/gui_element_internal.h joins the umbrella between flow and chrome -- the unit's
cross-boundary decls (wrappers, styled painters, mem seam); the adornments invoked from below
stay declared in their consumer's documented upward-seam blocks (draw_nav_ring in
core/gui_core.h, draw_drop_ring in interact/gui_interact.h), and the element bridge
(el_style_derive, g_gui_el_slot_map) stays in style/gui_style.h -- one decl, one home.  The
element TU now includes render/gui_render.h (it paints -- astride is the unit's definition).
NEW gui_element_unit_mem_bytes() = s_el_style + g_gui_el_slot_map, wired into gui_ui_memory.
Full + mono builds clean (one trivial fix: the missing render header); both canaries clean.

R7 DONE: the FLOW UNIT is re-seated (compose/ -> flow/; compose/gui_flow.c -> root
gui_flow.c) -- the folder now matches the unit, joining the R1b header (flow/gui_flow.h,
real since this increment).  Constituents unchanged: gui_layout_core.c, gui_scroll.c,
gui_layout_child.c, gui_sublayout.c, gui_split.c, gui_region.c, gui_layout.c.  The carve was
purely mechanical (the TU existed since the predecessor campaign); the increment's work was
the DEP AUDIT now recorded in the banners: downward, flow composes over core (frames, keyed
state, anim ease, io), style (the spacing metrics), interact (resize_apply_edges -- child
edge-resize rides the window mechanism), and the render clip stack -- flow computes THE view
rect, so it owns the region scissor lifecycle (draw_push/pop_clip_rect) + the matching
s_scope.clip fence + the GUI_DBG_REGION outline; no other render call belongs there (flow
places, it does not paint).  UPWARD SEAMS formalized in flow/gui_flow.h ("do not add more"):
scrollbar_widget (the region gutter's ONE widget -- the plan's sanctioned flow -> chrome
call) and the child box paint trio (draw_child_bg / draw_child_border /
draw_resize_highlight, declared in draw/gui_draw.h, defined in paint_core, bound for element
at R8).  The old banner's second upward seam (gui_anim_f32) is downward since the anim
utilities moved to core at R4.  gui_flow_unit_mem_bytes unchanged (s_layout_state_stack +
s_split_stack + s_sublayout_sink; the layout-frame stack stays a core static).  Full + mono
builds clean (first try); both canaries clean.

R6 DONE: the INTERACT UNIT exists (root gui_interact.c) -- gesture mechanisms over the
interact server: interact/gui_move.c + gui_resize.c + gui_drag.c + gui_feature.c +
gui_behavior.c (from user/ -- the user/ folder is now DISSOLVED: canvas -> draw R3, query ->
core R4, stacks -> style R5, behavior -> interact R6).  ACCEPTANCE: no render header; the
documented upward seams live in interact/gui_interact.h mirroring core's block --
draw_drop_ring (un-static'd in paint_core; the ONE adornment paint, invoked where the accept
is decided, like core's draw_nav_ring), the drag preview tooltip through public chrome verbs,
and resize's WIN_BORDER read (geometry, not paint).  New cross-TU seams: drag_new_frame
(frame_begin drives it) + move_grab_offset (viewport tear-off reads it) un-static'd.
SELECT RE-CLASSIFIED (the R6 design resolution): the plan mapped "gui_select.c (decision
half)" to interact, but the audit shows the decision half is inseparable from server
crossings interact must never make -- the press/sweep protocol reads the render server's run
capture (select_capture_*, select_run) and measures with draw-unit font metrics
(font_char_advance).  Window text selection is CHROME: policy astride both servers, riding
the generic core verbs.  The whole controller moved to window/gui_select.c (chrome TU,
included before the window files; decls -> chrome/gui_chrome.h), and its two raw
s_interaction writes were re-seated onto NEW core verb interact_claim( id, button )
(core/gui_item.c, beside interact_held/idle) -- preserving the rule that core/ + interact/
are the only raw writers of the arbitration fields.  Memory accounting: NEW
gui_interact_unit_mem_bytes() (= s_drag; the gesture latches are scalar statics, uncounted
by contract); s_select now counted in gui_chrome_unit_mem_bytes.  Full + mono builds clean
(first try); both canaries clean.

R5 DONE: the STYLE UNIT exists (root gui_style.c) -- the first library over the interact
server: state flags in, colors/metrics out, never paints.  Constituents: style/gui_theme.c
(from core/ -- theme registry, s_style_base/s_style, theme API, lattice, layout_compute),
style/gui_style_core.c (from core/gui_style.c, renamed on the rect/gui_rect_core.c precedent
to free the root unit name -- slot space, push/pop/next stacks, style_el_col, seam hooks),
style/gui_stacks.c (from user/ WHOLE, per the mapping -- the cross-cut is banner-documented:
id/item-flag brackets forward DOWN to core seams, the style/scale brackets to this unit's own
statics, which therefore stay static).  The state -> color projections (col_frame_bg,
col_item_bg, col_item_bg_anim + ANIM_TAG_BG) moved from present/gui_paint_core.c into
gui_style_core.c.  ACCEPTANCE: the unit includes no render header and calls no draw_*
routine; the impure wrappers (item_flags_resolve / item_flags_chrome_reset) deliberately
STAY in present/gui_paint_core.c (they apply draw-state consequences; -> element at R8).
PURITY (the R5 design task): all three projections take interact state as PARAMETERS;
col_item_bg_anim alone rides core's keyed anim utility explicitly (the sanctioned exception).
COLOR TABLE: gui_col_t gained the user-extended range GUI_COL_USER_0..7 (+ GUI_COL_USER_COUNT)
at the end -- engine code never reads them, themes leave them zero -- with the NEW public
resolved read style_color( slot ) (gui_style_color, vtable add => func_api_size grew, host
restart on hot-reload) as the door: kits seed a user slot and paint custom drawing through
the same theme + stack resolution as stock chrome.  New cross-TU seams (style/gui_style.h):
style_item_commit / style_chrome_reset (driven by paint_core), style_new_frame (driven by
gui_ctx_begin + gui_theme_reset), layout_compute (driven by gui_style_apply, which stays
frame -- the rescale reads font metrics style must not touch; documented upward seam, as is
style_el_col's read of the installed element style), gui_style_unit_mem_bytes (gui_ui_mem
seam).  Fixed en route: sb_gui's k_col_names was missing GUI_COL_NAV_CAPTURE (NULL label row
in the style editor); both sandbox name tables gained the user range.  Full + mono builds
clean (first try); both canaries clean.

R4 DONE: the INTERACT SERVER unit exists (root gui_core.c): core/gui_io.c + gui_ctx.c +
gui_id.c + gui_state.c + gui_surface.c (from surface/, folder removed) + gui_item.c +
gui_anim.c (from interact/) + gui_query.c (from user/), with a gui_core_unit_mem_bytes()
foot.  The server includes NO render header -- the acceptance criterion holds; its documented
upward seams (gui_core.h): draw_nav_ring (the one adornment paint) + the DBG_*/STEP_SET_OWNER
debug stamps.  Carve moves that made that true:
  - PANE BRACKET -> frame/gui_pane.c: pane_tag + gui_pane_begin/end stamp BOTH servers (draw
    segment key + interaction scope), so the go-between verb lives with the orchestrator;
    core keeps the interact half (surface_hover_nominate, pool, z dispenser).
  - item_flags split: core keeps the PURE halves (item_flags_take / item_flags_chrome_drop);
    the style/draw application wrappers keep the old names in present/gui_paint_core.c
    (declared in style/gui_style.h; placement refined R5/R8).
  - ctx_new_frame no longer calls style_new_frame -- gui_ctx_begin (frame) pairs the two.
  - gui_mem_stats/print -> gui_ui_mem.c (aggregates both servers); gui_ctx_create/destroy/
    bind/set_listening -> frame/gui_context.c (destroy tears down GPU surfaces); the pool
    storage + ctx_alloc_slot/ctx_bind stay core (externs in gui_ctx.h).
  - label grammar id half (label_vis_len/label_id_str/item_id) -> core/gui_id.c.
  - DBG_* overlay macro block + the GUI_CMD_STEPPER switch + STEP_SET_OWNER moved from
    render/gui_render.h -> debug/gui_debug.h (cross-server debug tooling reaches every unit
    via the umbrella; implementations stay render-side).
  - New cross-TU seams: io_frame_begin/end, io_dirty, ctx_pool_init/ctx_bind/ctx_new_frame,
    interaction_frame_reset, cursor_flush, item_flag_push/pop/next, gui_state_usage(_t),
    gui_anim_timer_t, gui_api_anim_* adapters, s_viewport_dirty extern,
    gui_owned_window_event un-static'd (io's one upward call into frame).
PANE RE-SHAPE: gui_pane_t (gui.h) already matches the model sketch -- id + rect + z(u32) +
u8 vp + u8 input, 2 pad bytes spare (field named `vp`, the sketch's `up`).  No change needed.
BEHAVIOR DATA AUDIT (the R4 design task): keyed-by-id retained data (anim dampers/timers,
feat_collapse/feat_pin, region scroll, open flags, table persist) already share ONE utility
set -- the keyed state pool's tiny/small/big classes; R4 moved its anim tenants server-side
so style blends and interact tweens rent from the same pool.  The single-user gesture latches
(move offset, press-defer, drag payload, select controller, vp request, next-window channel)
are deliberately AMBIENT SINGULAR records, not pool tenants: one mouse means at most one
gesture in flight, so a keyed table buys capacity nothing needs.  Conclusion: no further
consolidation warranted; rule recorded as "keyed-by-id rides the pool, one-user gesture state
stays a single ambient record."
GOTCHA cleared: stale pre-R2 gui_backend.obj files in build/obj/gui{,_stress}/ were still
being swept into the libs (LNK4006 spam) -- deleted; a rename that retires a unit must also
delete its old obj.  Full build + mono build clean; both canaries clean.

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

- R4 (RESOLVED): the pane record already matches the sketch; the behavior data audit found
  the shared keyed utility set already exists (the state pool's three classes) and the
  one-user gesture latches are correctly ambient singular records -- see the R4 DONE block.
- R5 (RESOLVED): the color id table is core ids + GUI_COL_USER_0..7 at the end, read through
  the new public style_color(); resolution is pure -- state arrives as parameters, and
  col_item_bg_anim rides core's anim utility explicitly -- see the R5 DONE block.
- R3/R8 (RESOLVED): gui_symbol.c split at R8 -- the parameter-pure emitters stayed in draw;
  the emitters that resolve their own look moved to element/gui_symbol_style.c, and the
  style SETTERS at the foot went to the style unit -- see the R8 DONE block.

## Increment ladder (one per go-ahead, each builds green)

    R1  rect     folder + root unit + move leaf math out of paint_core        <- DONE
    R2  render   backend/ -> render/; gui_backend.h -> render/gui_render.h; gui_backend.c -> gui_render.c   <- DONE
    R3  draw     NEW drawing-routine unit: pure wrappers + shape half of gui_symbol.c + canvas   <- DONE
    R4  core     interact server assembly; pane bracket -> frame; behavior data audit        <- DONE
    R5  style    style/ folder + root unit; projections in; user color range        <- DONE
    R6  interact gesture unit; select re-classified as chrome; interact_claim   <- DONE
    R7  flow     compose/ -> flow/; unit .c to root; flow/gui_flow.h (rect producer sits below element)   <- DONE
    R8  element  absorb styled paint helpers; unit .c to root   <- DONE
    R9  chrome   six folders under chrome/; chrome/gui_chrome.h   <- DONE
    R10 debug    unit .c to root; debug/gui_debug.h; frame_overlay -> frame/   <- DONE
    R11 frame    gui.c = pure orchestrator; gui_internal.h DELETED; GUI_ARCHITECTURE.md rewrite

Method per increment (proved in inc 10): create/move the unit + orb.targets lines (gui AND
gui_stress) + -gen; build; let the compiler enumerate the seams; place each decl in its
owner's header; repeat until green; canary sb_gui_example + host_editor.
